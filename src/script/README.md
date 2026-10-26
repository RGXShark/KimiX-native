# `src/script/` — the Lua script sandbox (`kimix-script`)

`kimix::script::Sandbox` runs **one Lua chunk per call** in a throw-away,
budgeted `lua_State` whose environment holds only a vetted subset of the standard
library. A script can never read or write a file, spawn a process, load a module,
reach debug introspection, obtain an address, exit the process, read the
environment, or allocate/run without a bound — and when it tries, the run
**aborts loudly** with a message that names the API, the category and the script
line, and that message travels back to the caller as a typed `Result`.

```cpp
#include <script/script_sandbox.h>

kimix::script::Sandbox sb;                       // default Limits
auto r = sb.run("return 1 + 1", "script.lua");
if (r.ok()) { use(r.results[0].i); }             // 2
else        { report(r.status, r.message); }     // "script.lua:7: DENIED: io.open — …"
```

The same surface exists as a plain C ABI for foreign callers:
`<api/ffi_lua.h>` (`kimix_lua_*`, documented in `docs/ffi.md`).

---

## 1. The five layers

Defence in depth, not redundancy — each layer closes a hole the one above it
leaves open.

| Layer | Mechanism | What it stops |
|---|---|---|
| **L0** creation | `lua_newstate(budget_alloc, self, fixed_seed)`, `lua_atpanic`, `lua_setwarnf`, one extraspace header. Never `luaL_newstate`, never `luaL_openlibs`. | inherited libraries, a stderr panic, a stderr warning handler, address/time-based non-determinism |
| **L1** whitelist | `luaL_openselectedlibs(L, base\|string\|table\|math\|utf8, 0)` | `io`, `os`, `package`, `debug`, `require` are never opened — structurally absent |
| **L2** tripwires | tables/functions that exist, are callable, and raise `DENIED: <api> — <why>` | *silent* containment: `io` is a `table`, not `nil`, so the attempt is named instead of "attempt to index a nil value" |
| **L3** sticky + re-raise | a first-violation-wins flag in the extraspace header plus re-raising `pcall`/`xpcall`/`coroutine.resume`/`coroutine.close` | a script swallowing a denial with its own `pcall` (stock Lua catches a plain error 1000/1000 times) |
| **L4** budgets | one budgeted `lua_Alloc`, one `LUA_MASKCOUNT` hook, a cancel flag | unbounded memory, runaway CPU, a host that wants out |
| **L5** marshalling | bounded, cycle-safe conversion of return values; output caps | a hostile *return value* turning into a native OOM or hang |

## 2. Capability matrix

| Capability | Behaviour |
|---|---|
| file read/write/stat/delete/rename | impossible — `DENIED: io.open — file I/O is not available in the sandbox` |
| subprocess (`os.execute`, `io.popen`) | impossible — `DENIED`, `os.exit` can never kill the host |
| dynamic loading (`require`, `package.loadlib`, `dofile`, `loadfile`) | impossible — `DENIED … module loading is not available in the sandbox` |
| debug introspection (`debug.*`) | impossible — `DENIED … debug introspection is not available in the sandbox` |
| bytecode | `string.dump` is denied; `load` forces text mode and refuses a binary chunk with `LOADERR: attempt to load a binary chunk (mode is 't')` |
| pointers | no userdata is ever reachable; `tostring`/`print`/`string.format('%s')` render tables and functions as `table: #1` / `function: #2`; `%p` is denied |
| env vars / locale / clock | not exposed (`os.getenv`, `os.setlocale`, `os.time`, … are tripwires) |
| `collectgarbage` | only `"count"`, only when enabled at construction |
| stdout/stderr | captured into `Result::output`, never written to the process streams |
| global mutation across runs | impossible with `fresh_state_per_run` (the default): a fresh state and a fresh `_ENV` per run |
| heap/GC state outliving a run | impossible: the state is closed and every byte released (`alloc_used == 0`, reported on stderr if a host bug ever breaks it) |
| memory | one budget (`Limits::max_memory_bytes`), one single-allocation cap, sticky OOM (a retry loop cannot outlive the first denial) |
| CPU | wall-clock deadline + optional instruction budget through the count hook, and `Sandbox::cancel()` from another thread |

