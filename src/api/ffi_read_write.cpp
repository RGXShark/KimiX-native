/*
 * ffi_read_write.cpp -- implementation of the file read/write C FFI
 * (api/ffi_read_write.h).
 *
 * THE THREE OBJECTS
 * -----------------
 *   kimix_rw_stream   wraps kimix::BinaryFileStream   (core/binary_file_stream.h)
 *   kimix_rw_writer   wraps kimix::BinaryFileWriter   (core/binary_file_writer.h)
 *   kimix_rw_context  the DEFERRED CONTEXT: recorded commands plus every
 *                     temporary buffer they need, owned by the context.
 *
 * The first two are thin and deliberately faithful to the C++ classes: the same
 * non-throwing contract ("an unopenable path leaves the object closed, and every
 * later call reports the neutral value"), the same all-or-nothing write, and the
 * same platform limits -- except that this layer turns the classes' silent
 * closed state into an explicit KIMIX_ERR_NOT_FOUND at open time, because a
 * foreign caller has no C++ object to inspect afterwards and needs to know
 * whether the path worked before it starts a pipeline.
 *
 * WHY THE HANDLES ARE ALLOCATED HERE AND NOT EMBEDDED IN CALLER MEMORY
 * --------------------------------------------------------------------
 * Unlike kimix_vec (a caller-owned placeholder with a guard word), these objects
 * own an OS handle -- a FILE* / an xnvme device -- and their C++ classes are
 * neither copyable nor default-constructible-after-open.  A caller-provided
 * placeholder would have to be exactly as large and aligned as the C++ object,
 * which the ABI promises nowhere, so the handle is a pointer to memory this
 * library allocates on its own mimalloc heap and releases in the matching
 * close/free call.  One heap, and nothing internal escapes (rule 4 of
 * api/ffi_common.h).
 *
 * THE DEFERRED CONTEXT -- MEMORY LIFECYCLE
 * ----------------------------------------
 * The context is the reason this area exists beyond a thin wrapper.  A batch
 * needs buffers that live from the RECORD call to the COMMIT: the destination of
 * a read, the payload of a write, the staging buffer of a copy.  A managed
 * caller cannot pin its own buffers across that window, so the context allocates
 * and owns them:
 *
 *   add_read()   allocates the destination (mi_zalloc -- zero-filled, so a slot
 *                that ends up holding nothing never leaks heap content).
 *   add_write()  copies the caller's bytes into a library allocation, so the
 *                caller's buffer is free the moment the call returns.
 *   add_copy()   allocates the staging buffer for a source-to-destination copy.
 *
 * Every one of those allocations is counted in the context's ledger
 * (owned_bytes / owned_blocks, published by kimix_rw_context_owned_*() and by
 * the stats of a commit).  The ledger is not bookkeeping for its own sake: it is
 * the observable half of the automatic release, and the test suite asserts on it.
 *
 * commit() releases, with no free call from the caller:
 *   * the write payloads, once their bytes reached the file and it was synced
 *     (the "auto free after the commit's disk write" the header promises);
 *   * the copy staging buffers, right after they were written out;
 *   * nothing of a read slot -- its bytes ARE the result.  Those are released by
 *     take_read() (ownership moves to the caller), by clear() or by free().
 *
 * A local scope owns a scope's memory in C++, and the class destructors do the
 * releasing; here the context is that owner, and destroy()/clear()/commit() are
 * its destructor/ranges.
 *
 * COMMIT ORDER (and why it is fixed)
 * ----------------------------------
 *   1. pending reads, grouped by path  -- one open/read/close per file;
 *   2. the read half of every copy, grouped by source path;
 *   3. the writes: recorded writes + the copies whose read landed fully, grouped
 *      by destination path, each file synced and closed after its writes.
 * Reads run before every write, so a commit that reads and writes the same file
 * is well defined, and a commit never mutates a file whose content another read
 * in the same batch still needs.
 *
 * PER-COMMAND RESULT, NOT AN AGGREGATE
 * ------------------------------------
 * The batching itself is done here rather than by handing the commands to
 * kimix::DeferredBinaryFileStream / kimix::DeferredBinaryWriter because those
 * two report a COUNT of fully completed reads / writes -- enough for a C++
 * caller that can retry, but not enough for an ABI whose caller has to know WHICH
 * of its 500 recorded commands failed.  Grouping by path (the expensive part:
 * one open per file) is exactly what they do; issuing each command individually
 * through the grouped stream/writer keeps the per-command byte count available,
 * and is not slower in practice -- tests/unit/core/bench_binary_file_stream.cpp
 * measures the batched read path at ~85-87 % of the same reads issued one by one.
 *
 * UNITY BUILD
 * -----------
 * Same rule as the other src/api translation units: the build merges these files
 * (cxx batch_size 8), so every file-local helper is `rw_`-prefixed inside an
 * anonymous namespace and the only global names added are the ones the header
 * declares (plus the three handle structs it declares as incomplete types).  The
 * command records are nested inside kimix_rw_context precisely so they cannot
 * collide with anything another area invents.
 */
