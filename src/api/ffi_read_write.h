/*
 * ffi_read_write.h -- binary file reading and writing for foreign callers.
 *
 * SCOPE
 * -----
 * The file I/O a foreign language (C, C#, Zig, Rust, Python ctypes, ...) needs
 * without any C++ in the picture: read a file, write a file, and batch several
 * reads/writes into one commit.  Three objects, one area:
 *
 *   kimix_rw_stream   a reader, the C ABI of kimix::BinaryFileStream
 *                     (src/core/binary_file_stream.h): open a path, ask for its
 *                     length / position, read a span at the current position,
 *                     read the whole file, seek, close.
 *   kimix_rw_writer   a writer, the C ABI of kimix::BinaryFileWriter
 *                     (src/core/binary_file_writer.h): create/truncate/keep a
 *                     path, write at the current position, write at an absolute
 *                     position, ask for the position, fsync, close.
 *   kimix_rw_context  the DEFERRED CONTEXT -- the mid-term lifetime manager
 *                     described below: record reads/writes/copies now, hand
 *                     them to the disk in one commit(), and let the context own
 *                     (and release) every temporary buffer in between.
 *
 * A managed caller cannot tie its object lifetime to a C++ scope: it opens, uses
 * and closes across separate calls, and the GC decides when its own buffers move
 * or die.  Every entry point below is therefore self-contained -- a handle plus
 * plain values in, a status (or a byte count) out -- and no pointer handed back
 * is ever required to outlive the next call unless the documentation says so.
 *
 * THE DEFERRED CONTEXT (kimix_rw_context) -- WHY IT EXISTS, AND WHAT IT OWNS
 * ------------------------------------------------------------------------
 * A batch needs memory that survives from the moment a command is RECORDED to
 * the moment the disk WRITE has happened: the destination of a read, the payload
 * of a write, the staging buffer of a copy.  In C++ that is a local vector; in a
 * managed caller it would be a pinned GC buffer held across an indeterminate
 * number of calls -- exactly the lifetime a managed language cannot express.
 * The context takes that memory over:
 *
 *   kimix_rw_context_add_read(ctx, path, offset, size, &index)
 *       The library ALLOCATES the destination buffer (zero-filled, on this
 *       library's mimalloc heap) and records a positioned read into it.  It is
 *       owned by the context; the caller never passes a buffer it has to keep
 *       alive.
 *   kimix_rw_context_add_write(ctx, path, offset, data, size)
 *       The library COPIES `data` onto the library heap.  The caller's buffer is
 *       free the moment the call returns -- it may be unpinned, reused or
 *       collected immediately.
 *   kimix_rw_context_add_copy(ctx, src_path, src_offset, dst_path, dst_offset,
 *                             size)
 *       The library allocates the staging buffer; the commit reads the source
 *       into it and pushes it out to the destination.
 *
 * kimix_rw_context_commit() then does the I/O and RELEASES what the disk no
 * longer needs, with no free call from the caller:
 *
 *   - a WRITE payload is released once its bytes reached the file (and the file
 *     was synced), so a recorded write costs memory only for the record-to-write
 *     window -- "auto free after the commit's disk write";
 *   - a COPY's staging buffer is released right after it is written out;
 *   - a READ destination is KEPT, because the bytes it holds are the result the
 *     caller asked for: they stay readable through
 *     kimix_rw_context_read_result() (borrowed) or become the caller's own
 *     memory through kimix_rw_context_take_read() (then released with
 *     kimix_mem_free()), and the context releases anything still untaken when it
 *     is cleared or freed.
 *   - the ledger is observable: kimix_rw_context_owned_bytes() /
 *     ..._owned_blocks() report exactly what the context still holds, and
 *     kimix_rw_stats.owned_bytes / owned_blocks report it as of the commit that
 *     just returned.
 *
 * A commit() never leaves a command half-owned: the recorded writes and copies
 * are always consumed (dropped, memory released), and every read slot that was
 * executed keeps its bytes for the caller.
 *
 * ONE HEAP
 * --------
 * Every buffer this area hands out -- read slots taken with take_read(), the
 * contents of a kimix_vec filled by kimix_rw_stream_read_all() -- is allocated
 * on THIS library's mimalloc heap (src/api/ffi_mem.h) and must be released with
 * the matching call of this library (kimix_mem_free() / kimix_vec_destroy() /
 * kimix_vec_free()), never with the caller's own free().
 *
 * THE ONE-SHOT HELPERS
 * --------------------
 * kimix_rw_read_file() and kimix_rw_write_file() are the open/use/close
 * sequences of the two handles folded into a single call, for the common
 * "the whole file is one buffer" case.
 *
 * PLATFORM LIMITS (inherited from the C++ classes, see their headers)
 * ------------------------------------------------------------------
 *   - Reading (kimix_rw_stream_*): kimix::BinaryFileStream positions with a
 *     `long`, and derives the length with ftell(), which is 32 bits on Windows
 *     (LLP64).  A file of 2 GiB or more is therefore not addressable through
 *     this area ON WINDOWS (an offset above LONG_MAX is refused with
 *     KIMIX_ERR_OUT_OF_RANGE instead of being silently truncated, and length()
 *     is then a 32-bit value).  Linux has no such limit.
 *   - Writing (kimix_rw_writer_*): kimix::BinaryFileWriter is backed by the
 *     vendored xNVMe, whose open path stats the file with a 32-bit `stat` on
 *     Windows: a file of 2 GiB or more cannot be OPENED there, so open() reports
 *     KIMIX_ERR_NOT_FOUND.  Linux has no such limit.
 *   - Batched I/O (kimix_rw_context_*): writes go through the writer, so the
 *     same Windows 2 GiB open limit applies; reads go through the reader, so the
 *     same `long` position limit applies.
 *   - A platform for which this build wires no xnvme backend (macOS, FreeBSD,
 *     ...) can read through this area but every writer open fails with
 *     KIMIX_ERR_NOT_FOUND.
 *   - Windows: a file cannot be deleted while a kimix_rw_writer still holds it
 *     open -- call kimix_rw_writer_close() first (the xnvme share mode omits
 *     FILE_SHARE_DELETE).
 *
 * THREADING
 * ---------
 * As everywhere in this ABI: a handle is not thread-safe, two threads must not
 * use the same one concurrently, and each thread that is foreign to the host
 * process may bracket its life with kimix_mem_thread_init() /
 * kimix_mem_thread_done() (api/ffi_mem.h).
 *
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h for
 * the rules shared by the whole surface.  The index is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_READ_WRITE_H
#define KIMIX_API_FFI_READ_WRITE_H

#include <api/ffi_common.h>
#include <api/ffi_vec.h>

/* The three handles are opaque: the C++ objects behind them (kimix's
 * BinaryFileStream / BinaryFileWriter and the deferred context) live in this
 * library's translation unit, and a caller only ever holds the pointer plus the
 * free function named for it.  A NULL handle is accepted by every function that
 * does not create one and answered with the documented neutral value, never with
 * a crash. */