What is **not** in scope, stated so nobody oversells the result:

- Another module inside the same process can still `dlopen` Lua, call
  `luaL_openlibs` or link its own copy. This protects the host from *scripts*, not
  the process from its own modules. `scripts/check_lua_sandbox.py` is the guard
  rail against that happening by accident in this tree.
- No kernel-level isolation: a memory-safety bug in the Lua C core would be a
  process compromise.
- Uninterruptible C work (a pathological `string.find`/`gsub` pattern burns CPU
  with zero hook strikes) is *bounded* by the memory budget, not eliminated.
- Process separation (`fork` a helper, JSON over pipes) is the real answer for
  untrusted third-party scripts and is deliberately deferred.

## 3. Message format

One format, composed in exactly one place (`script_error.cpp`), so what a
script's own error handler sees and what the host receives cannot diverge:

```
<chunk>:<line>: <PREFIX>: <api> — <detail>
```

| Prefix | `Status` | Example |
|---|---|---|
| `DENIED: ` | `denied` | `script.lua:7: DENIED: io.open — file I/O is not available in the sandbox` |
| `LOADERR: ` | `denied` | `script.lua:5: LOADERR: attempt to load a binary chunk (mode is 't')` |
| `MEMORY: ` | `memory` | `script.lua:9: MEMORY: allocation of 67108864 bytes exceeds the sandbox budget (used 14361 of 16777216)` |
| `TIMEOUT: ` | `timeout` | `script.lua:2: TIMEOUT: script exceeded its 2.000 s deadline (4523000 instructions)` |
| `CANCELLED: ` | `cancelled` | `script.lua:2: CANCELLED: the host cancelled this run (512000 instructions executed)` |
| `LIMIT: ` | `limit` | `script.lua:N: LIMIT: output exceeds the 65536-byte cap` |
| `RUNTIME: ` | `runtime` | `script.lua:1: RUNTIME: attempt to index a nil value (global 'nosuch')` |
| `BEHAVIOUR: ` | `behaviour` | `script.lua:1: BEHAVIOUR: a sandbox primitive was replaced by the script` |
| `SYNTAX: ` | `syntax` | Lua's own message, kept verbatim |
| `INTERNAL: ` | `internal` | a host bug (a concurrent `run()`), or a Lua panic |

`<line>` comes from `luaL_where(L, 1)` at raise time. `LUA_IDSIZE` is 60, so a
long chunk name is truncated **inside the message**; the full name is always
carried in `Result::violation.chunk` (and `kimix_lua_last_violation`).

## 4. The per-run-state rule

A run creates its own `lua_State`, builds its own `_ENV`, runs, collects, and
closes. That costs ~13 % more than reusing a state (≈110 µs vs ≈100 µs in a debug
build, see `tests/unit/script/test_script_bench.cpp`) and buys three things
nothing else does:

1. no global written by one run is visible to the next (a fresh `_ENV` alone is
   not enough — `string.rep = …` would still hijack the shared library table),
2. every byte is released when the run ends (`alloc_used == 0`, asserted),
3. `_G` is the run environment, so there is no path to the real globals.

`Limits::fresh_state_per_run = false` keeps one state alive across runs: faster,
and documented as **not isolated**. `_ENV` is still rebuilt per run, but library
tables survive.

## 5. The longjmp rule (read before adding a native)

The project is built without C++ exceptions, and `lua_error()` unwinds by
`longjmp`. **No C++ object with a destructor may be alive when a `lua_CFunction`
can raise.** Concretely:

