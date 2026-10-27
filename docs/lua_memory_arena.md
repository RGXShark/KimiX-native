# Lua memory arena (`KIMIX_LUA_ARENA`)

A per-`lua_State` size-class arena behind the interpreter's `lua_Alloc`, plus the
table-memory policy that goes with it: `table.new` presize, `table.shrink`, a
trim/purge opcode and an optional table-block compaction pass.

Scope of the change: `src/ext/lua` only (`larena.c/h` are new; every other edit
is `#if defined(KIMIX_LUA_ARENA)`-guarded and exported to
`src/ext/lua.kimix.patch`), plus one root option, two test targets, the two unit
suites, the footprint benchmark and the upstream-corpus driver
(`tests/unit/ext/lua_e2e/run_testes.py`). **`luaL_newstate` is untouched**: an
existing embedder (`src/script`, `src/api`, `tests/unit/ext/test_lua.cpp`)
behaves exactly as before.

Everything below is measured on this tree's default host (Windows, MSVC 14.44,
`-m debug`); numbers are printed by `bench_lua_mem` and are regenerated, not
transcribed.

## What is reachable from plain C

```c
#include "lua.h"        /* lua_ArenaConfig/Stats, LUA_GCTRIM, LUA_GCCOMPACT */
#include "lauxlib.h"    /* luaL_newstate_arena, luaL_arena, luaL_freestate_arena */

lua_State *L = luaL_newstate_arena(NULL);   /* NULL = the defaults */
...
lua_ArenaStats st;
lua_ArenaStatsGet(L, &st);                  /* logical_live, commit_bytes, ... */
lua_gc(L, LUA_GCTRIM, 0);                   /* level 0: return empty segments */
lua_gc(L, LUA_GCTRIM, 1);                   /* level 1: + drop the huge cache,
                                               mi_collect(true) */
lua_gc(L, LUA_GCCOMPACT, 0);                /* pack table blocks (needs
                                               cfg.enable_compaction = 1) */
lua_settop(L, lua_shrinktable(L, 1));       /* table.shrink(t) from C */
```

Script side (both `KIMIX_LUA_ARENA`-guarded):

- `table.new(narr [, nrec])` — a presized table, so building a big one costs a
  single array allocation instead of the 1 → 2 → 4 → … growth chain.
- `table.shrink(t)` — gives back unused array capacity; returns the bytes
  released. `#t` and the array-part order are unchanged, and elements that fall
  outside the new array part are re-inserted into the hash part (no key is lost).

lua_ArenaClassCount/ClassOf/ClassInfoGet expose the class table (slot size,
segment payload, live/free slots) for embedder diagnostics and tests, and
lua_ArenaInteriorHolders() reports the debug net the compaction pass asserts on
(always 0 in a release build, where the net is compiled out).

Design, in one paragraph

A block's size class is a pure function of the requested size
(lua_arena_classof), and the same function maps osize back on free, so no
per-block header exists (a header would cost 8–16 B on every 9*N+4 array
block). Tiers: 16-byte granules to 512 B, a ×1.25 ladder snapped to 16 B up to
small_max (64 KiB), exact-size classes for table array blocks (9*N+4,
N a power of two) and node blocks (32*2^k [+16]), and a huge tier above
small_max that hands out one exact mimalloc run plus a per-size LRU of freed
runs. Each class owns whole 64 KiB segments carved into slots at first use, with
one intrusive LIFO free list per segment: a segment is "empty" exactly when its
free count equals its slot count, which is what trim returns to mimalloc. No
locks — one arena per state. cfg.dry_run = 1 runs the identical decision path
and counts everything (would_*) while allocating from mimalloc, which is what
makes the logic testable with zero layout risk; it also answers
lua_gc(LUA_GCCOMPACT) with the plan it would follow (slots_free /
nslots_per_seg whole segments, one segment always kept as the target) instead of
walking segments it never carved.

The compaction pass itself (off by default, cfg.enable_compaction) moves table
array blocks out of sparse segments and is guarded by an interior-pointer net:
luaH_next and luaH_resize bump a per-state holder count around their body, the
count is cleared by luaD_throw when an error unwinds out of one, and
luaC_compact asserts it is zero before it moves anything.