typedef struct kimix_rw_stream kimix_rw_stream;
typedef struct kimix_rw_writer kimix_rw_writer;
typedef struct kimix_rw_context kimix_rw_context;

KIMIX_FFI_BEGIN

/* ===========================================================================
 * 1. Reader -- kimix::BinaryFileStream
 *
 *    The stream is a FILE* reader: it keeps a position, every read advances it,
 *    and one handle serves one thread at a time.
 * ======================================================================== */

/* Opens `path` read-only.  `path` is a UTF-8 path in the platform's filename
 * rules (the library passes it to fopen, so the platform's own encoding rules
 * apply).
 *   KIMIX_OK              *out is a handle whose file is open.
 *   KIMIX_ERR_NOT_FOUND   the file is missing or cannot be opened (permission,
 *                         a directory, a Windows 2 GiB+ file -- see the platform
 *                         limits in this header); *out is NOT written.
 *   KIMIX_ERR_INVALID_ARG `path` or `out` is NULL.
 *   KIMIX_ERR_OUT_OF_MEMORY  the handle itself could not be allocated.
 * Release the handle with kimix_rw_stream_close() (or kimix_rw_stream_free(),
 * which additionally releases the handle's own memory). */
KIMIX_FFI kimix_status kimix_rw_stream_open(KIMIX_IN const char *path,
                                            KIMIX_OUT kimix_rw_stream **out);

