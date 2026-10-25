/*
 * binary_file_stream_xnvme_shim.c — xnvme call layer of
 * kimix::XnvmeBinaryFileStream (Phase 2), kimix::DeferredBinaryFileStream
 * (Phase 4) and kimix::BinaryFileWriter (Phase 5).
 *
 * Compile target: kimix-xnvme-shim (src/xmake.lua), which uses the SAME
 * toolchain as the vendored xnvme library itself (MinGW on Windows) — the only
 * toolchain that can include <libxnvme.h> (see the header comment in
 * binary_file_stream_xnvme_shim.h for the full rationale).
 *
 * Everything xnvme-shaped stays in this translation unit: the option struct,
 * the synchronous command context, the asynchronous command-queue and its
 * completion result are declared by the real xnvme headers here, so the
 * MSVC-compiled C++ side only ever sees pointers, 64-bit integers and sizes.
 *
 * PLATFORM SPLIT (three-way, explicit):
 *   * _WIN32      — the positioned reads and writes are issued against the file
 *                   handle that xnvme opened instead of through
 *                   xnvme_file_pread() — see _win_pread() below for the
 *                   verified upstream bug this works around. The batched read
 *                   channel is a plain sequence of positioned ReadFile()s.
 *   * __linux__   — reads and writes go through xnvme_file_pread() /
 *                   xnvme_file_pwrite() exactly as the C API documents them,
 *                   and the batched read channel runs on the xnvme async
 *                   command-queue.
 *   * everything  — macOS, FreeBSD, … : STUBS. xnvme's backends are only wired
 *     else          for Linux and Windows in this build (see src/ext/xmake.lua),
 *                   so every entry point below reports "no backend": open
 *                   fails (NULL handle → is_open() == false), positioned reads
 *                   and writes report -ENOSYS and a commit completes nothing.
 *                   The archive stays buildable there instead of compiling
 *                   calls into a backend that does not exist.
 */
#include "binary_file_stream_xnvme_shim.h"

#include <errno.h>
#include <libxnvme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    /* xnvme_dev_get_be_state() -> struct xnvme_be_windows_state, i.e. the
       HANDLE (and CRT fd) xnvme opened for this file. Shipped with the vendored
       xnvme headers, but declared in an internal one
       (__INTERNAL_XNVME_BE_WINDOWS_H) — the workaround below is pinned to the
       vendored revision and must be re-checked when the submodule moves. */
    #include <xnvme_be_windows.h>
    #include <windows.h>
#endif

/* Maximum number of reads in flight at once (Phase 4). xnvme_queue_init()
   requires a POWER-OF-TWO capacity in [1, 4096), so 256 is both a legal queue
   depth and the chunk size the batched channel works in: a commit() of 10k
   recorded reads still needs a 256-entry queue, not a 10k-entry one. */
#define KIMIX_XNVME_BREADER_QD 256

void *kimix_xnvme_stream_open(const char *path)
{
#if defined(_WIN32) || defined(__linux__)
    struct xnvme_opts opts;

    if (!path || !path[0]) {
        return NULL;
    }

    /* xnvme_opts_default() returns rdwr = 1, and the backend ORs the mode flags
       together (xnvme_be_windows.c: GENERIC_WRITE | GENERIC_READ), so a reader
       must clear rdwr as well as set rdonly — setting rdonly alone would still
       request write access and fail on a read-only file. */
    opts = xnvme_opts_default();
    opts.rdwr = 0;
    opts.rdonly = 1;

    /* xnvme_file_open() -> xnvme_dev_open(): NULL on any failure (missing file,
       unsupported URI/backend, permission problem) instead of an error code. */
    return xnvme_file_open(path, &opts);
#else
    /* No wired xnvme backend on this platform: every open fails, so is_open()
       is false there (see the platform split at the top of this file). */
    (void)path;
    return NULL;
#endif
}

#ifdef _WIN32

