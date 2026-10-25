/*
 * binary_file_stream_xnvme.h — kimix::XnvmeBinaryFileStream: an xNVMe-backed
 * high-performance binary file reader mirroring kimix::BinaryFileStream.
 *
 * kimix::XnvmeBinaryFileStream:
 *   Opens a file by path (const kimix::string&) read-only through xNVMe
 *   (xnvme_file_open) and reads it byte-granularly with xnvme_file_pread().
 *   Default-constructed streams are not open.
 *
 *   Methods:
 *     length()        — file size captured at open (0 when unknown/closed)
 *     position()      — current read position (0 at start)
 *     read(span)      — read into a std::span<byte>, returns bytes read
 *     read(ptr, sz)   — read into a raw buffer, returns bytes read
 *     read_all()      — read the whole file (from offset 0) into vector<byte>
 *     set_pos(pos)    — absolute seek to a byte offset
 *     close()         — close the handle, resetting position and length
 *     is_open()       — true while a file handle is open
 *
 *   `byte` is kimix::byte (== int8_t), exactly as in kimix::BinaryFileStream —
 *   core file I/O speaks kimix::byte.
 *
 *   Move semantics: moving transfers the xnvme_dev handle; the moved-from
 *   stream becomes closed. Opening a non-existent file (or an empty path)
 *   leaves the stream closed (is_open() == false) instead of throwing.
 *   read_all() leaves the position at the end of the data it read, matching
 *   kimix::BinaryFileStream::read_all().
 *
 *   LIMITS — WINDOWS: a file of 2 GiB or more cannot be opened at all, because
 *   the vendored xnvme open path stats the file through a 32-bit stat, so the
 *   open fails and is_open() is false; Linux has no such limit (the same code
 *   opens multi-GiB files and pread(2) addresses any 64-bit offset).
 *
 *   LIMITS — NO WIRED BACKEND: on a platform xnvme has no backend for in this
 *   build (macOS, FreeBSD, … — see src/ext/xmake.lua) every open fails as well,
 *   so is_open() is false and read() returns 0 there.
 *
 *   WHY THIS IS A COMPILED HEADER/SOURCE PAIR, UNLIKE kimix::BinaryFileStream:
 *   binary_file_stream.h is header-inline ON PURPOSE — the FILE* and every
 *   block read_all() returns must be created and consumed inside one module,
 *   because each module that archives kimix-core links its own CRT.
 *
 *   This class cannot follow that pattern, because what it wraps is not the CRT
 *   but the xNVMe library: the vendored xnvme sources are compiled once and
 *   linked into kimix-core (kimix-core.dll / libkimix-core.so). An inline
 *   implementation would make every consumer module resolve xnvme_file_open /
 *   xnvme_file_close / xnvme_file_pread itself, and every module pulling in its
 *   own copy of the static archive would instantiate xnvme's process-global
 *   state (backend tables, the device registry, per-backend init) more than
 *   once — handles created in one module would be unknown to another, and
 *   xnvme's global initialization would run repeatedly.
 *
 *   Out-of-line + KIMIX_CORE_API instead means: the xnvme code is statically
 *   linked exactly once (inside kimix-core), the single definition of
 *   XnvmeBinaryFileStream lives there as well, and open/close/read always
 *   execute against that one copy of xnvme — no matter which module owns the
 *   stream object or where the path it was opened with came from.
 *
 *   The xnvme C API stays an implementation detail: this public header never
 *   includes <libxnvme.h>, so consumers need neither xnvme's include path nor
 *   its C headers, and the opaque `struct xnvme_dev` remains forward-declared
 *   here. The .cpp does not include it either — xnvme's headers are GCC-only,
 *   while the .cpp is compiled by kimix-core's toolchain (MSVC on Windows), so
 *   the xnvme calls go through the small C ABI of
 *   binary_file_stream_xnvme_shim.h, whose .c side is compiled with xnvme's own
 *   toolchain (see the kimix-xnvme-shim target in src/xmake.lua).
 *
 * Example:
 *   kimix::XnvmeBinaryFileStream stream("data.bin");
 *   if (!stream) { /* handle error *\/ }
 *   auto data = stream.read_all();
 *   stream.close();
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
// Only binary_file_stream_xnvme.cpp includes the xnvme headers.
struct xnvme_dev;

namespace kimix {

// ---------------------------------------------------------------------------
// XnvmeBinaryFileStream — reads binary files through xNVMe
// ---------------------------------------------------------------------------

class KIMIX_CORE_API XnvmeBinaryFileStream {
public:
    XnvmeBinaryFileStream() noexcept;

    // Opens 'path' read-only (xnvme_opts_default() with opts.rdonly set).
    // A missing/unopenable file leaves the stream closed.
    explicit XnvmeBinaryFileStream(const string &path) noexcept;

    XnvmeBinaryFileStream(const XnvmeBinaryFileStream &) = delete;
    XnvmeBinaryFileStream &operator=(const XnvmeBinaryFileStream &) = delete;

    XnvmeBinaryFileStream(XnvmeBinaryFileStream &&other) noexcept;
    XnvmeBinaryFileStream &operator=(XnvmeBinaryFileStream &&other) noexcept;

    ~XnvmeBinaryFileStream();

    // Sequential read at the current position; returns the bytes read (0 when
    // the stream is closed, the buffer/size is empty or xnvme reports an error).
    size_t read(std::span<byte> buffer) noexcept;

    // Read data into a pre-allocated buffer
    size_t read(void *buffer, size_t size) noexcept;

    // Read the entire file from offset 0 in one pread and return the bytes
    // actually read (the vector is resized to that count); the position ends up
    // after the data that was read. Returns an empty vector for a closed stream
    // or an empty file.
    vector<byte> read_all();

    // Absolute seek to a byte offset ('pos' beyond length() is allowed; reads
    // from there return 0 bytes).
    void set_pos(uint64_t pos) noexcept;

    uint64_t position() const noexcept;

    // File size captured at open; 0 when closed or when the size could not be
    // queried.
    size_t length() const noexcept;

    // xnvme_file_close() + reset (position and length go back to 0)
    void close() noexcept;

    bool is_open() const noexcept;
    explicit operator bool() const noexcept;

private:
    xnvme_dev *_dev;
    uint64_t _pos;
    size_t _length;
};

} // namespace kimix