#include <api/detail.h>
#include <api/ffi_read_write.h>
#include <core/binary_file_stream.h>
#include <core/binary_file_writer.h>
#include <core/stl/pdqsort.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>
#include <mimalloc.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

/* ---------------------------------------------------------------------------
 * Why read_all() is re-expressed here instead of being called
 * -------------------------------------------------------------------------
 * kimix::BinaryFileStream::read_all() returns a kimix::vector<kimix::byte>
 * (= vector<int8_t>), while the ABI's byte buffer is a vector<std::byte>.  The
 * two are the same std::vector specialization over the same allocator with
 * 1-byte elements, and api/detail.h's vec_construct_move_from_char() bridge is
 * the established way to move one into a kimix_vec -- for kimix::repair(), whose
 * result is produced by an OUT-OF-LINE call the compiler cannot see into.
 *
 * Doing the same with read_all() is measurably broken: read_all() is
 * header-inline, so the source vector is built in THIS translation unit, and the
 * move that reinterprets it as a vector<std::byte> is a type-punning move the
 * optimizer is entitled to ignore (the two vector types are unrelated, so a
 * store through the reinterpreted glvalue may not be assumed to change the
 * source objects).  Measured with gcc 16 -O2: the source kept its buffer and its
 * destructor freed it, handing the placeholder a dangling vector -- the whole
 * file read lost, silently (MSVC happened to do the intuitive thing, which is
 * exactly the kind of mismatch a cross-platform ABI must not rely on).
 *
 * So this file reads into the destination ELEMENT TYPE directly: one seek, one
 * read straight into the placeholder's buffer, one resize to the bytes that
 * landed.  That is read_all()'s behaviour step for step (core's own:
 * vector(length) -> fseek(0) -> fread -> resize(got)), it performs the same
 * single copy out of the OS, and nothing is ever reinterpreted.  The
 * static_assert below pins the length type the resize uses.
 * ------------------------------------------------------------------------- */
static_assert(sizeof(kimix::api::byte_vector::value_type) == 1,
              "the whole-file read counts bytes, so one element must be one byte");
static_assert(kimix::api::k_vec_guard_offset >= sizeof(kimix::api::byte_vector),
              "the guard slot would overlap the vector kimix_rw_stream_read_all() constructs");

/* ---------------------------------------------------------------------------
 * The objects behind the opaque handles
 * ------------------------------------------------------------------------- */

/* kimix::BinaryFileStream is header-inline ON PURPOSE (see its header): a FILE*
 * must be created and consumed inside one module.  Here that module is
 * kimix_api.dll -- fopen/fread/fclose and read_all()'s allocation all happen
 * inside this image, and the bytes it hands out come from the shared mimalloc
 * heap kimix_mem_free() releases (MI_SHARED_LIB is exported by kimix-core). */
struct kimix_rw_stream {
    kimix::BinaryFileStream stream;
};

/* kimix::BinaryFileWriter is a compiled, KIMIX_CORE_API class: its xnvme calls
 * (and xnvme's process-global state) live in kimix-core, and this file only
 * calls the imported methods. */
struct kimix_rw_writer {
    kimix::BinaryFileWriter writer;
};

/* The deferred context.  Nested command records: no other translation unit can
 * name them (unity build), and they are plain aggregates without destructors, so
 * moving a command on a vector growth moves its buffer POINTER and nothing else
 * -- the owning side is always this struct, which releases explicitly. */
struct kimix_rw_context {
    /* One recorded read.  'buffer' is the library allocation the bytes land in
     * (NULL exactly when size == 0), 'got' what the last commit delivered, and
     * 'executed'/'taken' the slot's state: not run yet -> run (context owns the
     * bytes) -> taken (the caller owns them). */
    struct read_command {
        kimix::string path;
        uint64_t offset;
        size_t size;
        void *buffer;
        size_t got;
        bool executed;
        bool taken;
    };

    /* One recorded write.  'payload' is a library copy of the caller's bytes and
     * is released by the commit that wrote it (or by clear()/free()). */
    struct write_command {
        kimix::string path;
        uint64_t offset;
        size_t size;
        void *payload;
        bool completed;
    };

