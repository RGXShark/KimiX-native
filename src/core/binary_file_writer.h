/*
 * binary_file_writer.h — kimix::BinaryFileWriter + kimix::DeferredBinaryWriter:
 * the write side of the xNVMe-backed file I/O (the counterpart of
 * kimix::XnvmeBinaryFileStream / kimix::DeferredBinaryFileStream).
 *
 * kimix::BinaryFileWriter:
 *   Opens (and creates) a file for writing through xNVMe and writes
 *   byte-granularly at an absolute position. Writes are sequential from the
 *   current position; set_pos() moves that position, so a writer can also
 *   overwrite arbitrary regions of an existing file (truncate = false).
 *
 *   Methods:
 *     write(ptr, size) / write(span) — write at the current position, returns
 *                                      the bytes written (== size) or 0
 *     set_pos(pos)                   — absolute position for the next write
 *     position()                     — current write position
 *     sync()                         — flush to stable storage (fsync /
 *                                      FlushFileBuffers), true on success
 *     close()                        — close the handle and reset the position
 *     is_open() / operator bool      — true while a handle is open
 *
 *   Constructor semantics: BinaryFileWriter("f.bin") truncates (default
 *   truncate = true, i.e. the file is created empty); BinaryFileWriter("f.bin",
 *   false) creates a missing file but KEEPS existing content, which is what
 *   offset writes into an existing file need. A missing/unopenable file leaves
 *   the writer closed (no exception); every method is then a no-op that reports
 *   failure (0 bytes / false).
 *
 * kimix::DeferredBinaryWriter:
 *   The batched counterpart: the caller hands over the payload of every write
 *   with add_write(), and commit() groups the commands by path, opens each path
 *   once for writing WITHOUT truncation, writes every command at its offset,
 *   syncs, closes and finally releases the payloads.
 *
 *   MEMORY LIFECYCLE (the important part):
 *     - add_write() MOVES the payload into the command list; the caller's vector
 *       is empty afterwards and must be passed as std::move(data).
 *     - The bytes stay owned by the command until commit() or clear() returns, so
 *       the caller cannot observe or mutate them in between.
 *     - commit() writes straight out of the command's vector storage — no copy of
 *       the payload is made anywhere on the path.
 *
 *   Example:
 *     kimix::DeferredBinaryWriter writer;
 *     kimix::vector<std::byte> payload = ...;
 *     writer.add_write("out.bin", 4096, std::move(payload)); // payload is now empty
 *     if (writer.commit() == 1) { ... }
 *
 *   Both classes are move-only. Writes and syncs go through the xnvme C ABI shim
 *   (binary_file_stream_xnvme_shim.h); this header never includes <libxnvme.h> and
 *   keeps `struct xnvme_dev` opaque. Like the xNVMe-backed readers, neither class
 *   is part of <core/kimix_core.h>.
 *
 *   LIMITS — WINDOWS: a file of 2 GiB or more cannot be opened at all, because the
 *   vendored xnvme open path stats the file through a 32-bit stat, so the open
 *   fails and is_open() is false (write() then returns 0); Linux has no such
 *   limit.
 *
 *   LIMITS — NO WIRED BACKEND: on a platform xnvme has no backend for in this
 *   build (macOS, FreeBSD, … — see src/ext/xmake.lua) every open fails as well,
 *   so is_open() is false and write() returns 0 there.
 */
#pragma once

#include "dll_export.h"
#include "basic_traits.h"
#include "stl/string.h"
#include "stl/vector.h"

#include <cstddef>
#include <cstdint>
#include <span>

// Opaque xNVMe device/file handle (src/ext/xnvme/include/libxnvme_dev.h).
struct xnvme_dev;

namespace kimix {

// ---------------------------------------------------------------------------
// BinaryFileWriter — positioned writes through xNVMe
// ---------------------------------------------------------------------------

class KIMIX_CORE_API BinaryFileWriter {
public:
    BinaryFileWriter() noexcept;

    // Opens 'path' for writing (created when missing). 'truncate' == true empties
    // an existing file; 'truncate' == false keeps its content, which is required
    // for positioned overwrites. A missing/unopenable file leaves the writer
    // closed instead of throwing.
    explicit BinaryFileWriter(const string &path, bool truncate = true) noexcept;

    ~BinaryFileWriter();

    BinaryFileWriter(const BinaryFileWriter &) = delete;
    BinaryFileWriter &operator=(const BinaryFileWriter &) = delete;

    BinaryFileWriter(BinaryFileWriter &&other) noexcept;
    BinaryFileWriter &operator=(BinaryFileWriter &&other) noexcept;

    // Writes 'size' bytes at the current position and advances it; returns the
    // bytes written (== size, the write is all-or-nothing) or 0 when the writer
    // is closed, the buffer is NULL or the write failed.
    size_t write(const void *buffer, size_t size) noexcept;

    // Same, from a span (the read side uses std::span<byte>; a writer takes a
    // read-only view).
    size_t write(std::span<const byte> buffer) noexcept;

    // Absolute position the next write goes to ('pos' beyond the current end is
    // allowed; the write then extends the file).
    void set_pos(uint64_t pos) noexcept;

    uint64_t position() const noexcept;

    // Flush the written data to stable storage (XNVME_SPEC_FS_OPC_FLUSH:
    // fsync()/FlushFileBuffers()); false when closed or on failure.
    bool sync() noexcept;

    // Closes the handle and resets the position to 0. Idempotent, and implicitly
    // called by the destructor. Note that close() does not sync: data already
    // handed to the OS survives it, only the durability guarantee needs sync().
    void close() noexcept;

    bool is_open() const noexcept;
    explicit operator bool() const noexcept;

private:
    xnvme_dev *_dev;
    uint64_t _pos;
};

// ---------------------------------------------------------------------------
// DeferredBinaryWriter — batched positioned writes through xNVMe
// ---------------------------------------------------------------------------

class KIMIX_CORE_API DeferredBinaryWriter {
public:
    DeferredBinaryWriter() noexcept;
    ~DeferredBinaryWriter();

    DeferredBinaryWriter(const DeferredBinaryWriter &) = delete;
    DeferredBinaryWriter &operator=(const DeferredBinaryWriter &) = delete;

    // Move transfers the pending command list (and with it the ownership of
    // every payload); the moved-from object is empty.
    DeferredBinaryWriter(DeferredBinaryWriter &&other) noexcept;
    DeferredBinaryWriter &operator=(DeferredBinaryWriter &&other) noexcept;

    // Records one positioned write and takes OWNERSHIP of 'data' (moved into the
    // command list — pass std::move(data)). The bytes stay alive inside the
    // writer until commit()/clear() returns. No file is opened and no I/O happens
    // here.
    void add_write(const string &path, uint64_t offset, vector<std::byte> &&data);

    // Number of recorded writes (0 after clear() or commit()).
    size_t pending_count() const noexcept;

    // Drops every recorded write, releasing the owned payloads, without any I/O.
    void clear() noexcept;

    // Groups the recorded writes by path, opens each distinct path once for
    // writing (created if missing, NEVER truncated: offset writes into an
    // existing file must keep the other content), writes every command at its
    // offset, syncs and closes the file. ALWAYS clears the recorded writes
    // afterwards (releasing the payload vectors).
    // Returns the number of writes that completed fully; failed and unopenable
    // paths count as failures but do not stop the other commands.
    size_t commit();

private:
    struct Command {
        string path;
        uint64_t offset;
        vector<std::byte> data;
    };

    vector<Command> _commands;
};

} // namespace kimix
