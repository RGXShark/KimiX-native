/*
 * script_sandbox.h -- the public C++ surface of the Lua script sandbox
 * (kimix-script).
 *
 * WHAT THIS IS
 * ------------
 * `kimix::script::Sandbox` runs one Lua chunk per call in a throw-away lua_State
 * whose environment contains ONLY a vetted subset of the standard library:
 * base (minus the file/process/bytecode entry points), string, table, math and
 * utf8.  A script that reaches for anything else (file I/O, subprocesses, module
 * loading, bytecode, debug introspection, pointers, unbounded memory, unbounded
 * CPU) gets a LOUD denial that ABORTS the run and travels back to the caller as
 * a typed Result -- it cannot be swallowed by the script's own pcall/xpcall/
 * coroutine.resume.  See src/script/README.md for the five defence layers and
 * the message format.
 *
 * The six properties every caller can rely on:
 *   1. run() never throws (the project is built without exceptions) and never
 *      returns a bare failure: every non-ok Result carries a message.
 *   2. Nothing reaches stdout/stderr: print()/warn() are captured into
 *      Result::output, Lua warnings (__gc errors) are captured too.
 *   3. A run cannot exceed its memory budget, its deadline, or its
 *      output/source/result caps (Limits).
 *   4. State is per run (fresh_state_per_run, the default): no global written by
 *      one run is visible to the next, and every byte is released when the run
 *      ends (asserted in debug builds).
 *   5. No address ever crosses the boundary: results are nil/boolean/number/
 *      string/canonical-JSON, and tostring()/string.format('%s') render tables
 *      and functions as "table: #1" / "function: #2".
 *   6. Determinism: the state is created with a fixed seed and math.randomseed
 *      is pinned, so `pairs` order and math.random are reproducible.
 *
 * THREADING
 * ---------
 * A Sandbox is NOT thread-safe: a lua_State is single-threaded and this class
 * keeps one per run.  A concurrent run() on the same Sandbox is answered with
 * Status::internal ("concurrent run on one Sandbox").  `cancel()` is the ONLY
 * call that may be made from another thread while a run is in flight; `busy()`
 * is the read-only probe.
 *
 * USAGE
 * -----
 *   kimix::script::Sandbox sb;                     // default Limits
 *   auto r = sb.run("return 1 + 1", "script.lua");
 *   if (r.ok()) { use(r.results[0].i); }           // 2
 *   else        { report(r.status, r.message); }   // "script.lua:1: DENIED: ..."
 *
 * See also: docs/ffi.md (the same surface as a plain C ABI, kimix_lua_*),
 * src/script/README.md (design + residual risks).
 */
#pragma once
#ifndef KIMIX_SCRIPT_SANDBOX_H
#define KIMIX_SCRIPT_SANDBOX_H

#include <cstdint>

#include <core/stl/memory.h> /* kimix::size_literals (16_M, ...) */
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <script/script_api.h>
#include <script/script_status.h>
#include <script/script_value.h>

/* Forward-declared on purpose: a consumer of this header never sees a vendored
 * Lua header, so nothing outside src/script can call the Lua C API through it. */
struct lua_State;

namespace kimix::script {

/* The resource contract of one run.  Everything a script can consume is
 * bounded here; the defaults are the ones the plan's tests assert. */
struct Limits {
    /* Whole-state budget, all threads, counted by the budgeted lua_Alloc
     * (script_alloc.cpp).  `used` includes the bootstrap state (~14 KB). */
    size_t max_memory_bytes = 16_M;
    /* The chunk source itself (`run`'s argument, and what `load` accumulates). */
    size_t max_source_bytes = 1_M;
    /* A SINGLE allocation cap: a script asking for one huge string is refused
     * early, with a clearer message than a slowly exhausted budget. */
    size_t max_string_bytes = 8_M;
    /* Captured print()/warn() text. */
    size_t max_output_bytes = 64_k;
    /* Marshalled return values (canonical JSON for tables). */
    size_t max_result_bytes = 256_k;
    uint32_t max_result_values = 64;
    /* Nesting cap for a returned table. */
    uint32_t max_result_depth = 32;
    /* Wall-clock deadline (0 = none). */
    double max_seconds = 2.0;
    /* Instruction budget (0 = unbounded; the wall clock still applies). */
    uint64_t max_instructions = 0;
    /* Count-hook granularity: the deadline is checked every N VM instructions. */
    uint32_t hook_instruction_granularity = 10'000;
    /* lua_newstate(alloc, ud, seed) seed and math.randomseed: fixed, so hash
     * order and math.random are reproducible run to run. */
    unsigned seed = 0x00C0FFEEu;
    uint32_t random_seed = 0x5EED2025u;
    /* true (default) = a new lua_State per run, i.e. perfect run-to-run
     * isolation.  false reuses one state: faster (~13 %), but globals and
     * library tables survive between runs -- documented as NOT isolated. */
    bool fresh_state_per_run = true;
};

class KIMIX_SCRIPT_API Sandbox {
public:
    explicit Sandbox(const Limits &limits = Limits{});
    ~Sandbox();

    /* Move-only (the state header holds pointers into this object). */
    Sandbox(Sandbox &&other) noexcept;
    Sandbox &operator=(Sandbox &&other) noexcept;
    Sandbox(const Sandbox &) = delete;
    Sandbox &operator=(const Sandbox &) = delete;

    /* Compile and run `source` with the sandbox environment.  The source is
     * copied before use.  `chunk_name` shows up in messages; keep it short
     * (LUA_IDSIZE is 60) -- the full name is carried in Result::violation.chunk.
     * Never throws; a concurrent call returns Status::internal. */
    Result run(kimix::string_view source, kimix::string_view chunk_name = "script.lua");

    /* The signature of a host-provided function (`lua_CFunction`).  The function
     * receives the sandbox lua_State and may only use the C API; it must be
     * POD-only and raise at the very end of its body (README §"Adding a native"). */
    using NativeFn = int (*)(lua_State *L);

    /* Install `name` into the run environment as a C function.  `upvalues` is
     * the number of nil upvalues to create for it (0 = a plain function).
     * Returns false when `name` is empty/not a valid identifier or `fn` is null. */
    bool add_native(kimix::string_view name, NativeFn fn, int upvalues = 0);

    /* coroutine.create/resume/status/wrap/yield/close: default OFF.  When on,
     * resume/close are replaced with re-raising wrappers, so a denial inside a
     * coroutine still aborts the run. */
    bool allow_coroutines(bool enable) noexcept;

    /* collectgarbage("count") only: default OFF (every other option is denied,
     * so a script cannot stop or pause the collector). */
    bool allow_collectgarbage_count(bool enable) noexcept;

    /* Ask the in-flight run to abort (thread-safe; the count hook raises
     * Status::cancelled).  Cleared automatically at the start of the next run. */
    void cancel() noexcept;

    /* true while a run() is in flight on this Sandbox. */
    bool busy() const noexcept;

    const Limits &limits() const noexcept;

private:
    struct Impl;
    Impl *_impl = nullptr;
};

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_SANDBOX_H */
