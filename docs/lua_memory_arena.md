# Lua memory arena (`KIMIX_LUA_ARENA`)

A per-`lua_State` size-class arena behind the interpreter's `lua_Alloc`, plus the
table-memory policy that goes with it: `table.new` presize, `table.shrink`, a
trim/purge opcode and an optional table-block compaction pass.

Scope of the change: `src/ext/lua` only (`larena.c/h` are new; every other edit
is `#if defined(KIMIX_LUA_ARENA)`-guarded and exported to
`src/ext/lua.kimix.patch`), plus one root option, one test target and three test
suites. **`luaL_newstate` is untouched**: an existing embedder (`src/script`,
`src/api`, `tests/unit/ext/test_lua.cpp`) behaves exactly as before.

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

`lua_ArenaClassCount/ClassOf/ClassInfoGet` expose the class table (slot size,
segment payload, live/free slots) for embedder diagnostics and tests.

## Design, in one paragraph

A block's size class is a **pure function of the requested size**
(`lua_arena_classof`), and the same function maps `osize` back on free, so no
per-block header exists (a header would cost 8–16 B on every `9*N+4` array
block). Tiers: 16-byte granules to 512 B, a ×1.25 ladder snapped to 16 B up to
`small_max` (64 KiB), **exact-size classes** for table array blocks (`9*N+4`,
N a power of two) and node blocks (`32*2^k [+16]`), and a **huge tier** above
`small_max` that hands out one exact mimalloc run plus a per-size LRU of freed
runs. Each class owns whole 64 KiB segments carved into slots at first use, with
one intrusive LIFO free list per segment: a segment is "empty" exactly when its
free count equals its slot count, which is what `trim` returns to mimalloc. No
locks — one arena per state. `cfg.dry_run = 1` runs the identical decision path
and counts everything (`would_*`) while allocating from mimalloc, which is what
makes the logic testable with zero layout risk.

## Measured results (the part that matters)

`bench_lua_mem` (run it per workload in a fresh process: `--only N`), commit
measured as a `mi_process_info` delta over the pre-state baseline, mirrored
against the arena's own `commit_bytes`:

| workload | mode | live | commit | arena says | commit/live | plan band |
|---|---|---|---|---|---|---|
| wide_array_tables | mimalloc | 6.31 MB | 12.10 MB | — | 1.92× | 1.43× |
| wide_array_tables | arena | 6.31 MB | 13.21 MB | 8.67 MB | 2.10× | ≤ 1.55× ✗ |
| hash_string_tables | mimalloc | 5.51 MB | 9.94 MB | — | 1.80× | 1.76× |
| hash_string_tables | arena | 5.51 MB | 12.78 MB | 7.70 MB | 2.32× | ≤ 1.35× ✗ |
| one_big_array | mimalloc | 17.18 MB | 20.91 MB | — | 1.22× | 2.79× |
| one_big_array | arena | 17.18 MB | 24.11 MB | 18.04 MB | 1.40× | ≤ 1.15× ✗ |
| resize_churn | mimalloc | 0.02 MB | 1.51 MB | — | — | +5.8 MB |
| resize_churn | arena | 0.02 MB | 6.64 MB | 1.04 MB | — | ≤ 1 MiB ✗ |

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

## Gates

| Gate | Command | Result |
|---|---|---|
| Arena unit tests (16 tests, 215 023 asserts) | `xmake run test_lua_arena` | green |
| Table policy (7 tests, 69 asserts) | `xmake run test_lua_table_mem` | green |
| Existing Lua suite untouched | `xmake run test_lua` | green (64 asserts) |
| Upstream suite | `bin/<mode>/lua-tests.exe <script>.lua` in `src/ext/lua/testes` | 16 of 18 scripts green, **identical with `--lua_arena=false`** |
| Patch discipline | `python scripts/apply_lua_patch.py --check -v` | 22 hunks, all guarded, applies to pinned `5f8f492` |
| Model vs engine | `python scripts/lua_mem_model.py --trace t.jsonl --check-engine` | 0 disagreements over 1424 calls |
| Benchmark with bands | `bin/<mode>/bench_lua_mem.exe` | prints the table above, exits 1 (bands missed, documented) |

`big.lua` and `attrib.lua` fail in *both* modes: `big.lua` is written to run
inside `all.lua`'s coroutine wrapper, and `attrib.lua` asserts on a Windows path
case. `all.lua` itself cannot run here because `main.lua` needs `io.popen`,
which this interpreter build does not have on Windows.

`KIMIX_LUA_MEMCHECK` (on in every non-NDEBUG build) makes the arena assert on a
double free or a foreign pointer, and the conservation invariants are asserted by
the tests: `slots_used == 0` after `lua_close`, `seg_count == 0`
after a converging trim, `huge_cached == 0` after `lua_ArenaTrim(1)`.

## Tooling

- `scripts/lua_mem_model.py` — replays a `KIMIX_LUA_MEMTRACE` JSONL trace
  through the class/segment/huge/trim decisions without a Lua engine; prints the
  class table, the predicted commit and the per-class detail, and reports any
  class disagreement with the engine (`--check-engine`).
- `scripts/apply_lua_patch.py` — `--check` verifies every hunk keeps its
  `KIMIX_LUA_` guard and that `src/ext/lua.kimix.patch` still applies to the
  pinned submodule commit; `--write` regenerates it (including the two new
  files, via `git add -N`).
- `KIMIX_LUA_MEMTRACE=<path>` (bounded by `KIMIX_LUA_MEMTRACE_MAX_MB`, default
  64, off by default) and `KIMIX_LUA_ARENA_DRYRUN=1` are the two environment
  hooks; both are inert in a stock build.

## Build switch

`option("lua_arena")`, default **on**. Off (`xmake f --lua_arena=false -c`) the
vendored sources compile unchanged — that is the compatibility gate, and
`test_lua` / the upstream scripts above were run in both configurations.
