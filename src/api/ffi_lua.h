/*
 * ffi_lua.h -- the Lua script sandbox (kimix-script) as a plain C ABI.
 *
 * WHAT THIS FAMILY IS
 * -------------------
 * A foreign caller (C, C#, Zig, Rust, Python ctypes/cffi, Julia, Node ffi-napi)
 * creates ONE opaque sandbox handle and runs Lua chunks through it.  Each run
 * compiles and executes one chunk in a throw-away, budgeted lua_State whose
 * environment holds only a vetted subset of the standard library: a script
 * cannot read or write a file, spawn a process, load a module, reach debug
 * introspection, obtain an address, exit the process, read the environment, or
 * allocate/run without a bound.  An attempt is a LOUD denial that ABORTS the run
 * and comes back here as `status == KIMIX_LUA_DENIED` plus a message of the
 * documented shape
 *
 *     <chunk>:<line>: DENIED: <api> -- <why>
 *
 * even when the script wrapped the call in its own pcall/xpcall/coroutine.resume.
 * The full model (five layers, capability matrix, residual risks) is
 * src/script/README.md; the C++ twin of this surface is
 * <script/script_sandbox.h>.
 *
 * TWO-PHASE, CALLER-OWNED BUFFERS
 * -------------------------------
 * There is NO library allocation on any read path.  Every variable-length result
 * (message, output, traceback, violation strings, a string value) uses the same
 * protocol as the rest of kimix_api: pass NULL/0 to learn the required length,
 * then pass caller storage.  A buffer that is too small answers
 * KIMIX_ERR_OUT_OF_RANGE and STILL reports the required length, so a caller can
 * resize and retry.  Text is copied with a NUL terminator (the length excludes
 * it); a payload is never NUL-free on this surface, because every string here is
 * either a message or a Lua string value.
 *
 * OWNERSHIP
 * ---------
 * The ONLY object this area allocates is the sandbox handle
 * (kimix_lua_sandbox_create); it is released by exactly one
 * kimix_lua_sandbox_destroy.  Everything the caller receives is copied into the
 * caller's own buffers, so no pointer from this library ever has to be handed
 * back -- the one-heap rule of api/ffi_common.h holds trivially here.
 * `kimix_lua_status_name` and `kimix_lua_version_string` return static storage
 * owned by the library; never free them.
 *
 * THREADING
 * ---------
 * A sandbox handle is NOT thread-safe: a Lua state is single-threaded and the
 * handle keeps one per run.  A concurrent kimix_lua_run answers
 * KIMIX_LUA_INTERNAL ("concurrent run on one Sandbox").  kimix_lua_cancel is the
 * ONLY call that may be made from another thread while a run is in flight; the
 * count hook then aborts the run with KIMIX_LUA_CANCELLED.  One handle per worker
 * thread is the intended deployment.
 *
 * STATUS CONTRACT
 * ---------------
 * - kimix_lua_sandbox_create / _destroy / _cancel / the accessors return the
 *   house `kimix_status` (KIMIX_OK == 0).
 * - kimix_lua_run returns `kimix_status` for the CALL (bad arguments, no
 *   handle) and reports the CHUNK's outcome in `result->status`, one of the
 *   KIMIX_LUA_* codes below -- KIMIX_LUA_OK == 0, so
 *   `if (res.status)` reads as "the run failed".
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h for
 * the rules shared by the whole surface.  The index is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_LUA_H
#define KIMIX_API_FFI_LUA_H

#include <api/ffi_common.h>

KIMIX_FFI_BEGIN

/* The opaque handle: one per worker thread, created by
 * kimix_lua_sandbox_create and released by kimix_lua_sandbox_destroy. */
typedef struct kimix_lua_sandbox kimix_lua_sandbox;

/* ===========================================================================
 * 1. Vocabulary
 * ======================================================================== */

/* The outcome of one run.  Numerically identical to kimix::script::Status, so a
 * binding can switch on the same integers as the C++ side. */