/*
 * The file HANDLE xnvme opened for this device.
 *
 * state->sync_handle is created WITHOUT FILE_FLAG_OVERLAPPED (see
 * xnvme_be_windows_dev.c), which is what makes the positioned ReadFile /
 * WriteFile calls below usable: with a non-NULL OVERLAPPED and a synchronous
 * handle, Windows starts the transfer at OVERLAPPED.Offset/OffsetHigh, blocks
 * until it completed, and reports the transferred byte count in the out
 * parameter (MSDN: "Considerations for working with synchronous file handles").
 */
static HANDLE _win_handle(struct xnvme_dev *dev)
{
    const struct xnvme_be_windows_state *state = xnvme_dev_get_be_state(dev);

    if (!state || !state->sync_handle) {
        return NULL;
    }

    return (HANDLE)state->sync_handle;
}

static void _win_overlapped(OVERLAPPED *ovl, long long offset)
{
    memset(ovl, 0, sizeof(*ovl));
    ovl->Offset = (DWORD)((uint64_t)offset & 0xFFFFFFFFull);
    ovl->OffsetHigh = (DWORD)((uint64_t)offset >> 32);
}

/*
 * Positioned read on the handle xnvme opened for this file.
 *
 * WORKAROUND for a verified bug in the vendored xnvme revision (post-v0.7.5,
 * src/ext/xnvme): the file-system read opcode never seeds the overlapped
 * offset. lib/xnvme_be_windows_fs.c contains
 *
 *     OVERLAPPED overlapped;
 *     memset(&overlapped, 0, sizeof(OVERLAPPED));
 *     ...
 *     case XNVME_SPEC_FS_OPC_WRITE:
 *             overlapped.Offset = ctx->cmd.nvm.slba;            <- write sets it
 *     case XNVME_SPEC_FS_OPC_READ:
 *             if (!ReadFile(state->sync_handle, dbuf, dbuf_nbytes, &res, &overlapped))
 *
 * — the read case passes a zeroed OVERLAPPED to ReadFile(), and Windows starts
 * every read at the offset found there. The measured effect is that
 * xnvme_file_pread() ignores its offset argument on Windows: EVERY call returns
 * bytes from offset 0 of the file (a byte-level probe showed a read at offset
 * 12345 returning 0x07 0x26 0x45 0x64 = the file's first bytes, and a read at
 * EOF returning a full buffer). That silently corrupts repeated and positioned
 * reads, i.e. the whole contract of this stream class; it is an upstream bug,
 * not something kimix can fix inside src/ext.
 *
 * The one-line upstream fix is to set overlapped.Offset / overlapped.OffsetHigh
 * from ctx->cmd.nvm.slba in the XNVME_SPEC_FS_OPC_READ case, exactly like the
 * write case does. Until that lands, read against the same handle xnvme opened
 * (a plain, byte-granular positioned ReadFile); everything else — open, backend
 * selection, geometry, lifetime, close — still goes through xnvme.
 *
 * DELETE THIS FUNCTION once the read path is fixed: xnvme_file_pread() is the
 * intended call, and it is what every other platform uses.
 */
static long long _win_pread(struct xnvme_dev *dev, void *buffer, size_t count, long long offset)
{
    HANDLE handle = _win_handle(dev);
    OVERLAPPED ovl;
    DWORD res = 0;

    if (!handle) {
        return -EIO;
    }
    /* ReadFile() takes a 32-bit length; larger requests (not produced by this
       class today) must be split by the caller. */
    if (offset < 0 || count > 0xFFFFFFFFull) {
        return -EOVERFLOW;
    }

    _win_overlapped(&ovl, offset);
    if (!ReadFile(handle, buffer, (DWORD)count, &res, &ovl)) {
        return -(long long)GetLastError();
    }

    return (long long)res;
}

/*
 * Positioned write on the same handle.
 *
 * The vendored xnvme write path DOES seed overlapped.Offset (see the excerpt
 * above), so xnvme_file_pwrite() is not known to be broken on Windows; the shim
 * still issues its own WriteFile so that reads and writes travel the same code
 * path, behave symmetrically (both are byte-granular and both report a short
 * transfer) and do not depend on the vendored revision's file-system opcodes.
 */
