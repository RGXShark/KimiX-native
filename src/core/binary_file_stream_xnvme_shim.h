/*
 * binary_file_stream_xnvme_shim.h — C ABI between the MSVC-compiled
 * kimix::XnvmeBinaryFileStream (binary_file_stream_xnvme.cpp) and the
 * MinGW-compiled xnvme C library.
 *
 * WHY A SHIM EXISTS AT ALL:
 *   xnvme's public headers cannot be parsed by MSVC: libxnvme_spec.h uses
 *   __attribute__((packed)) / __packed__ and libxnvme_util.h uses
 *   clock_gettime() / CLOCK_MONOTONIC — none of which MSVC knows (upstream
 *   builds xnvme with MinGW/MSYS2, which is exactly what the kimix-xnvme target
 *   does here; MSVC fails with C2065/C2079/C3861 the moment <libxnvme.h> is
 *   included). The class itself must still be compiled by MSVC on Windows,
 *   because its C++ member symbols are consumed by MSVC-built modules
 *   (kimix-llm, kimix-cli, runtime_py, kimix_api, the unit tests).
 *
 *   So the xnvme call layer lives in binary_file_stream_xnvme_shim.c, compiled
 *   with the same toolchain as xnvme itself, and only these plain C entry
 *   points cross into the MSVC side. Nothing struct-shaped is passed across the
 *   boundary: no xnvme struct layout and no field offset is re-declared (and
 *   possibly mis-declared) by hand — every offset/size is checked by the
 *   compiler against the real xnvme headers in the shim's translation unit.
 *
 * Contract (C linkage, non-throwing, no allocation):
 *
 *   --- Phase 2: the single-command read channel ----------------------------
 *   kimix_xnvme_stream_open(path)     -> opaque handle, or NULL on error
 *                                        (missing/unsupported/unopenable path).
 *                                        The file is opened READ-ONLY.
 *   kimix_xnvme_stream_pread(...)     -> bytes read (>= 0) or negative -errno;
 *                                        byte-granular, like pread(2), at any
 *                                        byte offset of the file.
 *   kimix_xnvme_stream_close(handle)  -> 0, or negative -errno.
 *   The handle is owned by the caller (kimix::XnvmeBinaryFileStream) and is
 *   closed exactly once; NULL handles are accepted and ignored/reported.
 *
 *   --- Phase 4: the batched read channel (kimix::DeferredBinaryFileStream) --
 *   kimix_xnvme_breader_open(path)          -> opaque reader, or NULL on
 *                                              failure (NULL/empty path, missing
 *                                              file). The file is opened
 *                                              READ-ONLY, exactly like
 *                                              kimix_xnvme_stream_open().
 *   kimix_xnvme_breader_add(r, buf, off, n)  -> record one positioned read of 'n'
 *                                              bytes at 'off' into 'buf';
 *                                              0 on success, negative -errno on a
 *                                              rejected record (NULL reader,
 *                                              NULL buffer with n != 0). 'buf' is
 *                                              CALLER-OWNED and must stay valid
 *                                              and unmodified until commit()
 *                                              returns. The recorded request list
 *                                              is a plain malloc/realloc array
 *                                              owned by the reader.
 *   kimix_xnvme_breader_commit(r)            -> dispatches every recorded read,
 *                                              waits for all of them, and returns
 *                                              the number that completed FULLY
 *                                              (short reads and errors count as
 *                                              failures). The recorded list is
 *                                              reset, so the reader is reusable.
 *                                              Reads are grouped by the caller
 *                                              (one reader per distinct path).
 *                                              The reads of a batch complete
 *                                              SERIALLY on both transports (no
 *                                              read is in flight while another
 *                                              one is — see below).
 *   kimix_xnvme_breader_close(r)             -> close the file and free the
 *                                              reader (NULL is a no-op).
 *
 *   --- Phase 5: the write channel (kimix::BinaryFileWriter) -----------------
 *   kimix_xnvme_open_write(path, truncate) -> opaque handle, or NULL. Opens (and
 *                                              creates) the file for writing.
 *                                              truncate != 0 empties an existing
 *                                              file; truncate == 0 PRESERVES
 *                                              existing content (positioned
 *                                              writes then extend/overwrite it).
 *   kimix_xnvme_pwrite(handle, buf, n, off) -> 0 when exactly 'n' bytes landed at
 *                                              'off'; negative -errno otherwise.
 *                                              A zero-length write is a no-op
 *                                              that reports success.
 *   kimix_xnvme_sync(handle)               -> 0, or negative -errno
 *                                              (xnvme_file_sync / fsync /
 *                                              FlushFileBuffers).
 *
 * The pread contract above holds on every platform, but on Windows it is NOT
 * served by xnvme_file_pread(): the vendored xnvme revision drops the offset of
 * file-system reads there, so the shim reads from the handle xnvme opened
 * instead. The bug, its evidence and the one-line upstream fix are documented
 * in binary_file_stream_xnvme_shim.c (_win_pread). The write side deliberately
 * uses the same handle-level mechanism (_win_pwrite) so read and write behave
 * symmetrically and neither depends on the vendored revision's read path.
 *
 * PLATFORM LIMITS (both inherited by the C++ classes — see their headers):
 *   * Windows: a file of 2 GiB or more cannot be opened at all — the vendored
 *     xnvme open path uses a 32-bit stat — so kimix_xnvme_stream_open() and
 *     kimix_xnvme_breader_open() return NULL. Linux has no such limit.
 *   * Every platform that is neither _WIN32 nor __linux__ (macOS, FreeBSD, …):
 *     xnvme's backends are only wired for Linux and Windows in this build (see
 *     src/ext/xmake.lua), so all entry points below are stubs — opens fail
 *     (NULL), positioned reads/writes report -ENOSYS and a commit completes
 *     nothing.
 *
 * BATCHED READS ARE NOT PARALLEL: neither transport overlaps two reads.
 *   * Linux: the batch is submitted to xnvme's async command-queue, but the only
 *     async backend enabled for a regular file is xnvme's emulated one, which
 *     completes the queued commands serially inside xnvme_queue_drain().
 *   * Windows: the batch is a sequence of positioned ReadFile() calls on the
 *     synchronous handle xnvme opened (not FILE_FLAG_OVERLAPPED), i.e. one
 *     blocking call after another.
 *   A batch groups reads of the same path (one open/close per path, one
 *   submission loop) and amortizes that bookkeeping; it does not shorten the
 *   wall-clock time the transfers take.
 */