    /* One recorded copy.  'buffer' is the staging allocation, released by the
     * commit that consumed it -- whatever the outcome: after the disk write when
     * it landed, immediately when the read already failed. */
    struct copy_command {
        kimix::string src_path;
        kimix::string dst_path;
        uint64_t src_offset;
        uint64_t dst_offset;
        size_t size;
        void *buffer;
        bool read;      /* the read half delivered every byte */
        bool completed; /* read + written */
    };

    kimix::vector<read_command> reads;
    kimix::vector<write_command> writes;
    kimix::vector<copy_command> copies;

    /* The ledger: every live allocation this context owns.  Kept in step with the
     * allocations themselves (rw_alloc / rw_release), which is what makes
     * "commit released it" an observable fact rather than a claim. */
    size_t owned_bytes = 0;
    size_t owned_blocks = 0;
};

namespace {

/* ---------------------------------------------------------------------------
 * The context ledger
 * ------------------------------------------------------------------------- */

/* A zero-filled block from the library heap, counted in the ledger.  NULL (the
 * allocation failed, or size == 0 -- no block is needed for an empty command)
 * leaves the ledger untouched, which keeps alloc/release exactly paired. */
void *rw_alloc(kimix_rw_context *ctx, size_t size) noexcept {
    if (size == 0) {
        return nullptr;
    }
    void *block = mi_zalloc(size); // zero-filled: a slot that stays empty leaks nothing
    if (!block) {
        return nullptr;
    }
    ctx->owned_bytes += size;
    ctx->owned_blocks += 1;
    return block;
}

/* Takes a block out of the ledger WITHOUT releasing it: the ownership moved to
 * someone else (kimix_rw_context_take_read()), so this context must stop counting
 * it but must not free it.  The mirror image of rw_alloc() for a transfer. */
void rw_forget(kimix_rw_context *ctx, void *block, size_t size) noexcept {
    if (!block) {
        return;
    }
    ctx->owned_bytes -= size;
    ctx->owned_blocks -= 1;
}

/* Gives a block back and takes it out of the ledger.  NULL is a no-op, so the
 * release paths never need to ask whether size was 0. */
void rw_release(kimix_rw_context *ctx, void *block, size_t size) noexcept {
    if (!block) {
        return;
    }
    rw_forget(ctx, block, size);
    mi_free(block);
}

/* ---------------------------------------------------------------------------
 * The reader's 32-bit position limit
 * ------------------------------------------------------------------------- */

/* kimix::BinaryFileStream seeks with a `long` (32 bits on Windows / LLP64).  An
 * offset it cannot express is REFUSED, never truncated: a truncated seek would
 * silently read the wrong bytes, which is the one failure mode a caller cannot
 * detect from the byte count. */
bool rw_offset_seekable(uint64_t offset) noexcept {
    return offset <= static_cast<uint64_t>(LONG_MAX);
}

/* ---------------------------------------------------------------------------
 * Grouping by path
 * ------------------------------------------------------------------------- */

/* A command's file, used to sort commands so every file is opened once.  The
 * path is a POINTER into the command list, which is only read during a commit;
 * 'serial' is the record order and is what keeps commands that share a path in
 * the order they were recorded (a plain unstable sort by path would not, and for
 * overlapping writes into one file the order is part of the semantics). */
struct rw_group_key {
    const kimix::string *path;
    size_t index;
    size_t serial;
};

void rw_sort_groups(kimix::vector<rw_group_key> &keys) {
    kimix::sort(keys.begin(), keys.end(), [](const rw_group_key &lhs, const rw_group_key &rhs) {
        if (*lhs.path != *rhs.path) {
            return *lhs.path < *rhs.path;
        }
        return lhs.serial < rhs.serial;
    });
}

/* One write to be issued, from either command list.  'index' points back at the
 * command so its result can be recorded; 'serial' is the position in the job
 * list, i.e. the record order within a path. */
struct rw_write_job {
    const kimix::string *path;
    uint64_t offset;
    const void *data;
    size_t size;
    size_t index;
    size_t serial;
    bool is_copy;
};

} // namespace

KIMIX_FFI_BEGIN

// ===========================================================================
// 1. Reader -- kimix::BinaryFileStream
// ===========================================================================