- every `lua_CFunction` in this module is POD-only and raises at the very end
  (`return sb_raise(...)` / `return lua_error(L)`);
- scratch buffers live in `SandboxState` (`body`, `load_buffer`, `json_buffer`,
  `json_path`, `json_key_stack`), never on the stack of a raising frame;
- a raise goes through `sb_raise()` → `sb_do_raise()`, which composes the message
  **before** it longjmps;
- every call into the interpreter happens under the ONE `lua_pcallk` in
  `script_run.cpp` (`sb_bootstrap`), so an error there always lands on the same
  frames and no destructor is skipped.

### Audit of the calling sites (re-run it after a change)

```bash
grep -n "lua_call\|lua_pcall\|lua_resume\|lua_closethread" src/script/*.cpp
grep -n "lua_State \*" src/script/*.h          # only the INTERNAL headers
```

What it must show, and why each site is inside the protection:

| Site | Why it is protected |
|---|---|
| `script_run.cpp` — `lua_pcallk` (the boundary) and the bootstrap's `lua_call` | the chunk runs *inside* the boundary: its errors land on the boundary's frames |
| `script_env.cpp` — `sbx_load` (reader function), `sbx_format` (stock `string.format`), `sbx_collectgarbage` (stock `collectgarbage`), `sbx_tostring` (`__tostring`) | each is a `lua_CFunction`; it is only reachable from script code, i.e. only from inside the boundary, and its `lua_call` raises into that same `lua_pcallk` |
| `script_env.cpp` — `lua_pcallk` in `sbx_pcall`/`sbx_xpcall` (with the `sbx_finishpcall` continuation), `lua_resume`/`lua_closethread` in `sbx_coresume`/`sbx_coclose` | these ARE the re-raising wrappers: they make their own protected call and then re-raise the sticky message |
| `script_env.cpp` — `lua_call` for `math.randomseed` during env construction | inside the bootstrap, i.e. inside the boundary |
| `script_state.cpp` — `lua_pcall` around the final `collectgarbage` at teardown | the one call that runs after the boundary has returned; it protects the finalizers a raising `__gc` would otherwise make fatal |

`lua_State *` appears only in the INTERNAL headers (`script_state.h`,
`script_env.h`, `script_error.h`, `script_value.h`). The public
`script_sandbox.h` forward-declares `struct lua_State` and exposes it only as a
`NativeFn` parameter, and the C ABI (`api/ffi_lua.h`) never mentions it at all, so
nothing outside this directory can reach the interpreter through these headers.

## 6. Adding a native (a host-provided function)

```cpp
#include "lua.h"   // a native speaks the Lua C API; the sandbox itself never leaks it

static int host_hash(lua_State *L) {              // POD-only!
    size_t len = 0;
    const char *text = lua_tolstring(L, 1, &len);
    lua_pushinteger(L, static_cast<lua_Integer>(kimix::hash64(text, len)));
    return 1;                                     // raises only at the very end
}

kimix::script::Sandbox sb;
sb.add_native("host_hash", &host_hash);            // installed into every run's _ENV
auto r = sb.run("return host_hash('abc')");
```

Rules for a native: it is installed per run, it sees the sandbox `lua_State`, it
must not keep a pointer to it, it must be POD-only across any raising call, and it
should deny nothing — the sandbox's own tripwires already cover the dangerous
capabilities. A native is how a host injects a *deterministic* capability (a
clock, randomness, a lookup) without opening a forbidden one.

## 7. Files