/* Closes the file and resets the handle to the closed state (length 0,
 * position 0, every later call reports the neutral value).  NULL is a no-op.
 * The handle stays usable as an object -- it just has no file any more, and
 * there is no way to reopen it: open a new stream instead. */
KIMIX_FFI void kimix_rw_stream_close(KIMIX_NULLABLE kimix_rw_stream *stream);

/* Releases the handle itself (closing the file first).  NULL is a no-op, so a
 * caller's cleanup path can be unconditional.  After this call the pointer is
 * dangling -- do not close() or free() it again. */
KIMIX_FFI void kimix_rw_stream_free(KIMIX_TRANSFER KIMIX_NULLABLE kimix_rw_stream *stream);

/* True while a file is open.  False for a NULL handle and for a closed one. */
KIMIX_FFI bool kimix_rw_stream_is_open(KIMIX_IN const kimix_rw_stream *stream);

/* The file size captured when it was opened, in BYTES.  0 for a NULL handle, a
 * closed stream or an unknown size.  Note the Windows 32-bit ftell limit
 * documented at the top of this header. */
KIMIX_FFI uint64_t kimix_rw_stream_length(KIMIX_IN const kimix_rw_stream *stream);

/* The position the NEXT read starts at, in BYTES from the beginning of the file.
 * 0 for a NULL handle.  A negative value from the underlying ftell (an error)
 * is reported as 0. */
KIMIX_FFI uint64_t kimix_rw_stream_position(KIMIX_IN const kimix_rw_stream *stream);

/* Seeks to an absolute position; the next read starts there.  'pos' beyond the
 * end of the file is allowed (the following read simply returns 0 bytes).
 *   KIMIX_OK                the position was applied.
 *   KIMIX_ERR_INVALID_ARG   `stream` is NULL.
 *   KIMIX_ERR_INVALID_STATE no file is open.
 *   KIMIX_ERR_OUT_OF_RANGE  'pos' cannot be expressed by the underlying 32-bit
 *                           `long` position (Windows, 'pos' > 2147483647) --
 *                           refused rather than silently truncated. */
KIMIX_FFI kimix_status kimix_rw_stream_set_pos(KIMIX_IN_OUT kimix_rw_stream *stream,
                                               uint64_t pos);

/* Reads up to `size` bytes at the current position into `buffer` and advances
 * the position by the number of bytes read.  Returns the number of bytes that
 * landed; a SHORT count is normal at the end of the file (read until 0 to be
 * sure the data is complete).  0 also covers a NULL handle, a closed stream, a
 * NULL buffer, a 0 size and an unreadable file -- the reader shares one error
 * channel with end-of-file, so a caller that needs to tell the two apart checks
 * kimix_rw_stream_is_open() and the file's expected size.  `buffer` is
 * caller-owned and must be writable for `size` bytes. */
KIMIX_FFI size_t kimix_rw_stream_read(KIMIX_IN_OUT kimix_rw_stream *stream,
                                      KIMIX_OUT void *buffer, size_t size);

/* kimix_rw_stream_set_pos() + kimix_rw_stream_read(), in one call: reads at an
 * absolute position, leaving the stream's position just past the bytes read.
 * Same return value and same rules as the two calls it folds; an unseekable
 * position (see set_pos) reads 0 bytes. */
KIMIX_FFI size_t kimix_rw_stream_read_at(KIMIX_IN_OUT kimix_rw_stream *stream, uint64_t pos,
                                         KIMIX_OUT void *buffer, size_t size);

/* Reads the WHOLE file from the beginning into `out` (the stream's position
 * ends at the end of the data, as kimix::BinaryFileStream::read_all() does).
 *   KIMIX_OK              *out holds the bytes (empty for an empty file).
 *   KIMIX_ERR_INVALID_ARG a NULL handle or `out`.
 *   KIMIX_ERR_INVALID_STATE no file is open, or `out` already holds a live
 *                           kimix_vec -- destroy()/free() it first, this entry
 *                           point only INITIALISES a raw placeholder.
 * The bytes are owned by the caller's placeholder: release them with
 * kimix_vec_destroy() (or kimix_vec_free() for the heap form). */