kimix_status kimix_rw_stream_open(const char *path, kimix_rw_stream **out) {
    if (!path || !out) {
        return KIMIX_ERR_INVALID_ARG;
    }
    /* Allocate first, then construct in place: the handle is created only when a
     * file could be opened, so a caller never has to free a failed open. */
    auto *block = static_cast<kimix_rw_stream *>(mi_malloc_aligned(sizeof(kimix_rw_stream),
                                                                  alignof(kimix_rw_stream)));
    if (!block) {
        return KIMIX_ERR_OUT_OF_MEMORY;
    }
    ::new (block) kimix_rw_stream{kimix::BinaryFileStream(kimix::string(path))};
    if (!block->stream.is_open()) {
        /* The C++ class has no way to say WHY fopen failed (missing file, a
         * directory, permission, an over-long path): all of it is "not found"
         * from here, which is the one thing a caller can act on. */
        block->~kimix_rw_stream();
        mi_free(block);
        return KIMIX_ERR_NOT_FOUND;
    }
    *out = block; // written only on success
    return KIMIX_OK;
}

void kimix_rw_stream_close(kimix_rw_stream *stream) {
    if (stream) {
        stream->stream.close();
    }
}

void kimix_rw_stream_free(kimix_rw_stream *stream) {
    if (stream) {
        stream->~kimix_rw_stream(); // closes the FILE* through the member's destructor
        mi_free(stream);
    }
}

bool kimix_rw_stream_is_open(const kimix_rw_stream *stream) {
    return stream && stream->stream.is_open();
}

uint64_t kimix_rw_stream_length(const kimix_rw_stream *stream) {
    return stream ? static_cast<uint64_t>(stream->stream.length()) : 0;
}

uint64_t kimix_rw_stream_position(const kimix_rw_stream *stream) {
    if (!stream) {
        return 0;
    }
    /* ftell() returns -1L on error; the caller sees 0 (the start of the file),
     * which is the neutral value this area documents. */
    long const pos = stream->stream.position();
    return pos > 0 ? static_cast<uint64_t>(pos) : 0;
}

kimix_status kimix_rw_stream_set_pos(kimix_rw_stream *stream, uint64_t pos) {
    if (!stream) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (!stream->stream.is_open()) {
        return KIMIX_ERR_INVALID_STATE;
    }
    if (!rw_offset_seekable(pos)) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    stream->stream.set_pos(static_cast<long>(pos));
    return KIMIX_OK;
}

size_t kimix_rw_stream_read(kimix_rw_stream *stream, void *buffer, size_t size) {
    /* A 0-byte read is a no-op with a defined answer, and fread(NULL, 1, 0, f) is
     * not something to rely on: answer 0 here without touching the file. */
    if (!stream || !buffer || size == 0) {
        return 0;
    }
    return stream->stream.read(buffer, size);
}

size_t kimix_rw_stream_read_at(kimix_rw_stream *stream, uint64_t pos, void *buffer, size_t size) {
    if (!stream || !buffer || size == 0) {
        return 0;
    }
    if (!stream->stream.is_open() || !rw_offset_seekable(pos)) {
        return 0;
    }
    stream->stream.set_pos(static_cast<long>(pos));
    return stream->stream.read(buffer, size);
}

kimix_status kimix_rw_stream_read_all(kimix_rw_stream *stream, kimix_vec *out) {
    if (!stream || !out) {
        return KIMIX_ERR_INVALID_ARG;
    }
    /* INIT semantics, like kimix_repair(): a live placeholder would be leaked by
     * overwriting it, so that is an error, not an assignment. */
    if (!kimix::api::vec_is_raw_storage(out)) {
        return KIMIX_ERR_INVALID_STATE;
    }
    if (!stream->stream.is_open()) {
        return KIMIX_ERR_INVALID_STATE;
    }
    /* One seek, one read into the placeholder's own element type, one resize to
     * what landed -- read_all()'s behaviour without the type-punned move (see the
     * long comment at the top of this file). */
    kimix::api::byte_vector data;
    stream->stream.set_pos(0);
    size_t const length = stream->stream.length();
    if (length != 0) {
        data.resize(length);
        size_t const got = stream->stream.read(data.data(), data.size());
        data.resize(got); // a short count at the tail is a smaller result, not an error
    }
    kimix::api::vec_construct_move(out, std::move(data));
    return KIMIX_OK;
}

kimix_status kimix_rw_read_file(const char *path, kimix_vec *out) {
    if (!path || !out) {
        return KIMIX_ERR_INVALID_ARG;
    }
    /* Validate the placeholder before opening anything: the file must not be
     * touched by a call that is going to fail anyway. */
    if (!kimix::api::vec_is_raw_storage(out)) {
        return KIMIX_ERR_INVALID_STATE;
    }

    kimix_rw_stream *stream = nullptr;
    kimix_status const opened = kimix_rw_stream_open(path, &stream);
    if (opened != KIMIX_OK) {
        return opened; // NOT_FOUND / INVALID_ARG / OUT_OF_MEMORY, nothing to close
    }
    kimix_status const read = kimix_rw_stream_read_all(stream, out);
    kimix_rw_stream_free(stream); // opened here, closed here, whatever happened
    return read;
}