| File | Role |
|---|---|
| `script_sandbox.h` | the public C++ surface: `Sandbox`, `Limits`, `Result` |
| `script_status.h` | `Status`, the message prefixes, `Violation`, `Result` |
| `script_value.h` | `Value` — the only thing that crosses the boundary (no pointers) |
| `script_state.h/.cpp` | `SandboxState`, state creation/teardown, panic, warnings, the `Sandbox` object |
| `script_alloc.h/.cpp` | the budgeted `lua_Alloc` (memory cap + sticky OOM) |
| `script_env.h/.cpp` | `_ENV`: allow list, replacements, tripwires, redaction, integrity post-check |
| `script_error.h/.cpp` | the one message format and the raise helpers |
| `script_sticky.cpp` | first-violation-wins + re-raising the sticky message |
| `script_hook.cpp` | the count hook: deadline, instruction budget, cancel |
| `script_run.cpp` | THE single `lua_pcallk` boundary, the message handler, the run flow |
| `script_api.h` | `KIMIX_SCRIPT_API` (three states, `_STATIC` → nothing) |
| `script_lua.h` | the only include of the vendored Lua C API (wrapped in `extern "C"`) |

## 8. Tests

| Target | What it proves |
|---|---|
| `test_script_sandbox` | lifecycle, isolation, no stderr, no leak, marshalling + caps, memory/timeout/output budgets, cancel, concurrent-run rejection, natives |
| `test_script_hostile` | the abort contract: 17 forbidden APIs × 7 wrappers, bytecode refusal, pointer hygiene, a denial inside a `__gc`, primitive replacement ⇒ `behaviour` |
| `test_script_surface` | the golden capability allowlist (any new reachable global fails the suite until reviewed) and the tagged tripwires |
| `test_script_bench` | env-gated benchmark (`KIMIX_SCRIPT_BENCH=1`): fresh vs reused state, hook granularity, marshalling, the denial path |
| `test_lua` (untouched) | the raw vendored library still behaves (it uses `luaL_newstate`, which the sandbox avoids) |

```bash
xmake f -m debug -c -y && xmake build
xmake run test_script_sandbox
xmake run test_script_hostile
xmake run test_script_surface
python scripts/check_lua_sandbox.py          # the source lint
python bootstrap.py --test                   # smoke binary, not the suites
```

## 9. Residual risks and open decisions

| Risk | Status | Mitigation |
|---|---|---|
| Uninterruptible C work: a pathological `string.find`/`gsub` pattern burns CPU with 0 hook strikes | **open** | the memory budget bounds the subject size; a match-step budget in `lstrlib.c` (behind a `KIMIX_LUA_SANDBOX` define) or process isolation would close it |
| A host bug that calls the Lua API unprotected ⇒ panic ⇒ `abort()` | accepted | the panic names itself a host bug; `src/script` never leaks a `lua_State` |
| `lua_close` still runs finalizers that may call script code | accepted | a protected `collectgarbage('collect')` runs first, inside the run's protection, so a denying finalizer becomes a sticky violation |
| `LUA_IDSIZE = 60` truncates long chunk names inside a message | accepted | keep names ≤ 40 chars; the full name is in `violation.chunk` |
| A concurrent `run()` on one `Sandbox` | rejected | `Status::internal` (`busy` is an atomic exchange) |
| The vendored Lua sources are unmodified | deliberate | the sandbox is written from the host side; the optional `KIMIX_LUA_SANDBOX` source hardening of the plan (S-I8) is **not** applied, because `string.format('%p')` and `%s` of a non-scalar are already closed by wrapping `string.format` in `script_env.cpp` |
| Another module can still open `io` | by design | the lint plus "never return a `lua_State*`" |

Open decisions, with the default that is implemented:

1. **Coroutines** — off by default (`allow_coroutines(false)`); enabling installs
   re-raising `resume`/`close` wrappers.
2. **`collectgarbage`** — only `"count"`, only when enabled. Default off.
3. **Clock/env/randomness** — denied. A host that needs one injects a native.
4. **`load`** — text only (mode `"t"`), reader functions accumulated against the
   source cap, `_ENV` injected.
5. **Table results** — canonical JSON (minified, keys byte-sorted), depth ≤ 32,
   ≤ 256 KiB by default.
6. **Module name** — `src/script/`, `kimix::script`, `kimix-script`.