KIMIX_FFI kimix_status kimix_rw_stream_read_all(KIMIX_IN_OUT kimix_rw_stream *stream,
                                                KIMIX_OUT kimix_vec *out);

/* Opens `path`, reads it whole into `out` and closes it -- the one-shot form of
 * read_all().  Same statuses as kimix_rw_stream_open() plus the ones of
 * read_all() (KIMIX_ERR_INVALID_STATE for a placeholder that is already live). */
KIMIX_FFI kimix_status kimix_rw_read_file(KIMIX_IN const char *path, KIMIX_OUT kimix_vec *out);

/* ===========================================================================
 * 2. Writer -- kimix::BinaryFileWriter
 *
 *    Positioned writes through xNVMe (see the platform limits above).  The
 *    writer keeps a write position; set_pos() moves it, so an existing file can
 *    be patched in place (open with truncate = false).
 * ======================================================================== */

/* Opens (and creates) `path` for writing.
 *   `truncate` true   an existing file is emptied (the default of the C++
 *                     class's path constructor).
 *   `truncate` false  existing content is KEPT; positioned writes then overwrite
 *                     or extend it.
 *   KIMIX_OK              *out is a handle whose file is open (position 0).
 *   KIMIX_ERR_NOT_FOUND   the file could not be opened/created (missing
 *                         directory, permission, a Windows 2 GiB+ file, or a
 *                         platform without a wired xnvme backend); *out is NOT
 *                         written.
 *   KIMIX_ERR_INVALID_ARG `path`/`out` is NULL, or `path` is empty.
 *   KIMIX_ERR_OUT_OF_MEMORY  the handle itself could not be allocated.
 * Release the handle with kimix_rw_writer_close() (or kimix_rw_writer_free()).
 * close() does NOT sync: the data already handed to the OS survives it, only the
 * durability guarantee needs kimix_rw_writer_sync(). */
KIMIX_FFI kimix_status kimix_rw_writer_open(KIMIX_IN const char *path, bool truncate,
                                            KIMIX_OUT kimix_rw_writer **out);

/* Closes the file handle and resets the position to 0.  Does NOT sync.  NULL is
 * a no-op; the handle object stays allocated but closed. */
KIMIX_FFI void kimix_rw_writer_close(KIMIX_NULLABLE kimix_rw_writer *writer);

/* Releases the handle itself (closing the file first -- on Windows the file can
 * only be deleted afterwards).  NULL is a no-op. */
KIMIX_FFI void kimix_rw_writer_free(KIMIX_TRANSFER KIMIX_NULLABLE kimix_rw_writer *writer);

/* True while a file is open.  False for a NULL handle and for a closed one. */
KIMIX_FFI bool kimix_rw_writer_is_open(KIMIX_IN const kimix_rw_writer *writer);

/* The absolute position the next write goes to, in BYTES.  0 for a NULL handle
 * or a closed writer. */
KIMIX_FFI uint64_t kimix_rw_writer_position(KIMIX_IN const kimix_rw_writer *writer);

/* Sets the absolute position of the next write.  A position beyond the current
 * end of the file is allowed: the write then extends it.  NULL is a no-op (the
 * C++ setter is total and takes no status). */
KIMIX_FFI void kimix_rw_writer_set_pos(KIMIX_NULLABLE kimix_rw_writer *writer, uint64_t pos);

/* Writes `size` bytes at the current position and advances it.  ALL-OR-NOTHING:
 * the return value is `size` on success or 0 on failure (closed writer, NULL
 * buffer, a non-zero size reaching a 2 GiB+ file, a full disk).  A size of 0 is
 * a successful no-op that returns 0. */
KIMIX_FFI size_t kimix_rw_writer_write(KIMIX_IN_OUT kimix_rw_writer *writer,
                                       KIMIX_IN const void *data, size_t size);

/* kimix_rw_writer_set_pos() + kimix_rw_writer_write(), in one call: writes at an
 * absolute position, leaving the writer's position just past the bytes written.
 * Same return value and same rules as the two calls it folds. */