## Measured results (the part that matters)

`bench_lua_mem` (run it per workload in a fresh process: `--only N`), commit
measured as a `mi_process_info` delta over the pre-state baseline, mirrored
against the arena's own `commit_bytes`:

| workload | mode | live | commit | arena says | commit/live | plan band |
|---|---|---|---|---|---|---|
| wide_array_tables | mimalloc | 6.31 MB | 12.8–13.5 MB | — | 2.03–2.14× | 1.43× |
| wide_array_tables | arena | 6.31 MB | 13.21 MB | 8.67 MB | 2.10× | ≤ 1.55× ✗ |
| hash_string_tables | mimalloc | 5.51 MB | 7.77 MB | — | 1.41× | 1.76× |
| hash_string_tables | arena | 5.51 MB | 12.78 MB | 7.70 MB | 2.32× | ≤ 1.35× ✗ |
| one_big_array | mimalloc | 17.18 MB | 18.72 MB | — | 1.09× | 2.79× |
| one_big_array | arena | 17.18 MB | 24.11 MB | 18.04 MB | 1.40× | ≤ 1.15× ✗ |
| resize_churn | mimalloc | 0.02 MB | 0.69 MB | — | — | +5.8 MB |
| resize_churn | arena | 0.02 MB | 6.64 MB | 1.04 MB | — | ≤ 1 MiB ✗ |

The three arena rows are bit-reproducible (13.21 / 12.78 / 24.11 MB); the
mimalloc rows move a few hundred KB between runs because the commit of a plain
state depends on what mimalloc has cached from earlier workloads in the same
process.

**The plan's process-commit bands are not met on this host, and the bench says
so** (it exits 1 in strict mode). Two separate reasons, both measurable:

1. *Inside the arena* the footprint is 1.37×–1.40× of live on the mixed
   workloads, not the 1.27× the model predicted. The gap is the **partial
   segment tail**: every class in use keeps up to one 64 KiB segment that is
   only partly filled, and these workloads touch 20–40 classes (≈ 1–2 MB). The
   model in `.kimix_cache/lua_memory_opt_plan.md` §3.3 counted slot rounding but
   not that tail.
2. *Outside the arena* the OS commit does not follow the arena's accounting
   (skew 0.60–0.75): freed 64 KiB segments are handed back with `mi_free`, but
   mimalloc only returns whole pages it owns, so the process keeps commit that
   the arena has already given up. The huge-table case shows this cleanly: the
   arena's own commit is 18.04 MB for 17.18 MB live (1.05×), while the process
   reports 24.11 MB. `mi_collect(true)` is already called by `lua_ArenaTrim(1)`;
   the purge delay is a *global* mimalloc option and is deliberately not touched
   (the heap is shared with `kimix-core`).

The one place the arena's own numbers improve dramatically is churn: after a
full GC plus `lua_ArenaTrim(1)` it commits **1.04 MB** for a workload that
allocated 200 × 18 KB tables, versus the plan's 5.8 MB "retained" baseline. The
process-level number is again dominated by reason 2.

So this lands as: the mechanism works, is accounted for, and is safe (see the
gates below), but it is **not a footprint win over plain mimalloc on this
platform**. Follow-ups that would attack the two measured causes:

- geometric segment sizing (start a class at 4 KiB and double up to
  `segment_bytes`), which removes most of the partial-tail waste;
- `lua_ArenaTrim(1)` after *every* run in a host that cares (the sandbox plan),
  so the churn win is not hidden by mimalloc's purge delay.

The plan's bands were derived on a host where plain mimalloc was already 1.43×
on `wide_array_tables` and 2.79× on `one_big_array`. On this host the *same
mimalloc-only runs* measure 1.92× / 1.42× / 1.09×, i.e. the arena has to beat a
baseline that is itself 0.5× worse than the plan's, which is why the `1.55×`
band is unreachable here even in principle (a plain state is already above it).

Gates

Run in the default configuration (`xmake f -m debug --lua_arena=true`), with the
three host-specific suites listed below.