static int _win_pwrite(struct xnvme_dev *dev, const void *buffer, size_t count, long long offset)
{
    HANDLE handle = _win_handle(dev);
    OVERLAPPED ovl;
    DWORD res = 0;

    if (!handle) {
        return -EIO;
    }
    if (offset < 0 || count > 0xFFFFFFFFull) {
        return -EOVERFLOW;
    }

    _win_overlapped(&ovl, offset);
    if (!WriteFile(handle, buffer, (DWORD)count, &res, &ovl)) {
        return -(int)GetLastError();
    }
    if (res != (DWORD)count) {
        return -EIO;
    }

    return 0;
}

#endif /* _WIN32 */

long long kimix_xnvme_stream_pread(void *handle, void *buffer, size_t count, long long offset)
{
    struct xnvme_dev *dev = handle;

    if (!dev || !buffer || !count) {
        return 0;
    }

#if defined(_WIN32)
    return _win_pread(dev, buffer, count, offset);
#elif defined(__linux__)
    struct xnvme_cmd_ctx ctx;
    int err;

    /* off_t is the width the OS offers here (64-bit on Linux). Report an offset
       it cannot express instead of truncating it and reading the wrong bytes. */
    if ((long long)(off_t)offset != offset) {
        return -EOVERFLOW;
    }

    /* xnvme_file_pread() is byte-granular but, unlike pread(2), does not return
       the transfer length: that lands in ctx.cpl.result. It returns 0 on
       success and -errno on failure.

       errno is cleared first, and the COMPLETION is what this function trusts:
       xnvme's synchronous backend classifies a transfer as failed by comparing
       the count with the request and then reading ERRNO (psync:
       `sc = res != nbytes ? errno : 0`). A short read at the tail is a SUCCESS
       for pread(2) — it returns the short count — but with a stale non-zero
       errno left over from an earlier call that heuristic turns it into the
       error -errno with ctx.cpl.result zeroed, so the shim used to report a
       failed read AT THE TAIL instead of the short count. Clearing errno makes
       the short read land in ctx.cpl.result, and the result is returned as long
       as xnvme did not zero it while reporting an error. */
    ctx = xnvme_file_get_cmd_ctx(dev);
    errno = 0;
    err = xnvme_file_pread(&ctx, buffer, count, (off_t)offset);
    if (err && !ctx.cpl.result) {
        return (long long)err;
    }

    return (long long)ctx.cpl.result;
#else
    (void)offset;
    return -ENOSYS; /* no wired xnvme backend on this platform */
#endif
}

int kimix_xnvme_stream_close(void *handle)
{
    if (!handle) {
        return 0;
    }

#if defined(_WIN32) || defined(__linux__)
    return xnvme_file_close(handle);
#else
    return 0; /* the stub open never produced a handle; nothing to close */
#endif
}

/* ===========================================================================
 * Phase 4: batched read channel
 * ======================================================================== */

struct kimix_breader_req {
    void *buf;
    uint64_t offset;
    size_t size;
    int ok; ///< 1 once the read has been observed to complete FULLY
};

struct kimix_xnvme_breader {
    struct xnvme_dev *dev;
    struct kimix_breader_req *reqs; ///< malloc/realloc-owned request array
    size_t count;                   ///< number of recorded reads
    size_t capacity;                ///< allocated entries in 'reqs'
};

struct kimix_xnvme_breader *kimix_xnvme_breader_open(const char *path)
{
    if (!path || !path[0]) {
        return NULL;
    }

#if defined(_WIN32) || defined(__linux__)
    struct kimix_xnvme_breader *reader = calloc(1, sizeof(*reader));

    if (!reader) {
        return NULL;
    }

    /* Read-only, exactly like the single-command read channel. */
    reader->dev = kimix_xnvme_stream_open(path);
    if (!reader->dev) {
        free(reader);
        return NULL;
    }

    return reader;
#else
    return NULL; /* no wired xnvme backend on this platform */
#endif
}