typedef enum kimix_lua_status_code {
    KIMIX_LUA_OK = 0,         /* the chunk ran; results are available */
    KIMIX_LUA_SYNTAX = 1,     /* the chunk did not compile (Lua's own message) */
    KIMIX_LUA_RUNTIME = 2,    /* an ordinary Lua error */
    KIMIX_LUA_DENIED = 3,     /* a tripwire: a forbidden capability was called */
    KIMIX_LUA_MEMORY = 4,     /* the memory budget was exhausted */
    KIMIX_LUA_TIMEOUT = 5,    /* the deadline / instruction budget was exhausted */
    KIMIX_LUA_BEHAVIOUR = 6,  /* the script replaced a sandbox primitive */
    KIMIX_LUA_CANCELLED = 7,  /* kimix_lua_cancel was called during the run */
    KIMIX_LUA_LIMIT = 8,      /* an output / result / source cap was exceeded */
    KIMIX_LUA_INTERNAL = 9    /* host misuse or a Lua panic (never a script error) */
} kimix_lua_status_code;

/* The value kinds a run can return.  A function, a thread and a userdata NEVER
 * cross this boundary (the run answers KIMIX_LUA_LIMIT instead), which is the
 * outbound half of the pointer-control rule. */
typedef enum kimix_lua_value_kind {
    KIMIX_LUA_VALUE_NIL = 0,
    KIMIX_LUA_VALUE_BOOLEAN = 1,
    KIMIX_LUA_VALUE_INTEGER = 2,
    KIMIX_LUA_VALUE_NUMBER = 3,
    KIMIX_LUA_VALUE_STRING = 4,
    KIMIX_LUA_VALUE_JSON = 5 /* a table, re-emitted as canonical JSON text */
} kimix_lua_value_kind;

/* The resource contract of a run.  `kimix_lua_default_limits()` returns the
 * library defaults (16 MiB state, 1 MiB source, 2.000 s deadline, 256 KiB
 * results, fresh state per run, no coroutines, no collectgarbage). */
typedef struct kimix_lua_limits {
    size_t max_memory_bytes;   /* whole-state budget, all threads */
    size_t max_source_bytes;   /* the chunk, and what `load` may accumulate */
    size_t max_string_bytes;   /* a single allocation cap */
    size_t max_output_bytes;   /* captured print()/warn() text */
    size_t max_result_bytes;   /* marshalled results (canonical JSON for tables) */
    uint32_t max_result_values;
    uint32_t max_result_depth; /* nesting cap for a returned table */
    uint32_t hook_granularity; /* count-hook period (instructions) */
    uint32_t random_seed;      /* math.randomseed, pinned for determinism */
    unsigned seed;             /* interpreter-state seed, pinned for determinism */
    double max_seconds;        /* wall-clock deadline, 0 = none */
    uint64_t max_instructions; /* instruction budget, 0 = unbounded */
    uint8_t fresh_state_per_run;        /* 1 (default): perfect isolation */
    uint8_t allow_coroutines;           /* 0 (default): no coroutine library */
    uint8_t allow_collectgarbage_count; /* 0 (default): collectgarbage denied */
    uint8_t _reserved;                  /* always 0 */
} kimix_lua_limits;

/* What one run produced.  The variable-length parts (message, output,
 * traceback, violation strings, values) are read through the accessors below --
 * this struct carries only lengths and counters. */
typedef struct kimix_lua_result {
    int32_t status;      /* one of KIMIX_LUA_* */
    int32_t line;        /* the script line of the failure, 0 when unknown */
    size_t message_len;  /* required bytes (excluding the NUL) */
    size_t output_len;   /* captured print()/warn() text */
    size_t traceback_len;
    size_t value_count;  /* number of return values marshalled */
    size_t memory_peak;  /* peak live bytes of the state during the run */
    uint64_t instructions;
    double seconds;
} kimix_lua_result;