| Gate | Command | Result |
|---|---|---|
| Arena unit tests (17 tests, 215 031 asserts) | `xmake run test_lua_arena` | green |
| Arena dry run (17 tests, 215 016 asserts; the two slot-mechanics tests skip, they have no carved slot to exercise) | `KIMIX_LUA_ARENA_DRYRUN=1 xmake run test_lua_arena` | green |
| Table policy (7 tests, 70 asserts) | `xmake run test_lua_table_mem` | green |
| Table policy dry run (7 tests, 45 asserts) | `KIMIX_LUA_ARENA_DRYRUN=1 xmake run test_lua_table_mem` | green |
| Existing Lua suite untouched | `xmake run test_lua` | green (64 asserts) |
| Sandbox unchanged with the arena built in (M-V7.3) | `xmake run test_script_sandbox` / `_surface` / `_hostile` | green (96 / 41 / 1234 asserts) |
| Upstream corpus, 31 scripts (`run_testes.py --record`) | `python tests/unit/ext/lua_e2e/run_testes.py` | 28 green, 3 host-specific failures, `memerr.lua` now converges |
| Upstream corpus through the engine dry run | `... --env KIMIX_LUA_ARENA_DRYRUN=1 --compare <record>` | identical (normalized output) |
| Upstream corpus with the arena compiled out | `xmake f --lua_arena=false` + `... --compare <record>` | identical (normalized output) |
| Patch discipline | `python scripts/apply_lua_patch.py --check -v` | 27 hunks, all guarded, applies to pinned `5f8f492` |
| Model vs engine | `python scripts/lua_mem_model.py --trace t.jsonl --check-engine --report` | 0 class disagreements over 76 716 calls, compaction plan printed |
| Benchmark with bands | `bin/<mode>/bench_lua_mem.exe` | footprint table above (4 bands missed, documented) |
| Hot path (M-V6) | same binary, `hot path:` line | +4.2 % / −13.4 % / +0.9 % / +0.2 % (bar +5 %) |
| Sparse-slab compaction (M3) | same binary, `sparse_slabs` section | 11.4 % of the arena's commit returned (bar 10 %) |

`attrib.lua`, `big.lua` and `files.lua` fail identically in both modes and in the
dry run: `big.lua` is written to run inside `all.lua`'s coroutine wrapper,
`attrib.lua` asserts on Windows path casing, `files.lua` on Windows `io.seek`
semantics. `all.lua` itself cannot run here because `main.lua` needs `io.popen`,
which this interpreter build does not have on Windows — that is why the corpus is
driven per script by `run_testes.py`. The comparison masks digits (random seeds,
timings, byte counts measured through the debug allocator), because the upstream
suites print run-dependent values on purpose: two runs of the *same* binary do
not produce byte-identical output.

`KIMIX_LUA_MEMCHECK` (on in every non-NDEBUG build) makes the arena assert on a
double free or a foreign pointer, and the conservation invariants are asserted by
the tests: `slots_used == 0` after `lua_close`, `seg_count == 0`
after a converging trim, `huge_cached == 0` after `lua_ArenaTrim(1)`.

### Two defects the verification pass found and fixed

1. **A leak on the out-of-memory path of a table resize** (present in the
   arena patch, invisible to every gate except the upstream corpus).
   `resizearray` was converted to `luaM_newblockx`, and `luaM_malloc_` *raises*
   on failure where upstream's `luaM_reallocvector` returns `NULL`. But
   `luaH_resize` builds the new hash part first and frees it only when the array
   allocation reports failure, so every failed resize leaked one hash block.
   `memerr.lua` — which raises the memory limit 7 bytes at a time until the task
   succeeds — then never converged: it spun for >10^6 iterations and grew ~7 B
   per iteration. The guarded path now uses `luaM_reallocx_(L, NULL, 0, n, cls)`,
   i.e. upstream's non-raising contract with the class hint. `memerr.lua`
   completes in ~1.5 s again.