int kimix_xnvme_breader_add(struct kimix_xnvme_breader *reader, void *buffer, uint64_t offset,
                            size_t size)
{
    struct kimix_breader_req *grown;
    size_t capacity;

    if (!reader) {
        return -EINVAL;
    }
    if (!buffer && size) {
        return -EINVAL;
    }

    if (reader->count == reader->capacity) {
        capacity = reader->capacity ? reader->capacity * 2 : 64;
        grown = realloc(reader->reqs, capacity * sizeof(*grown));
        if (!grown) {
            return -ENOMEM;
        }
        reader->reqs = grown;
        reader->capacity = capacity;
    }

    memset(&reader->reqs[reader->count], 0, sizeof(reader->reqs[reader->count]));
    reader->reqs[reader->count].buf = buffer;
    reader->reqs[reader->count].offset = offset;
    reader->reqs[reader->count].size = size;
    reader->count += 1;

    return 0;
}

#if defined(__linux__)

/*
 * Completion callback of one batched read.
 *
 * The per-command argument is the request slot itself (set with
 * xnvme_cmd_ctx_set_cb()), so completion is recorded where the byte count it
 * must match is known. ctx->cpl.result is the transfer length — xnvme's
 * file-system commands report the byte count there instead of as a return value
 * — and it is only meaningful when the completion status is clean. Returning
 * the command-context to the queue from the callback is what the vendored
 * xnvme examples do (examples/xnvme_io_async.c: cb_pool()).
 */