/* ===========================================================================
 * 2. Library / ABI queries
 * ======================================================================== */

/* KIMIX_API_ABI_VERSION as compiled into this image; a binding asserts on it at
 * load time.  Never fails. */
KIMIX_FFI uint32_t kimix_lua_abi_version(void);

/* The kimix-core version string of this build.  Static storage; never free it. */
KIMIX_FFI const char *kimix_lua_version_string(void);

/* The textual name of a KIMIX_LUA_* code ("ok", "denied", ...); an unknown code
 * answers "unknown".  Static storage; never free it. */
KIMIX_FFI const char *kimix_lua_status_name(KIMIX_IN int32_t status);

/* The library defaults, as a value a caller can start from and edit. */
KIMIX_FFI kimix_lua_limits kimix_lua_default_limits(void);

/* ===========================================================================
 * 3. Sandbox lifecycle
 * ======================================================================== */

/* Create one sandbox -- the ONLY allocation of this family.  A NULL `limits`
 * uses the defaults.  `*out` is written only on KIMIX_OK.  Release with exactly
 * one kimix_lua_sandbox_destroy.  KIMIX_ERR_INVALID_ARG for a NULL `out`. */
KIMIX_FFI kimix_status kimix_lua_sandbox_create(KIMIX_IN KIMIX_NULLABLE const kimix_lua_limits *limits,
                                                KIMIX_OUT kimix_lua_sandbox **out);

/* Destroy the handle (a run in flight must have finished; cancel it first and
 * join your own thread).  A NULL handle is ignored. */
KIMIX_FFI kimix_status kimix_lua_sandbox_destroy(KIMIX_TRANSFER kimix_lua_sandbox *sb);

/* Ask the in-flight run to abort -- the only cross-thread call.  The count hook
 * raises KIMIX_LUA_CANCELLED; the flag is cleared at the start of the next run.
 * KIMIX_ERR_INVALID_ARG for a NULL handle. */
KIMIX_FFI kimix_status kimix_lua_cancel(KIMIX_IN kimix_lua_sandbox *sb);

/* ===========================================================================
 * 4. Running a chunk
 * ======================================================================== */

/* Compile and run `src` (length-based: an embedded NUL is just a byte) in the
 * sandbox environment, with the chunk name "script.lua".  The source is copied
 * before use.
 *
 *   sb       the handle (KIMIX_ERR_INVALID_ARG when NULL)
 *   src      the chunk source; NULL is treated as an empty chunk
 *   src_len  its length in bytes
 *   res      REQUIRED out-parameter: the run's outcome (written on KIMIX_OK and
 *            on KIMIX_ERR_OUT_OF_RANGE, so a two-phase caller always learns the
 *            required message length)
 *   msg_buf  caller storage for the message, or NULL to only learn its length
 *   msg_cap  its capacity in bytes (NUL terminator included)
 *
 * Returns KIMIX_OK when the call itself succeeded (INSPECT res->status for the
 * chunk), KIMIX_ERR_OUT_OF_RANGE when msg_buf is too small (res->message_len is
 * still filled), KIMIX_ERR_INVALID_ARG for a NULL sb/res.
 *
 * This is the documented surface of the plan; use kimix_lua_run_named when the
 * message should name a real file. */
KIMIX_FFI kimix_status kimix_lua_run(KIMIX_IN kimix_lua_sandbox *sb,
                                     KIMIX_IN KIMIX_NULLABLE const char *src,
                                     KIMIX_IN size_t src_len,
                                     KIMIX_OUT kimix_lua_result *res,
                                     KIMIX_OUT KIMIX_NULLABLE char *msg_buf,
                                     KIMIX_IN size_t msg_cap);

/* Same as kimix_lua_run, with an explicit chunk name (it appears in every
 * message; keep it short -- LUA_IDSIZE is 60).  A NULL/empty name means
 * "script.lua". */