// ===========================================================================
// 2. Writer -- kimix::BinaryFileWriter
// ===========================================================================

kimix_status kimix_rw_writer_open(const char *path, bool truncate, kimix_rw_writer **out) {
    if (!path || !out) {
        return KIMIX_ERR_INVALID_ARG;
    }
    kimix::string const full(path);
    if (full.empty()) {
        /* xnvme would reject it too; reporting the caller's argument is clearer
         * than reporting "could not open an empty name". */
        return KIMIX_ERR_INVALID_ARG;
    }
    auto *block = static_cast<kimix_rw_writer *>(mi_malloc_aligned(sizeof(kimix_rw_writer),
                                                                  alignof(kimix_rw_writer)));
    if (!block) {
        return KIMIX_ERR_OUT_OF_MEMORY;
    }
    ::new (block) kimix_rw_writer{kimix::BinaryFileWriter(full, truncate)};
    if (!block->writer.is_open()) {
        /* Missing directory, permission, a Windows 2 GiB+ file, or a platform
         * with no wired xnvme backend -- all indistinguishable from here. */
        block->~kimix_rw_writer();
        mi_free(block);
        return KIMIX_ERR_NOT_FOUND;
    }
    *out = block; // written only on success
    return KIMIX_OK;
}

void kimix_rw_writer_close(kimix_rw_writer *writer) {
    if (writer) {
        writer->writer.close();
    }
}

void kimix_rw_writer_free(kimix_rw_writer *writer) {
    if (writer) {
        writer->~kimix_rw_writer(); // closes the device through the member's destructor
        mi_free(writer);
    }
}

bool kimix_rw_writer_is_open(const kimix_rw_writer *writer) {
    return writer && writer->writer.is_open();
}

uint64_t kimix_rw_writer_position(const kimix_rw_writer *writer) {
    return writer ? writer->writer.position() : 0;
}

void kimix_rw_writer_set_pos(kimix_rw_writer *writer, uint64_t pos) {
    if (writer) {
        writer->writer.set_pos(pos);
    }
}

size_t kimix_rw_writer_write(kimix_rw_writer *writer, const void *data, size_t size) {
    /* A 0-byte write is a successful no-op in this area (the C++ class reports 0
     * for it, which would read as a failure through this return value). */
    if (size == 0) {
        return 0;
    }
    if (!writer || !data) {
        return 0;
    }
    return writer->writer.write(data, size);
}

size_t kimix_rw_writer_write_at(kimix_rw_writer *writer, uint64_t pos, const void *data,
                                size_t size) {
    if (size == 0) {
        return 0;
    }
    if (!writer || !data) {
        return 0;
    }
    writer->writer.set_pos(pos);
    return writer->writer.write(data, size);
}

bool kimix_rw_writer_sync(kimix_rw_writer *writer) {
    return writer && writer->writer.sync();
}

kimix_status kimix_rw_write_file(const char *path, const void *data, size_t size, bool truncate) {
    if (!path || !*path) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (size != 0 && !data) {
        return KIMIX_ERR_INVALID_ARG;
    }

    kimix_rw_writer *writer = nullptr;
    kimix_status const opened = kimix_rw_writer_open(path, truncate, &writer);
    if (opened != KIMIX_OK) {
        return opened; // NOT_FOUND / INVALID_ARG / OUT_OF_MEMORY, nothing to free
    }

    /* write() returns `size` or 0 (all-or-nothing), and the 0-byte case returns 0
     * as well -- so `written != size` is the failure test for both. */
    size_t const written = kimix_rw_writer_write(writer, data, size);
    bool const synced = kimix_rw_writer_sync(writer);
    kimix_rw_writer_free(writer); // opened here, closed here, whatever happened

    if (written != size) {
        return KIMIX_ERR_FAILED; // a short/failed write; sync is pointless now
    }
    if (!synced) {
        return KIMIX_ERR_FAILED; // the bytes are in the file, but not durable
    }
    return KIMIX_OK;
}

// ===========================================================================
// 3. Deferred context
// ===========================================================================

kimix_status kimix_rw_context_create(kimix_rw_context **out) {
    if (!out) {
        return KIMIX_ERR_INVALID_ARG;
    }
    auto *ctx = static_cast<kimix_rw_context *>(mi_malloc_aligned(sizeof(kimix_rw_context),
                                                                 alignof(kimix_rw_context)));
    if (!ctx) {
        return KIMIX_ERR_OUT_OF_MEMORY;
    }
    ::new (ctx) kimix_rw_context();
    *out = ctx; // written only on success
    return KIMIX_OK;
}