static void _breader_cb(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
    struct kimix_breader_req *req = cb_arg;

    req->ok = (!xnvme_cmd_ctx_cpl_status(ctx) && ctx->cpl.result == (uint64_t)req->size) ? 1 : 0;

    xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

/*
 * Dispatch every recorded read through one xnvme command-queue.
 *
 * Follows examples/xnvme_io_async.c: one queue per batch, chunked so that no
 * more than xnvme_queue_get_capacity() commands are ever outstanding, the
 * documented -EBUSY/-EAGAIN retry with xnvme_queue_poke(), and drain (not the
 * deprecated xnvme_queue_wait(), which only prints a warning and forwards to
 * drain) to reap the completions. The queue is torn down here even on the
 * failure paths.
 */
static size_t _breader_commit_queue(struct kimix_xnvme_breader *reader)
{
    struct xnvme_queue *queue = NULL;
    size_t completed = 0;
    size_t chunk;
    int err;

    if (xnvme_queue_init(reader->dev, (uint16_t)KIMIX_XNVME_BREADER_QD, 0, &queue)) {
        /* No queue, no completions: every recorded read counts as failed. */
        return 0;
    }

    for (chunk = 0; chunk < reader->count; chunk += KIMIX_XNVME_BREADER_QD) {
        size_t nslots = reader->count - chunk;
        size_t slot;

        if (nslots > KIMIX_XNVME_BREADER_QD) {
            nslots = KIMIX_XNVME_BREADER_QD;
        }

        for (slot = 0; slot < nslots; ++slot) {
            struct kimix_breader_req *req = &reader->reqs[chunk + slot];
            struct xnvme_cmd_ctx *ctx;
            int tries;

            if ((uint64_t)(off_t)req->offset != req->offset) {
                continue; /* offset the local off_t cannot express */
            }

            ctx = xnvme_queue_get_cmd_ctx(queue);
            if (!ctx) {
                continue; /* unreachable: the pool holds capacity + 1 contexts */
            }
            xnvme_cmd_ctx_set_cb(ctx, _breader_cb, req);

            for (tries = 0; tries < 64; ++tries) {
                err = xnvme_file_pread(ctx, req->buf, req->size, (off_t)req->offset);
                if (err != -EBUSY && err != -EAGAIN) {
                    break;
                }
                /* Queue full: reap completions, then re-submit. */
                xnvme_queue_poke(queue, 0);
            }
            if (err) {
                xnvme_queue_put_cmd_ctx(queue, ctx);
                continue;
            }
        }

        /* Wait until every command of this chunk has completed.

           xnvme_queue_drain() returns the number of completions it reaped (0
           when the queue was empty) and a negative -errno when the backend could
           not report completions. On failure the wait is over: commands that did
           complete are already flagged in their callbacks, everything still
           outstanding — this chunk and every chunk after it, which is therefore
           not submitted at all — keeps ok == 0 (the slot is zeroed by add()) and
           is reported as failed below. The queue is still torn down. */
        err = xnvme_queue_drain(queue);
        if (err < 0) {
            /* The shim is C and has no logger; this is the only diagnostic. */
            fprintf(stderr, "[kimix] xnvme_queue_drain() failed: %d\n", err);
            break;
        }
    }

    xnvme_queue_term(queue);

    for (chunk = 0; chunk < reader->count; ++chunk) {
        completed += reader->reqs[chunk].ok ? 1 : 0;
    }

    return completed;
}

#elif defined(_WIN32)

/*
 * Dispatch every recorded read as a positioned ReadFile() on the handle xnvme
 * opened, in order, one at a time.
 *
 * There is no event, no wait and no parallelism to manage here: xnvme opens the
 * handle WITHOUT FILE_FLAG_OVERLAPPED (see _win_handle()), so every ReadFile()
 * call completes in place before it returns and its OVERLAPPED is used for one
 * thing only — Offset/OffsetHigh, which selects the file position. The byte
 * count comes back from GetOverlappedResult() (FALSE = do not wait), the
 * documented accessor for an OVERLAPPED-driven call, and it is correct for a
 * synchronous handle: a short read at the tail reports the short count, a read
 * at/after EOF fails with ERROR_HANDLE_EOF — both verified against the Win32
 * implementation of the handle xnvme opens.
 */
static size_t _breader_commit_win(struct kimix_xnvme_breader *reader)
{
    HANDLE handle = _win_handle(reader->dev);
    size_t completed = 0;
    size_t i;

    if (!handle) {
        return 0;
    }

    for (i = 0; i < reader->count; ++i) {
        struct kimix_breader_req *req = &reader->reqs[i];
        OVERLAPPED ovl;
        DWORD transferred = 0;

        if (req->size > 0xFFFFFFFFull) {
            continue; /* ReadFile() takes a 32-bit length */
        }

        _win_overlapped(&ovl, (long long)req->offset);

        if (!ReadFile(handle, req->buf, (DWORD)req->size, &transferred, &ovl)) {
            /* Defensive: with a synchronous handle ReadFile() has already
               completed either way. If a future xnvme opens an overlapped handle
               this waits on the OVERLAPPED's own event (no per-request event is
               created: the OVERLAPPED carries one when hEvent is set, and
               bWait = TRUE simply blocks until the transfer is done). */
            if (GetLastError() != ERROR_IO_PENDING) {
                continue; /* immediate failure: counts as not completed */
            }
            if (!GetOverlappedResult(handle, &ovl, &transferred, TRUE)) {
                continue;
            }
        } else if (!GetOverlappedResult(handle, &ovl, &transferred, FALSE)) {
            continue; /* error surfaced through the OVERLAPPED */
        }

        req->ok = transferred == (DWORD)req->size ? 1 : 0;
    }

    for (i = 0; i < reader->count; ++i) {
        if (reader->reqs[i].ok) {
            completed += 1;
        }
    }

    return completed;
}

#endif /* __linux__ / _WIN32 */

size_t kimix_xnvme_breader_commit(struct kimix_xnvme_breader *reader)
{
    size_t completed;

    if (!reader || !reader->count) {
        return 0;
    }

#if defined(_WIN32)
    completed = _breader_commit_win(reader);
#elif defined(__linux__)
    completed = _breader_commit_queue(reader);
#else
    completed = 0; /* no wired xnvme backend on this platform */
#endif

    /* Reset the recorded list (the array itself is kept for reuse). */
    reader->count = 0;

    return completed;
}

void kimix_xnvme_breader_close(struct kimix_xnvme_breader *reader)
{
    if (!reader) {
        return;
    }

    if (reader->dev) {
        xnvme_file_close(reader->dev);
    }
    free(reader->reqs);
    free(reader);
}

/* ===========================================================================
 * Phase 5: write channel
 * ======================================================================== */

#ifdef _WIN32

/*
 * Create 'path' if it does not exist, leaving existing content alone.
 *
 * xnvme's Windows option mapping (xnvme_file_creation_opts_to_windows()) turns
 * opts.create into CREATE_ALWAYS, which TRUNCATES an existing file, and
 * opts.truncate into TRUNCATE_EXISTING. There is no OPEN_ALWAYS equivalent, so
 * the "create but keep what is there" case is done here: OPEN_ALWAYS creates a
 * missing file and is a no-op on an existing one, after which xnvme can open it
 * with its plain, non-truncating flags.
 */
static int _win_create_if_missing(const char *path)
{
    HANDLE handle = CreateFile((LPCSTR)path, GENERIC_WRITE | GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    CloseHandle(handle);

    return 1;
}

#endif /* _WIN32 */

void *kimix_xnvme_open_write(const char *path, int truncate)
{
#if defined(_WIN32) || defined(__linux__)
    struct xnvme_opts opts;

    if (!path || !path[0]) {
        return NULL;
    }

    opts = xnvme_opts_default();
    opts.rdonly = 0;
    opts.wronly = 0;
    opts.rdwr = 1;

#ifdef _WIN32
    /* NOTE: xnvme ORs the creation flags, so create + truncate together yield
       CREATE_ALWAYS | TRUNCATE_EXISTING = 7, an invalid dwCreationDisposition.
       Set exactly one of them. */
    if (truncate) {
        opts.create = 1; /* CREATE_ALWAYS: creates a missing file AND truncates */
        opts.truncate = 0;
    } else {
        opts.create = 0;
        opts.truncate = 0; /* OPEN_EXISTING: the file must be there... */
        if (!_win_create_if_missing(path)) {
            return NULL; /* ...so create it first, without truncating it */
        }
    }
#else
    opts.create = 1; /* O_CREAT: creates a missing file, keeps an existing one */
    opts.truncate = truncate ? 1 : 0;
    opts.create_mode = 0644;
#endif

    return xnvme_file_open(path, &opts);
#else
    /* No wired xnvme backend on this platform (see the split at the top). */
    (void)path;
    (void)truncate;
    return NULL;
#endif
}

int kimix_xnvme_pwrite(void *handle, const void *buffer, size_t count, long long offset)
{
    struct xnvme_dev *dev = handle;

    if (!count) {
        return 0; /* zero-length write: nothing to do, nothing to fail */
    }
    if (!dev || !buffer) {
        return -EINVAL;
    }

#if defined(_WIN32)
    return _win_pwrite(dev, buffer, count, offset);
#elif defined(__linux__)
    struct xnvme_cmd_ctx ctx;
    int err;

    if ((long long)(off_t)offset != offset) {
        return -EOVERFLOW;
    }

    /* Like xnvme_file_pread(), xnvme_file_pwrite() reports the transfer length
       in ctx.cpl.result rather than returning it. errno is cleared first for the
       same reason as in kimix_xnvme_stream_pread() — xnvme's synchronous
       backend derives the status code from errno when the count does not match —
       but here the comparison below stays the authority: a short write is a
       failure no matter how errno was left. */
    ctx = xnvme_file_get_cmd_ctx(dev);
    errno = 0;
    err = xnvme_file_pwrite(&ctx, (void *)buffer, count, (off_t)offset);
    if (err && !ctx.cpl.result) {
        return err;
    }
    if (ctx.cpl.result != count) {
        return -EIO;
    }

    return 0;
#else
    (void)offset;
    return -ENOSYS; /* no wired xnvme backend on this platform */
#endif
}

int kimix_xnvme_sync(void *handle)
{
    struct xnvme_dev *dev = handle;

    if (!dev) {
        return -EINVAL;
    }

#if defined(_WIN32) || defined(__linux__)
    /* XNVME_SPEC_FS_OPC_FLUSH: fsync() on POSIX, FlushFileBuffers() on Windows.
       Both are served by the same backend on every platform, so no workaround is
       needed here. */
    return xnvme_file_sync(dev);
#else
    return -ENOSYS; /* no wired xnvme backend on this platform */
#endif
}