KIMIX_FFI size_t kimix_rw_writer_write_at(KIMIX_IN_OUT kimix_rw_writer *writer, uint64_t pos,
                                          KIMIX_IN const void *data, size_t size);

/* Flushes the written data to stable storage (fsync(2) / FlushFileBuffers()).
 * False for a NULL handle, a closed writer or an I/O error. */
KIMIX_FFI bool kimix_rw_writer_sync(KIMIX_IN_OUT kimix_rw_writer *writer);

/* Opens `path`, writes the whole buffer at position 0, syncs and closes -- the
 * one-shot form of the open/write/sync/close sequence.  `truncate` behaves as in
 * kimix_rw_writer_open().
 *   KIMIX_OK              every byte landed and was synced.
 *   KIMIX_ERR_NOT_FOUND   the file could not be opened/created.
 *   KIMIX_ERR_INVALID_ARG `path` is NULL/empty, or `data` is NULL with a
 *                         non-zero `size`.
 *   KIMIX_ERR_FAILED      the write or the sync failed (e.g. a full disk). */
KIMIX_FFI kimix_status kimix_rw_write_file(KIMIX_IN const char *path, KIMIX_IN const void *data,
                                           size_t size, bool truncate);

/* ===========================================================================
 * 3. Deferred context -- recorded I/O, one commit, owned temp memory
 * ======================================================================== */

/* What one kimix_rw_context_commit() did, plus the memory ledger after it.
 *
 * `reads_requested` counts the read slots THIS commit executed (the pending ones
 * at entry); a slot that was already executed or taken earlier is not retried
 * and not counted.  A read "completes" when it transferred exactly the recorded
 * byte count -- the short read at the end of a file is reported as a failure
 * with the bytes it did deliver, exactly like the C++ class reports it.
 *
 * `writes_requested` / `copies_requested` count every recorded command the
 * commit consumed (they are always all of them: a commit drops them all).
 *
 * `bytes_read` counts the bytes the reads and the read halves of the copies
 * delivered; `bytes_written` counts the bytes that reached the disk.
 *
 * `owned_bytes` / `owned_blocks` are the context's live temporary allocations
 * AFTER the commit: the read destinations it still holds (taken or not) and
 * nothing else -- the write payloads and the copy staging buffers were released
 * by the commit, and so is any read slot that has been taken.  Both are 0 for an
 * empty context. */
typedef struct kimix_rw_stats {
    size_t reads_requested;
    size_t reads_completed;
    size_t reads_failed;
    size_t writes_requested;
    size_t writes_completed;
    size_t writes_failed;
    size_t copies_requested;
    size_t copies_completed;
    size_t copies_failed;
    uint64_t bytes_read;
    uint64_t bytes_written;
    size_t owned_bytes;
    size_t owned_blocks;
} kimix_rw_stats;

/* Creates an empty context.
 *   KIMIX_OK              *out is a handle with no commands and no memory.
 *   KIMIX_ERR_INVALID_ARG `out` is NULL.
 *   KIMIX_ERR_OUT_OF_MEMORY  the context could not be allocated.
 * Release it with kimix_rw_context_free() (which releases every byte it owns). */
KIMIX_FFI kimix_status kimix_rw_context_create(KIMIX_OUT kimix_rw_context **out);

/* Releases the context and EVERY byte it still owns (pending commands, read
 * slots that were not taken, staging buffers), then the handle itself.  NULL is a
 * no-op.  Buffers a caller TOOK with kimix_rw_context_take_read() are the
 * caller's and are untouched -- release those with kimix_mem_free(). */
KIMIX_FFI void kimix_rw_context_free(KIMIX_TRANSFER KIMIX_NULLABLE kimix_rw_context *ctx);