2. **Uninitialized `global_State.arena`.** `lua_newstate` never initialised the
   field, so a plain state read back garbage from `G(L)->arena` (the header
   documents it as "NULL when the state was built with a plain allocator"). It
   only surfaced once something read it on a plain state — `luaL_newstate()`
   crashed with an access violation. `lua_newstate` now sets it to `NULL`.
3. **The sandbox surface depended on the build switch.**
   `test_script_surface`'s golden list failed with the arena built in, because
   `table.new`/`table.shrink` reach the interpreter's `table` library and the
   sandbox copied that table wholesale. The sandbox now drops both for every run
   (`src/script/script_env.cpp`), which is what the plan asked for ("the sandbox
   decides whether to expose it"): the reachable surface, the golden list and the
   hostile tests read the same with the arena on or off.

Tooling

- `scripts/lua_mem_model.py` — replays a `KIMIX_LUA_MEMTRACE` JSONL trace
  through the class/segment/huge/trim/compaction decisions without a Lua engine;
  prints the class table, the predicted commit, the compaction plan and the
  per-class detail, and reports any class disagreement with the engine
  (`--check-engine`).
- `scripts/apply_lua_patch.py` — `--check` verifies every hunk keeps its
  `KIMIX_LUA_` guard and that `src/ext/lua.kimix.patch` still applies to the
  pinned submodule commit; `--write` regenerates it (including the two new
  files, via `git add -N`).
- `tests/unit/ext/lua_e2e/run_testes.py` — the corpus driver behind the M-V3
  gate: one script per process, per-script timeouts, normalized-output record and
  compare, `--env K=V` for the dry-run replay (`--env KIMIX_LUA_ARENA_DRYRUN=1`).
- `KIMIX_LUA_MEMTRACE=<path>` (bounded by `KIMIX_LUA_MEMTRACE_MAX_MB`, default
  64, off by default; opened in append mode so one process may trace several
  arena states) and `KIMIX_LUA_ARENA_DRYRUN=1` are the two environment hooks;
  both are inert in a stock build.

## Known gaps (stated so nobody oversells the result)

- The plan's process-commit bands are **not met** on this host (see above); the
  arena's own accounting is 1.05–1.40× of live, the process number is dominated
  by mimalloc's retention, and plain mimalloc is already above the band.
- `lstring.c` carries no `LUA_MEMCLS_STR` hints, and no test walks the sources to
  pair the alloc/free hints: the funnel is documentation only (`luaM_*x_`
  ignore the class), so a missing or mismatched hint cannot change where a block
  lands or corrupt anything. The one place where the hint-carrying entry point
  mattered was `resizearray`, and there it was a bug, not a hint (see the leak
  above) — that is why the array allocation now uses `luaM_reallocx_`.
- The interior-pointer net covers `luaH_next` and `luaH_resize` — the two
  functions that both form a `Value *`/`Node *` into a vector and can be entered
  from outside a Lua/C call. `luaH_get*`/`luaH_pset*` carry no `lua_State *` in
  upstream's API, and a compaction only runs at a quiescent point
  (`nCcalls == 0`), which is asserted.
- The compaction dry run reports an **upper bound** (`slots_free /
  nslots_per_seg` whole segments, one segment kept as the target), because a dry
  run carves no segment to inspect; the plan/move equality is asserted in real
  mode by `test_lua_table_mem`.
- The Linux/ASan half of M-V5 and the `--lua_arena=false` *whole-suite* run were
  not executed in this pass (Windows host): the arena/library targets were built
  and tested in both modes, not every unrelated test target.
- Wiring the arena underneath the sandbox budget allocator
  (`src/script/script_alloc.cpp`) is the plan's `[needs src/script]` follow-up;
  `src/script/README.md` says so explicitly.

## Build switch

`option("lua_arena")`, default **on**. Off (`xmake f --lua_arena=false -c`) the
vendored sources compile unchanged — that is the compatibility gate. In this
pass `test_lua`, `test_lua_arena`, `test_lua_table_mem`, `bench_lua_mem` and
`lua-tests` were built and run in both configurations, and the upstream corpus
recorded in one mode and compared in the other (`run_testes.py --compare`).