#ifndef KIMIX_CORE_BINARY_FILE_STREAM_XNVME_SHIM_H
#define KIMIX_CORE_BINARY_FILE_STREAM_XNVME_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *kimix_xnvme_stream_open(const char *path);

long long kimix_xnvme_stream_pread(void *handle, void *buffer, size_t count,
                                   long long offset);

int kimix_xnvme_stream_close(void *handle);

/*
 * Batched read channel (Phase 4). The reader is opaque to the caller; only the
 * incomplete type below ever crosses the boundary.
 */
struct kimix_xnvme_breader;

struct kimix_xnvme_breader *kimix_xnvme_breader_open(const char *path);

int kimix_xnvme_breader_add(struct kimix_xnvme_breader *reader, void *buffer,
                            uint64_t offset, size_t size);

size_t kimix_xnvme_breader_commit(struct kimix_xnvme_breader *reader);

void kimix_xnvme_breader_close(struct kimix_xnvme_breader *reader);

/*
 * Write channel (Phase 5). The handle type is void * — the same opaque form the
 * read trio uses — so the C++ side never re-declares an xnvme type.
 */
void *kimix_xnvme_open_write(const char *path, int truncate);

int kimix_xnvme_pwrite(void *handle, const void *buffer, size_t count,
                       long long offset);

int kimix_xnvme_sync(void *handle);

#ifdef __cplusplus
}
#endif

#endif /* KIMIX_CORE_BINARY_FILE_STREAM_XNVME_SHIM_H */