/* Records a positioned read: `size` bytes of `path` at `offset`.  The library
 * allocates the destination buffer (zero-filled, on this library's heap) and
 * keeps it until the slot is taken or the context is cleared/freed -- the caller
 * passes no buffer to keep alive.
 * `out_index` receives the slot index used by kimix_rw_context_read_result() /
 * ..._take_read(); it may be NULL when the caller does not need it.  Indices stay
 * valid until kimix_rw_context_clear() (a taken slot keeps its index).
 * A read is only issued by kimix_rw_context_commit(); nothing is opened here.
 *   KIMIX_OK              the command is recorded.
 *   KIMIX_ERR_INVALID_ARG a NULL context or `path`.
 *   KIMIX_ERR_OUT_OF_MEMORY  the destination buffer could not be allocated (the
 *                         context is left unchanged); `size` 0 needs none and
 *                         records a read that delivers 0 bytes.
 * NOTE: `offset` above the reader's 32-bit position limit is refused at COMMIT
 * time (that read fails), not here -- recording a command only stores numbers. */
KIMIX_FFI kimix_status kimix_rw_context_add_read(KIMIX_IN_OUT kimix_rw_context *ctx,
                                                 KIMIX_IN const char *path, uint64_t offset,
                                                 size_t size,
                                                 KIMIX_OUT KIMIX_NULLABLE size_t *out_index);

/* Records a positioned write: `size` bytes of `data` to `path` at `offset`.  The
 * bytes are COPIED onto the library heap NOW, so the caller's buffer may be
 * released, reused or collected the moment this call returns; the copy lives
 * until the commit's disk write has happened and is released by that commit.
 * The file is opened (created if missing, NEVER truncated) at commit time, so
 * other content of the file is preserved.
 *   KIMIX_OK              the command is recorded.
 *   KIMIX_ERR_INVALID_ARG a NULL context or `path`, or `data` NULL with a
 *                         non-zero `size`.
 *   KIMIX_ERR_OUT_OF_MEMORY  the payload copy could not be allocated (the
 *                         context is left unchanged). */
KIMIX_FFI kimix_status kimix_rw_context_add_write(KIMIX_IN_OUT kimix_rw_context *ctx,
                                                  KIMIX_IN const char *path, uint64_t offset,
                                                  KIMIX_IN const void *data, size_t size);

/* Records a file-to-file copy: `size` bytes of `src_path` at `src_offset` are
 * read into a library-owned staging buffer at commit time and written to
 * `dst_path` at `dst_offset`.  The staging buffer is released by the commit right
 * after the write, so the caller never sees it; a copy whose read did not
 * deliver every byte is NOT written (and counts as a failed copy).
 * `dst_path` is opened (created if missing, NEVER truncated) at commit time.
 * Statuses as for the two commands above (INVALID_ARG for a NULL context or a
 * NULL path, OUT_OF_MEMORY for the staging buffer). */
KIMIX_FFI kimix_status kimix_rw_context_add_copy(KIMIX_IN_OUT kimix_rw_context *ctx,
                                                 KIMIX_IN const char *src_path,
                                                 uint64_t src_offset,
                                                 KIMIX_IN const char *dst_path,
                                                 uint64_t dst_offset, size_t size);

/* Number of recorded read slots: the valid index range of read_result() /
 * take_read().  Indices are assigned in add order and stay stable until clear(). */
KIMIX_FFI size_t kimix_rw_context_read_count(KIMIX_IN const kimix_rw_context *ctx);

/* Reads a read slot that has not been executed by a commit yet -- i.e. the work
 * the NEXT commit will do.  A slot that was executed (or taken) is not pending
 * any more. */
KIMIX_FFI size_t kimix_rw_context_pending_reads(KIMIX_IN const kimix_rw_context *ctx);

/* Recorded writes that the next commit will consume.  0 for a NULL context. */
KIMIX_FFI size_t kimix_rw_context_pending_writes(KIMIX_IN const kimix_rw_context *ctx);

/* Recorded copies that the next commit will consume.  0 for a NULL context. */
KIMIX_FFI size_t kimix_rw_context_pending_copies(KIMIX_IN const kimix_rw_context *ctx);

/* Bytes the context currently owns (read destinations, write payloads and copy
 * staging buffers) and the number of live allocations they came from.  0/0 for a
 * NULL context, an empty one, and one whose temp memory has all been released by
 * a commit / clear / take_read.  This is the ledger that makes the automatic
 * release observable: it is the same pair kimix_rw_stats reports after a commit. */
KIMIX_FFI size_t kimix_rw_context_owned_bytes(KIMIX_IN const kimix_rw_context *ctx);
KIMIX_FFI size_t kimix_rw_context_owned_blocks(KIMIX_IN const kimix_rw_context *ctx);