void kimix_rw_context_free(kimix_rw_context *ctx) {
    if (!ctx) {
        return;
    }
    /* Release everything still owned, then the command lists (which own only
     * paths, plus the pointers just released) and the context itself. */
    for (auto &read : ctx->reads) {
        rw_release(ctx, read.buffer, read.size);
        read.buffer = nullptr;
    }
    for (auto &write : ctx->writes) {
        rw_release(ctx, write.payload, write.size);
        write.payload = nullptr;
    }
    for (auto &copy : ctx->copies) {
        rw_release(ctx, copy.buffer, copy.size);
        copy.buffer = nullptr;
    }
    ctx->~kimix_rw_context();
    mi_free(ctx);
}

kimix_status kimix_rw_context_add_read(kimix_rw_context *ctx, const char *path, uint64_t offset,
                                       size_t size, size_t *out_index) {
    if (!ctx || !path) {
        return KIMIX_ERR_INVALID_ARG;
    }
    kimix_rw_context::read_command command{kimix::string(path), offset, size, nullptr, 0, false,
                                           false};
    command.buffer = rw_alloc(ctx, size);
    if (size != 0 && !command.buffer) {
        return KIMIX_ERR_OUT_OF_MEMORY; // the context is left unchanged
    }
    ctx->reads.push_back(std::move(command));
    if (out_index) {
        *out_index = ctx->reads.size() - 1;
    }
    return KIMIX_OK;
}

kimix_status kimix_rw_context_add_write(kimix_rw_context *ctx, const char *path, uint64_t offset,
                                        const void *data, size_t size) {
    if (!ctx || !path) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (size != 0 && !data) {
        return KIMIX_ERR_INVALID_ARG;
    }
    kimix_rw_context::write_command command{kimix::string(path), offset, size, nullptr, false};
    command.payload = rw_alloc(ctx, size);
    if (size != 0 && !command.payload) {
        return KIMIX_ERR_OUT_OF_MEMORY;
    }
    if (size != 0) {
        /* The copy is the whole point: after this returns, the caller's buffer is
         * nobody's business but the caller's. */
        std::memcpy(command.payload, data, size);
    }
    ctx->writes.push_back(std::move(command));
    return KIMIX_OK;
}

kimix_status kimix_rw_context_add_copy(kimix_rw_context *ctx, const char *src_path,
                                       uint64_t src_offset, const char *dst_path,
                                       uint64_t dst_offset, size_t size) {
    if (!ctx || !src_path || !dst_path) {
        return KIMIX_ERR_INVALID_ARG;
    }
    kimix_rw_context::copy_command command{kimix::string(src_path), kimix::string(dst_path),
                                           src_offset, dst_offset, size, nullptr, false, false};
    command.buffer = rw_alloc(ctx, size);
    if (size != 0 && !command.buffer) {
        return KIMIX_ERR_OUT_OF_MEMORY;
    }
    ctx->copies.push_back(std::move(command));
    return KIMIX_OK;
}

size_t kimix_rw_context_read_count(const kimix_rw_context *ctx) {
    return ctx ? ctx->reads.size() : 0;
}

size_t kimix_rw_context_pending_reads(const kimix_rw_context *ctx) {
    if (!ctx) {
        return 0;
    }
    size_t pending = 0;
    for (const auto &read : ctx->reads) {
        if (!read.executed && !read.taken) {
            ++pending;
        }
    }
    return pending;
}

size_t kimix_rw_context_pending_writes(const kimix_rw_context *ctx) {
    return ctx ? ctx->writes.size() : 0;
}

size_t kimix_rw_context_pending_copies(const kimix_rw_context *ctx) {
    return ctx ? ctx->copies.size() : 0;
}

size_t kimix_rw_context_owned_bytes(const kimix_rw_context *ctx) {
    return ctx ? ctx->owned_bytes : 0;
}

size_t kimix_rw_context_owned_blocks(const kimix_rw_context *ctx) {
    return ctx ? ctx->owned_blocks : 0;
}