KIMIX_FFI kimix_status kimix_lua_run_named(KIMIX_IN kimix_lua_sandbox *sb,
                                           KIMIX_IN KIMIX_NULLABLE const char *src,
                                           KIMIX_IN size_t src_len,
                                           KIMIX_IN KIMIX_NULLABLE const char *chunk_name,
                                           KIMIX_IN size_t chunk_name_len,
                                           KIMIX_OUT kimix_lua_result *res,
                                           KIMIX_OUT KIMIX_NULLABLE char *msg_buf,
                                           KIMIX_IN size_t msg_cap);

/* ===========================================================================
 * 5. Reading the last run (two-phase, caller-owned buffers)
 * ======================================================================== */

/* The captured print()/warn() text of the last run.  KIMIX_ERR_OUT_OF_RANGE when
 * `buf` is too small (with `*out_len` still filled); KIMIX_OK with `buf == NULL`
 * reports the length only. */
KIMIX_FFI kimix_status kimix_lua_last_output(KIMIX_IN kimix_lua_sandbox *sb,
                                             KIMIX_OUT KIMIX_NULLABLE char *buf,
                                             KIMIX_IN size_t cap,
                                             KIMIX_OUT size_t *out_len);

/* The bounded traceback of the last failure (empty after a clean run). */
KIMIX_FFI kimix_status kimix_lua_last_traceback(KIMIX_IN kimix_lua_sandbox *sb,
                                                KIMIX_OUT KIMIX_NULLABLE char *buf,
                                                KIMIX_IN size_t cap,
                                                KIMIX_OUT size_t *out_len);

/* The violation record of the last DENIED run: the exact api ("io.open"), its
 * category ("file-io"), the human detail and the full chunk name.  Any of the
 * buffers may be NULL to skip that field; every `*_len` is always written. */
KIMIX_FFI kimix_status kimix_lua_last_violation(KIMIX_IN kimix_lua_sandbox *sb,
                                                KIMIX_OUT KIMIX_NULLABLE char *api,
                                                KIMIX_IN size_t api_cap,
                                                KIMIX_OUT KIMIX_NULLABLE size_t *api_len,
                                                KIMIX_OUT KIMIX_NULLABLE char *category,
                                                KIMIX_IN size_t category_cap,
                                                KIMIX_OUT KIMIX_NULLABLE size_t *category_len,
                                                KIMIX_OUT KIMIX_NULLABLE char *detail,
                                                KIMIX_IN size_t detail_cap,
                                                KIMIX_OUT KIMIX_NULLABLE size_t *detail_len,
                                                KIMIX_OUT KIMIX_NULLABLE char *chunk,
                                                KIMIX_IN size_t chunk_cap,
                                                KIMIX_OUT KIMIX_NULLABLE size_t *chunk_len);

/* One marshalled return value of the last run.  `kind` receives a
 * KIMIX_LUA_VALUE_*; integers/numbers land in `i`/`d`, booleans in `b`, and a
 * string or a JSON table lands in `str_buf` (two-phase, like every other buffer:
 * NULL reports the length in `*str_len`).  KIMIX_ERR_OUT_OF_RANGE for an index
 * >= value_count or a too-small buffer; a NULL `kind` is allowed. */
KIMIX_FFI kimix_status kimix_lua_last_value(KIMIX_IN kimix_lua_sandbox *sb,
                                            KIMIX_IN size_t index,
                                            KIMIX_OUT KIMIX_NULLABLE int32_t *kind,
                                            KIMIX_OUT KIMIX_NULLABLE uint8_t *b,
                                            KIMIX_OUT KIMIX_NULLABLE int64_t *i,
                                            KIMIX_OUT KIMIX_NULLABLE double *d,
                                            KIMIX_OUT KIMIX_NULLABLE char *str_buf,
                                            KIMIX_IN size_t str_cap,
                                            KIMIX_OUT KIMIX_NULLABLE size_t *str_len);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_LUA_H */