/* Executes every pending command and releases the temp memory the disk no longer
 * needs.  The order is fixed and worth knowing:
 *   1. every pending READ is filled, grouping the commands by path so each file
 *      is opened, read and closed exactly once;
 *   2. the read half of every recorded COPY runs the same way (grouped by source
 *      path);
 *   3. the writes go to disk: the recorded writes plus the copies whose read
 *      landed fully, grouped by destination path (one open per file), each file
 *      synced and closed after its writes.  Reads run before every write, so
 *      reading and writing the same file inside one commit is well defined.
 * Afterwards: the recorded writes and copies are always dropped (their memory
 * released, including the copy staging buffers), the read slots stay -- holding
 * their result -- and one slot is retried only by adding a new one.
 * `out_stats` may be NULL; when it is not, it is fully written.
 *   KIMIX_OK              the commit ran.  Failures are COUNTED, not returned:
 *                         a missing/unopenable path, a short read or a rejected
 *                         write only fails that command and never aborts the
 *                         batch -- read the stats (and the per-slot result).
 *   KIMIX_ERR_INVALID_ARG a NULL context.
 * Memory exhaustion is not reported here: the allocation happens in the add_*
 * calls, so a commit only ever frees. */
KIMIX_FFI kimix_status kimix_rw_context_commit(KIMIX_IN_OUT kimix_rw_context *ctx,
                                               KIMIX_OUT KIMIX_NULLABLE kimix_rw_stats *out_stats);

/* The result of a read slot, BORROWED from the context (valid until the slot is
 * taken, the context is cleared or freed, whichever comes first).  `*data` and
 * `*size` receive the bytes that actually landed, so `*size` is 0 -- and `*data`
 * may be NULL -- for a slot whose read failed or delivered nothing.  The buffer
 * itself can be longer than `*size` (it is the recorded size); that extra room is
 * not part of the result.
 *   KIMIX_OK                the slot was executed; the pointers are written.
 *   KIMIX_ERR_INVALID_ARG   a NULL context, `data` or `size`.
 *   KIMIX_ERR_OUT_OF_RANGE  `index` is not a valid slot.
 *   KIMIX_ERR_INVALID_STATE the slot has not been executed by a commit yet, or
 *                           its bytes were already taken by the caller. */
KIMIX_FFI kimix_status kimix_rw_context_read_result(KIMIX_IN const kimix_rw_context *ctx,
                                                    size_t index,
                                                    KIMIX_OUT const void **data,
                                                    KIMIX_OUT size_t *size);

/* Hands the bytes of an executed read slot to the caller: the buffer stops being
 * owned by the context (so clear()/free() will not touch it) and is released by
 * the caller with kimix_mem_free().  One take per slot: a second take, a take
 * before the commit that filled it, and an invalid index are all reported and
 * change nothing.
 *   KIMIX_OK                `*data` (may be NULL for a 0-byte result) and `*size`
 *                           are the caller's; free `*data` with kimix_mem_free().
 *   KIMIX_ERR_INVALID_ARG   a NULL context, `data` or `size`.
 *   KIMIX_ERR_OUT_OF_RANGE  `index` is not a valid slot.
 *   KIMIX_ERR_INVALID_STATE the slot has not been executed yet or was already
 *                           taken. */
KIMIX_FFI kimix_status kimix_rw_context_take_read(KIMIX_IN_OUT kimix_rw_context *ctx, size_t index,
                                                  KIMIX_OUT void **data, KIMIX_OUT size_t *size);

/* Drops every recorded command and releases EVERY byte the context owns that the
 * caller has not taken: pending and executed reads alike, plus any write payload
 * or copy staging buffer not yet committed.  The handle stays usable (empty), and
 * the read index space restarts at 0.  Buffers taken with take_read() are the
 * caller's and are untouched.
 *   KIMIX_OK              the context is empty and owns nothing.
 *   KIMIX_ERR_INVALID_ARG a NULL context. */
KIMIX_FFI kimix_status kimix_rw_context_clear(KIMIX_IN_OUT kimix_rw_context *ctx);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_READ_WRITE_H */