kimix_status kimix_rw_context_commit(kimix_rw_context *ctx, kimix_rw_stats *out_stats) {
    if (!ctx) {
        return KIMIX_ERR_INVALID_ARG;
    }

    kimix_rw_stats stats{};
    kimix::vector<rw_group_key> keys;

    /* -- Phase 1: the pending reads, grouped by path ------------------------- */
    /* Only slots that were never executed (and were not taken) are run: a slot
     * keeps its first result, so a second commit never re-reads the whole table,
     * and the caller re-reads by recording a new command. */
    for (size_t i = 0; i < ctx->reads.size(); ++i) {
        const auto &read = ctx->reads[i];
        if (!read.executed && !read.taken) {
            keys.push_back({&read.path, i, keys.size()});
        }
    }
    stats.reads_requested = keys.size();
    rw_sort_groups(keys);

    size_t key = 0;
    while (key < keys.size()) {
        const kimix::string &path = *keys[key].path;
        size_t next = key;
        while (next < keys.size() && *keys[next].path == path) {
            ++next;
        }

        /* One reader (== one fopen/fclose) for every command of this path.  A
         * closed stream means the path could not be opened: its commands fail,
         * the remaining groups still run. */
        kimix::BinaryFileStream stream(path);
        bool const open = stream.is_open();

        for (size_t slot = key; slot < next; ++slot) {
            auto &read = ctx->reads[keys[slot].index];
            read.executed = true;
            read.got = 0;
            /* Not seekable (32-bit `long`) or not open: 0 bytes, counted as a
             * failed read below rather than silently reading the wrong offset. */
            if (open && rw_offset_seekable(read.offset) && read.size != 0) {
                stream.set_pos(static_cast<long>(read.offset));
                read.got = stream.read(read.buffer, read.size);
            }
            stats.bytes_read += read.got;
            if (read.got == read.size) {
                ++stats.reads_completed; // a short read at the tail lands here as a failure
            }
        }

        key = next;
    }

    /* -- Phase 2: the read half of every copy, grouped by source path -------- */
    keys.clear();
    for (size_t i = 0; i < ctx->copies.size(); ++i) {
        ctx->copies[i].read = false;
        ctx->copies[i].completed = false;
        keys.push_back({&ctx->copies[i].src_path, i, keys.size()});
    }
    stats.copies_requested = keys.size();
    rw_sort_groups(keys);

    key = 0;
    while (key < keys.size()) {
        const kimix::string &path = *keys[key].path;
        size_t next = key;
        while (next < keys.size() && *keys[next].path == path) {
            ++next;
        }

        kimix::BinaryFileStream stream(path);
        bool const open = stream.is_open();

        for (size_t slot = key; slot < next; ++slot) {
            auto &copy = ctx->copies[keys[slot].index];
            size_t got = 0;
            if (open && rw_offset_seekable(copy.src_offset) && copy.size != 0) {
                stream.set_pos(static_cast<long>(copy.src_offset));
                got = stream.read(copy.buffer, copy.size);
            }
            stats.bytes_read += got;
            /* A copy whose read was short is NOT written: half a file pushed over
             * the destination would be worse than not copying at all. */
            copy.read = (got == copy.size);
        }

        key = next;
    }

    /* -- Phase 3: the writes, grouped by destination path -------------------- */
    /* One job list over both sources of writes, kept in record order per path so
     * overlapping ranges land in the order the caller recorded them. */
    kimix::vector<rw_write_job> jobs;
    jobs.reserve(ctx->writes.size() + ctx->copies.size());
    for (size_t i = 0; i < ctx->writes.size(); ++i) {
        jobs.push_back({&ctx->writes[i].path, ctx->writes[i].offset, ctx->writes[i].payload,
                        ctx->writes[i].size, i, 0, false});
    }
    for (size_t i = 0; i < ctx->copies.size(); ++i) {
        if (ctx->copies[i].read) {
            jobs.push_back({&ctx->copies[i].dst_path, ctx->copies[i].dst_offset,
                            ctx->copies[i].buffer, ctx->copies[i].size, i, 0, true});
        }
    }
    for (size_t i = 0; i < jobs.size(); ++i) {
        jobs[i].serial = i;
    }
    kimix::sort(jobs.begin(), jobs.end(), [](const rw_write_job &lhs, const rw_write_job &rhs) {
        if (*lhs.path != *rhs.path) {
            return *lhs.path < *rhs.path;
        }
        return lhs.serial < rhs.serial;
    });
    stats.writes_requested = ctx->writes.size();

    size_t job = 0;
    while (job < jobs.size()) {
        const kimix::string &path = *jobs[job].path;
        size_t next = job;
        bool any_bytes = false;
        while (next < jobs.size() && *jobs[next].path == path) {
            any_bytes = any_bytes || jobs[next].size != 0;
            ++next;
        }

        /* The file is created when missing and NEVER truncated: an offset write
         * must not destroy the content it does not cover.  A group that only
         * carries 0-byte writes needs no file at all. */
        bool open = false;
        kimix::BinaryFileWriter writer;
        if (any_bytes) {
            writer = kimix::BinaryFileWriter(path, false);
            open = writer.is_open();
        }

        for (size_t slot = job; slot < next; ++slot) {
            const rw_write_job &item = jobs[slot];
            bool ok = false;
            if (item.size == 0) {
                ok = true; // nothing to hand to the disk: trivially complete
            } else if (open) {
                writer.set_pos(item.offset);
                size_t const written = writer.write(item.data, item.size);
                stats.bytes_written += written;
                ok = (written == item.size); // all-or-nothing per command
            }
            if (ok) {
                if (item.is_copy) {
                    ctx->copies[item.index].completed = true;
                    ++stats.copies_completed;
                } else {
                    ctx->writes[item.index].completed = true;
                    ++stats.writes_completed;
                }
            }
        }

        if (open) {
            /* Durability before the commit reports the bytes as written, and the
             * handle released before another group opens another file. */
            writer.sync();
            writer.close();
        }

        job = next;
    }

    /* -- Release what the disk no longer needs ------------------------------ */
    /* The writes and the copies are CONSUMED by a commit, whatever their outcome
     * (a failed command would only fail again; recording a new one is how a
     * caller retries).  This is the automatic free the header documents: each
     * payload goes back to the heap here, once its bytes reached the file (or
     * once it is known that they never will), and each copy staging buffer goes
     * back right after the write that used it. */
    for (auto &write : ctx->writes) {
        rw_release(ctx, write.payload, write.size);
        write.payload = nullptr;
    }
    ctx->writes.clear();
    for (auto &copy : ctx->copies) {
        rw_release(ctx, copy.buffer, copy.size);
        copy.buffer = nullptr;
    }
    ctx->copies.clear();

    /* Only the read slots survive a commit -- they hold the result. */
    stats.reads_failed = stats.reads_requested - stats.reads_completed;
    stats.writes_failed = stats.writes_requested - stats.writes_completed;
    stats.copies_failed = stats.copies_requested - stats.copies_completed;
    stats.owned_bytes = ctx->owned_bytes;
    stats.owned_blocks = ctx->owned_blocks;

    if (out_stats) {
        *out_stats = stats;
    }
    return KIMIX_OK;
}

