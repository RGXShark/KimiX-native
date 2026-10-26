// bench_lua_mem -- footprint benchmark for the kimix Lua memory arena.
//
// NOT a Boost.UT suite (its own main(), registered as a plain binary in
// tests/xmake.lua): a timing/footprint benchmark must not turn into a unit test
// that fails because the machine was busy. It prints one row per workload with
// the measured value next to its band and exits non-zero when a band is missed.
//
// The four workloads are the ones the plan measured (Appendix A of
// .kimix_cache/lua_memory_opt_plan.md):
//   wide_array_tables  300 array tables, n in 1..5000 (LCG)   band <= 1.55x
//   hash_string_tables 300 hash tables, 200 short + 40 long   band <= 1.35x
//   one_big_array      one array of 2 000 000 integers        band <= 1.15x
//   resize_churn       200 x (build 2000-int table, drop it)  retained <= 1 MiB
//
// "commit" is mimalloc's process commit delta over the moment before the state
// was created (mi_process_info), so the interpreter's own baseline is excluded
// from the ratio, exactly like the plan's harness. The arena's own
// commit_bytes is printed next to it, so an accounting bug cannot hide: the two
// numbers have to agree within a band.

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <mimalloc.h>

namespace {

struct workload {
    const char *name;
    const char *chunk;
    double band;          // max commit / logical live (0 = use 'churn_bytes')
    size_t churn_bytes;   // for the churn workload: max retained bytes
};

const char *k_wide_array =
    "T = {} "
    "local seed = 12345 "
    "for i = 1, 300 do "
    "  seed = (seed * 1103515245 + 12345) % 2147483648 "
    "  local n = 1 + seed % 5000 "
    "  local t = table.new(n) "
    "  for k = 1, n do t[k] = k end "
    "  T[i] = t "
    "end";

const char *k_hash_string =
    "T = {} "
    "for i = 1, 300 do "
    "  local t = {} "
    "  for k = 1, 200 do t['key:' .. i .. ':' .. k] = k end "
    "  for k = 1, 40 do t['a longer key that is not short:' .. i * 1000 + k] = k end "
    "  T[i] = t "
    "end";

const char *k_one_big_array =
    "T = table.new(2000000) "
    "for i = 1, 2000000 do T[i] = i end";

const char *k_churn =
    "for round = 1, 200 do "
    "  local t = table.new(2000) "
    "  for i = 1, 2000 do t[i] = i end "
    "  t = nil "
    "end";

struct measurement {
    size_t commit_delta = 0;   // mimalloc process commit over the baseline
    size_t arena_commit = 0;   // the arena's own accounting (0 for plain)
    size_t logical_live = 0;   // lua_gc(LUA_GCCOUNT) in bytes
    bool ok = false;
    const char *error = "";
};

// mimalloc's public process-info entry point (the one heap of the process).
size_t process_commit() {
    size_t elapsed = 0, user = 0, sys = 0, rss = 0, peak_rss = 0;
    size_t commit = 0, peak_commit = 0, faults = 0;
    mi_process_info(&elapsed, &user, &sys, &rss, &peak_rss, &commit, &peak_commit,
                    &faults);
    return commit;
}

int run_chunk(lua_State *L, const char *chunk) {
    int status = luaL_loadstring(L, chunk);
    if (status != LUA_OK) return status;
    return lua_pcall(L, 0, 0, 0);
}

// One fresh state, one workload. 'arena' selects the arena allocator; the plain
// state is the mimalloc baseline the plan measured.
measurement run(const workload &w, bool arena, int trim_level) {
    measurement m;
    if (process_commit() == 0) {
        m.error = "mi_process_info is not available in this build";
        return m;
    }
    const size_t before = process_commit();
    lua_State *L = nullptr;
    void *handle = nullptr;
    if (arena) {
        lua_ArenaConfig cfg;
        lua_ArenaConfigDefault(&cfg);
        L = luaL_newstate_arena(&cfg);
        handle = (L != nullptr) ? luaL_arena(L) : nullptr;
    }
    else {
        L = luaL_newstate();
    }
    if (L == nullptr) {
        m.error = "could not create the state";
        return m;
    }
    luaL_openlibs(L);
    if (run_chunk(L, w.chunk) != LUA_OK) {
        m.error = lua_tostring(L, -1);
        lua_close(L);
        return m;
    }
    // Collect first: a run ends with garbage still on the heap, and neither
    // mode should be credited for blocks the collector has not swept yet.
    lua_gc(L, LUA_GCCOLLECT, 0);
    // Bring both modes to the same state before measuring: the arena returns
    // its empty segments and purges (level 1 also drops the huge cache), the
    // plain state just purges -- which is what the plan's harness did too
    // ('mi_collect(true)' before reading the commit).
    if (handle != nullptr) lua_ArenaTrimHandle(handle, trim_level >= 0 ? trim_level : 1);
    else mi_collect(true);
    const size_t after = process_commit();
    m.commit_delta = (after > before) ? after - before : 0;
    m.logical_live = (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024u
                     + (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
    if (handle != nullptr) {
        lua_ArenaStats st;
        lua_ArenaStatsOf(handle, &st);
        m.arena_commit = st.commit_bytes;
    }
    lua_close(L);
    if (handle != nullptr) lua_ArenaFree(handle);
    m.ok = true;
    return m;
}

double ratio(size_t commit, size_t live) {
    return live ? double(commit) / double(live) : 0.0;
}

} // namespace

int main(int argc, char *argv[]) {
    bool strict = true;
    int only = -1;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-strict") == 0) strict = false;
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) only = std::atoi(argv[++i]);
    }
    const workload workloads[] = {
        {"wide_array_tables", k_wide_array, 1.55, 0},
        {"hash_string_tables", k_hash_string, 1.35, 0},
        {"one_big_array", k_one_big_array, 1.15, 0},
        {"resize_churn", k_churn, 0.0, size_t(1024 * 1024)},
    };
    int failed = 0;
    std::printf("%-20s %-10s %10s %10s %10s %8s %s\n", "workload", "mode", "live_MB",
                "commit_MB", "arena_MB", "ratio", "band");
    for (int wi = 0; wi < 4; wi++) {
        if (only >= 0 && wi != only) continue;
        const workload &w = workloads[wi];
        // trim only matters for the churn and huge-array cases; level 1 also
        // drops the huge cache and purges free mimalloc pages.
        const int trim = 1;
        std::printf("--- %s ----------------------------------------------\n", w.name);
        measurement sizes[2];
        const char *modes[2] = {"mimalloc", "arena"};
        for (int k = 0; k < 2; k++) {
            mi_collect(true);  // start each run from a clean heap
            const measurement m = run(w, k == 1, trim);
            sizes[k] = m;
            if (!m.ok) {
                std::printf("%-20s %-10s ERROR: %s\n", w.name, modes[k], m.error);
                failed++;
                continue;
            }
            if (w.churn_bytes != 0) {
                std::printf("%-20s %-10s %10.2f %10.2f %10.2f %8s retained=%zu B (band <= %zu B)\n",
                            w.name, modes[k], m.logical_live / 1048576.0,
                            m.commit_delta / 1048576.0, m.arena_commit / 1048576.0, "-",
                            m.commit_delta, w.churn_bytes);
                if (k == 1 && m.commit_delta > w.churn_bytes) {
                    std::printf("    MISS: retained %zu B is above the %zu B band\n",
                                m.commit_delta, w.churn_bytes);
                    failed++;
                }
            }
            else {
                std::printf("%-20s %-10s %10.2f %10.2f %10.2f %8.2f <= %.2f\n", w.name, modes[k],
                            m.logical_live / 1048576.0, m.commit_delta / 1048576.0,
                            m.arena_commit / 1048576.0, ratio(m.commit_delta, m.logical_live),
                            w.band);
                if (k == 1) {
                    const double r = ratio(m.commit_delta, m.logical_live);
                    if (r > w.band) {
                        std::printf("    MISS: arena ratio %.2f is above the %.2f band\n", r, w.band);
                        failed++;
                    }
                    // the arena's own accounting must agree with the OS
                    if (m.arena_commit) {
                        const double skew = double(m.arena_commit) / double(m.commit_delta ? m.commit_delta : 1);
                        if (skew > 1.25 || skew < 0.75) {
                            std::printf("    NOTE: arena accounting %.2f MB vs process commit "
                                        "%.2f MB (skew %.2f)\n",
                                        m.arena_commit / 1048576.0, m.commit_delta / 1048576.0, skew);
                        }
                    }
                }
            }
        }
        if (sizes[0].ok && sizes[1].ok && w.churn_bytes == 0 && sizes[0].logical_live > 0) {
            std::printf("    ratio change: mimalloc %.2f -> arena %.2f\n",
                        ratio(sizes[0].commit_delta, sizes[0].logical_live),
                        ratio(sizes[1].commit_delta, sizes[1].logical_live));
        }
    }
    std::printf("=====================================================\n");
    if (failed == 0) {
        std::printf("bench_lua_mem: every band met\n");
        return 0;
    }
    std::printf("bench_lua_mem: %d band(s) missed%s\n", failed,
                strict ? " (exit 1)" : " (--no-strict: exit 0)");
    return strict ? 1 : 0;
}
