/*
 * deferred_binary_file_stream.h — kimix::DeferredBinaryFileStream: records
 * positioned reads and executes them in one batch through xNVMe.
 *
 * kimix::DeferredBinaryFileStream:
 *   The batched counterpart of kimix::XnvmeBinaryFileStream: instead of reading
 *   one buffer at a time, the caller RECORDS reads with add_read() and commits
 *   them all at once. commit() groups the commands by path, opens every distinct
 *   path once (read-only), dispatches that path's reads as one batch, waits for
 *   every completion and closes the files again.
 *
 *   WHAT "BATCHED" ACTUALLY MEANS (no parallelism is promised or delivered for
 *   regular files in this build):
 *     * Linux: the batch goes to xnvme's async command-queue, but the only async
 *       backend enabled for a regular file here is xnvme's EMULATED one (see the
 *       backend table in src/ext/xmake.lua). Emulation queues the commands and
 *       then executes them SERIALLY — one xnvme_file_pread() (i.e. pread(2)) at a
 *       time while the queue is drained — so a batch saves the per-read
 *       submission bookkeeping, not wall-clock parallelism. Real asynchronous
 *       I/O needs an NVMe device with a backend such as io_uring or libaio, which
 *       this build does not enable.
 *     * Windows: the batch is a plain sequence of positioned ReadFile() calls on
 *       the synchronous handle xnvme opened, one after another (that handle is
 *       NOT opened with FILE_FLAG_OVERLAPPED).
 *   So commit() groups and amortizes; it does not run the reads concurrently.
 *
 *   Methods:
 *     add_read(path, offset, size, dst) — record one positioned read
 *     pending_count()                   — number of recorded reads
 *     clear()                           — drop the recorded reads (no I/O)
 *     commit()                          — dispatch + wait + clear; returns the
 *                                         number of reads that completed FULLY
 *
 *   OWNERSHIP OF THE DESTINATION BUFFERS: 'dst' is CALLER-OWNED. The caller must
 *   keep the memory valid AND unmodified until commit() returns — the reads are
 *   issued at commit() time, not at add_read() time. clear() and commit() never
 *   touch the buffers themselves; they only drop the commands referencing them.
 *
 *   FAILURES DO NOT ABORT THE BATCH: a missing/unopenable file, a rejected
 *   command or a short read simply does not count as completed — the other
 *   commands still run. commit() therefore returns
 *       pending count before the call   on full success,
 *       less than that                  when some reads failed.
 *
 *   Example:
 *     kimix::DeferredBinaryFileStream stream;
 *     kimix::vector<kimix::byte> a(4096), b(1024);
 *     stream.add_read("data.bin", 12345, a.size(), a.data());
 *     stream.add_read("other.bin", 0, b.size(), b.data());
 *     if (stream.commit() == 2) { ... }
 *
 *   LIMITS — WINDOWS: a file of 2 GiB or more cannot be opened at all, because
 *   the vendored xnvme open path stats the file through a 32-bit stat, so every
 *   read recorded for it fails (is_open() on the underlying handle is false);
 *   Linux has no such limit.
 *
 *   LIMITS — NO WIRED BACKEND: on a platform xnvme has no backend for in this
 *   build (macOS, FreeBSD, … — see src/ext/xmake.lua) no path can be opened, so
 *   every open fails and commits complete nothing.
 *
 *   WHY THIS IS A COMPILED HEADER/SOURCE PAIR, LIKE kimix::XnvmeBinaryFileStream:
 *   what it wraps is not the CRT but the xNVMe library, which is compiled once
 *   and linked into kimix-core (see the long rationale in
 *   binary_file_stream_xnvme.h). The xnvme C API stays an implementation detail:
 *   this header never includes <libxnvme.h>, and struct xnvme_dev / the batched
 *   reader stay opaque here — the xnvme calls live in the C ABI shim
 *   (binary_file_stream_xnvme_shim.h), whose .c side is compiled with xnvme's own
 *   toolchain.
 *
 *   NOT part of <core/kimix_core.h>, exactly like kimix::XnvmeBinaryFileStream:
 *   an xNVMe-backed feature is opt-in at the include site.
 */
#pragma once

#include "dll_export.h"
#include "stl/string.h"
#include "stl/vector.h"

#include <cstddef>
#include <cstdint>

namespace kimix {

// ---------------------------------------------------------------------------
// DeferredBinaryFileStream — batched positioned reads through xNVMe
// ---------------------------------------------------------------------------

class KIMIX_CORE_API DeferredBinaryFileStream {
public:
    DeferredBinaryFileStream() noexcept;
    ~DeferredBinaryFileStream();

    DeferredBinaryFileStream(const DeferredBinaryFileStream &) = delete;
    DeferredBinaryFileStream &operator=(const DeferredBinaryFileStream &) = delete;

    // Move transfers the pending command list; the moved-from object is empty.
    DeferredBinaryFileStream(DeferredBinaryFileStream &&other) noexcept;
    DeferredBinaryFileStream &operator=(DeferredBinaryFileStream &&other) noexcept;

    // Records one positioned read: 'size' bytes of 'path' at 'offset' into 'dst'.
    // 'dst' is caller-owned and must stay valid and unmodified until commit()
    // returns. No file is opened and no I/O happens here.
    void add_read(const string &path, uint64_t offset, size_t size, void *dst);

    // Number of recorded reads (0 after clear() or commit()).
    size_t pending_count() const noexcept;

    // Drops every recorded read without touching the 'dst' buffers and without
    // any I/O (the files were never opened).
    void clear() noexcept;

    // Groups the recorded reads by path, opens each distinct path once
    // (read-only), dispatches the reads of each path as one batch (xnvme's
    // emulated async queue on Linux, a sequence of positioned ReadFile() calls on
    // Windows — serially completed in both cases, see the header comment), waits
    // for all completions and closes the files. ALWAYS clears the recorded reads
    // afterwards.
    // Returns the number of reads that completed fully (i.e. that transferred
    // exactly the recorded byte count); failed and missing paths count as
    // failures but do not stop the other commands.
    size_t commit();

private:
    struct Command {
        string path;
        uint64_t offset;
        size_t size;
        void *dst;
    };

    vector<Command> _commands;
};

} // namespace kimix