kimix_status kimix_rw_context_read_result(const kimix_rw_context *ctx, size_t index,
                                          const void **data, size_t *size) {
    if (!ctx || !data || !size) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (index >= ctx->reads.size()) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    const auto &read = ctx->reads[index];
    if (!read.executed || read.taken) {
        /* Not filled yet, or the bytes left with the caller in take_read(): in
         * both cases the context is not the owner of a result to hand out. */
        return KIMIX_ERR_INVALID_STATE;
    }
    *data = read.buffer; // borrowed: valid until the slot is taken / cleared / freed
    *size = read.got;    // 0 (and possibly NULL) for a read that delivered nothing
    return KIMIX_OK;
}

kimix_status kimix_rw_context_take_read(kimix_rw_context *ctx, size_t index, void **data,
                                        size_t *size) {
    if (!ctx || !data || !size) {
        return KIMIX_ERR_INVALID_ARG;
    }
    if (index >= ctx->reads.size()) {
        return KIMIX_ERR_OUT_OF_RANGE;
    }
    auto &read = ctx->reads[index];
    if (!read.executed || read.taken) {
        return KIMIX_ERR_INVALID_STATE;
    }
    /* Ownership moves: out of the ledger, out of every release path of this
     * context from here on, and into kimix_mem_free()'s of the caller.  The
     * block itself must NOT be freed -- rw_forget() only stops counting it. */
    rw_forget(ctx, read.buffer, read.size);
    read.taken = true;
    *data = read.buffer; // the same block, now with no counterpart here
    *size = read.got;    // what landed, which may be less than the slot size
    read.buffer = nullptr;
    return KIMIX_OK;
}

kimix_status kimix_rw_context_clear(kimix_rw_context *ctx) {
    if (!ctx) {
        return KIMIX_ERR_INVALID_ARG;
    }
    for (auto &read : ctx->reads) {
        rw_release(ctx, read.buffer, read.size);
        read.buffer = nullptr;
    }
    for (auto &write : ctx->writes) {
        rw_release(ctx, write.payload, write.size);
        write.payload = nullptr;
    }
    for (auto &copy : ctx->copies) {
        rw_release(ctx, copy.buffer, copy.size);
        copy.buffer = nullptr;
    }
    ctx->reads.clear();
    ctx->writes.clear();
    ctx->copies.clear();
    /* The ledger is exactly zero here: every block was released above, and the
     * ones a caller took are not counted any more (take_read() removed them). */
    ctx->owned_bytes = 0;
    ctx->owned_blocks = 0;
    return KIMIX_OK;
}

KIMIX_FFI_END
