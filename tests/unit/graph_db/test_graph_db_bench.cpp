// Benchmarks for src/graph_db (kimix-graph-db) — graph_db_plan.md §7, epoch 0.
// This suite covers:
// - READ:  a prebuilt 100k-vertex graph (out-degree 4..8, 64-byte content), one
//          "user op" = find + content read + dependencies + neighbors + a
//          depth/budget-bounded 3-hop expansion + has_path + shortest_path on a
//          pre-sampled near pair; measured single-threaded (baseline) and with
//          cores/2, cores and 2*cores plain std::threads, plus two per-call
//          micro-baselines (find only, content read only) for epoch-1 profiling.
// - WRITE: (a) structural create_vertex+connect on a fresh graph, one thread (the
//          single-writer contract), fixed op budget so memory stays bounded;
//          (b) level-2 content updates with W threads on DISJOINT partitions of a
//          prebuilt pool — the no-false-sharing payoff, the number that must
//          scale; (c) mixed: one structural writer thread plus the content
//          updaters, reported as two separate rates.
// - SERIALIZE: 100k and a large pass (1M in release, 200k in debug): full save,
//          dual-stage load and a 1 %-dirty incremental save, each run twice
//          (warmup then measured), timed by steady_clock AND compared against the
//          serializer's own save_ms / load_ms, with bytes_written printed.
//
// Every measurement goes to stderr as a [bench] line in tests/unit/native/
// bench_util.h's format (single-threaded cases through kimix_bench::run, the
// threaded/serialized ones through explicit steady_clock loops), and every result
// is sunk with kimix_bench::sink. THERE IS NOT ONE TIMING ASSERTION IN THIS FILE,
// on purpose: a benchmark that fails on a slow CI machine is a broken benchmark.
// GATED: only runs when KIMIX_GRAPH_DB_BENCH=1 is set (same convention as
// KIMIX_PDQSORT_BENCH / KIMIX_GREP_BENCH); the fixtures take minutes to build
// in debug, so a plain test run skips the suite:
//   set KIMIX_GRAPH_DB_BENCH=1 && xmake run test_graph_db_bench
// The only expects() here are methodology checks — the fiber scheduler is bound,
// the worker bodies really ran on N distinct OS threads, the workloads really did
// work, and the serializer's counts are exact (saved == N, incremental == N/100).
//
// EPOCH-1 NOTE (read scaling): epoch 0 measured every structural read (find,
// dependencies, neighbors, has_path, shortest_path, and the traversal's
// dependencies() calls) against the graph's single std::shared_mutex in SHARED
// mode, so reader threads contended on one lock word and the aggregate rate did
// not scale with cores. graph.h now shards that lock (kGraphShardCount buckets,
// shard = hash(guid) & (mask)), so a reader of vertex V only contends with
// V's shard. The per-call baselines below are unchanged on purpose: they are the
// SAME workload as epoch 0, which is what makes the two rows comparable. The
// bounded expansion additionally switched to the non-allocating
// dependencies(guid, AdjacencyBuffer&) form, so one user op no longer allocates
// a kimix::vector per visited vertex; the allocating per-call baselines keep
// using the compatibility forms, as a third party would.
// An unbounded has_path over random far pairs is deliberately NOT in the mix: its
// cost is O(whole reachable set), so it would measure BFS-set size rather than the
// container.
//
// THREADING: the benchmark threads are plain std::thread, never fibers — a fiber
// would hide exactly the lock contention this suite measures. Fibers are only used
// inside the serializer's own fan-out, which is why main() binds a scheduler first
// (the fiber skill's root-main pattern: declared first so it outlives every suite
// Boost.UT runs from the _test destructors at the end of main()).
//
// CONTRACT HYGIENE (graph.h): structural writes come from exactly one thread per
// phase. The content pool is built by the bench thread BEFORE any worker starts,
// and update_content is a level-2 operation that is explicitly exempt from the
// single-writer rule, so the updaters need no ownership hand-over. In the mixed
// phase the structural writer thread calls Graph::reset_writer_thread() as its
// first action (the documented test helper) so it may own the structural writes of
// a graph the bench thread built, and it only ever touches the vertices it creates
// — never the updaters' pool, which is what makes the concurrent mix legal.
//
// Sizes and durations are the constants at the top of the anonymous namespace.
// The large serialization pass is reduced in debug builds (NDEBUG): the release
// run is the epoch record, and a debug run only has to prove the suite still
// passes. The printed lines always name the size that was actually used.

#include "ut/ut.hpp"
#include "unit/native/bench_util.h" // tests/ is the include root: shared bench harness
#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first among the core headers (winsock2 order)
#include <core/fiber.h>
#include <core/platform.h>

#include <graph_db/graph.h>
#include <graph_db/graph_db.h>
#include <graph_db/serialize.h>
#include <graph_db/utils.h>
#include <graph_db/vertex.h>

#include <core/guid.h>
#include <core/stl/format.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib> // std::getenv (KIMIX_GRAPH_DB_BENCH gate)
#include <random>
#include <thread>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::ColdStorage;
using graph_db::Graph;
using graph_db::GraphDeserializer;
using graph_db::GraphSerializeStats;
using graph_db::GraphSerializer;
using graph_db::Vertex;
namespace gtest = kimix::graph_db::test;

namespace {

using clock_type = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Shared read-benchmark graph: vertices, out-degree window, content size.
constexpr size_t kReadVertices = 100'000;
constexpr size_t kReadDegreeMin = 4;
constexpr size_t kReadDegreeMax = 8;
constexpr size_t kReadContentBytes = 64;
// Wall duration of one reader configuration (the plan asks for 1-2 s).
constexpr double kReadSeconds = 1.0;
// Shorter window for the find-only lock-isolation probe (it is cheap per op).
constexpr double kFindSeconds = 0.5;
// A user op expands at most kReachNodeBudget vertices over kReachDepth hops, so a
// traversal costs a bounded number of dependencies() calls (a real UI query limit).
constexpr unsigned kReachDepth = 3;
constexpr size_t kReachNodeBudget = 64;
// Pre-sampled near pairs (1-2 outgoing hops apart) for has_path / shortest_path.
constexpr size_t kPairCount = 4096;
// Random start guids the readers pick from.
constexpr size_t kGuidSampleCount = 8192;
// Payload size an updater writes (level-2 content write).
constexpr size_t kContentBytes = 96;

// Structural write bench: a fixed op budget, so the graph (and the RSS) stays
// bounded instead of growing for a whole time window.
constexpr size_t kStructuralOps = 200'000;

// Content-update bench: a prebuilt pool split into disjoint partitions.
constexpr size_t kContentPoolVertices = 40'000;
constexpr double kWriteSeconds = 1.0;

// Mixed bench: one structural writer + updaters over a separate pool.
constexpr size_t kMixedPoolVertices = 20'000;
constexpr double kMixedSeconds = 1.0;

// Serialization bench.
constexpr size_t kSerializeVertices = 100'000;
#if defined(NDEBUG)
constexpr size_t kSerializeLargeVertices = 1'000'000;
#else
// Debug builds are a correctness smoke check, not the epoch record: 1M vertices
// costs minutes there. The [bench] line prints the size that was used.
constexpr size_t kSerializeLargeVertices = 200'000;
#endif
constexpr size_t kSerializeContentBytes = 64;
// Out-degree window of every generated write/serialize graph (same shape as the
// read graph, so the three benchmark groups measure comparable structures).
constexpr size_t kGraphDegreeMin = 4;
constexpr size_t kGraphDegreeMax = 8;

// ---------------------------------------------------------------------------
// stderr reporting — the same layout kimix_bench::run uses, so the whole run
// reads as one table. Everything here is informational; nothing asserts on it.
// ---------------------------------------------------------------------------

inline double elapsed_ms(clock_type::time_point from, clock_type::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// One logical operation of a steady_clock loop (op count + elapsed window).
inline void print_rate(char const *name, double ops, double seconds,
                       char const *suffix) noexcept {
    double const safe_seconds = seconds > 0.0 ? seconds : 1e-9;
    double const safe_ops = ops > 0.0 ? ops : 1.0;
    std::fprintf(stderr, "[bench] %-44s %12.1f ns/op %14.0f ops/s  (%s)\n", name,
                 seconds * 1e9 / safe_ops, ops / safe_seconds, suffix);
}

// One whole-operation measurement (a save, a load, an incremental save): the
// "ops/s" column is then "operations of this kind per second" (fractional — a
// 1.4 s save really is 0.7 saves/s).
inline void print_elapsed(char const *name, double ms, char const *suffix) noexcept {
    double const safe_ms = ms > 0.0 ? ms : 1e-9;
    std::fprintf(stderr, "[bench] %-44s %12.3f ms   %14.3f ops/s  (%s)\n", name, ms,
                 1000.0 / safe_ms, suffix);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// std::thread::id has no portable printed form, so every threaded benchmark prints
// the distinct hash stamps of its workers: proof that N threads really ran.
inline size_t current_thread_hash() noexcept {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

inline size_t hardware_threads() noexcept {
    unsigned const reported = std::thread::hardware_concurrency();
    return reported > 0 ? static_cast<size_t>(reported) : size_t{4};
}

// Deterministic pseudo-random bytes: the same seed always yields the same blob (a
// run is reproducible) and a different seed yields a different blob (so a content
// write really changes the vertex's cached MD5).
kimix::vector<std::byte> make_payload(size_t seed, size_t size) {
    kimix::vector<std::byte> out(size);
    uint64_t x = (static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull) | 1ull;
    for (size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<std::byte>(x & 0xFFu);
    }
    return out;
}

// Stamp `value` into the first 8 bytes of `payload`, so repeated writes to the same
// vertex store different bytes without allocating anything per operation.
inline void stamp(kimix::vector<std::byte> &payload, uint64_t value) noexcept {
    for (size_t i = 0; i < 8 && i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
}

// A random DAG with an explicit out-degree window: every vertex depends on
// kDegreeMin..kDegreeMax strictly lower-indexed vertices, so the graph is acyclic by
// construction and every traversal terminates. Deterministic for a fixed seed.
kimix::vector<Vertex *> build_random_dag(Graph &graph, size_t count, size_t degree_min,
                                         size_t degree_max, uint32_t seed,
                                         size_t content_bytes) {
    kimix::vector<Vertex *> out;
    out.reserve(count);
    size_t const deg_min = degree_min == 0 ? 1 : degree_min;
    size_t const deg_max = degree_max < deg_min ? deg_min : degree_max;
    for (size_t i = 0; i < count; ++i) {
        auto content = make_payload(i + 1, content_bytes);
        Vertex *vertex = graph.create_vertex(content);
        if (vertex == nullptr) {
            break;
        }
        out.push_back(vertex);
    }
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> degree_dist(deg_min, deg_max);
    for (size_t i = 1; i < out.size(); ++i) {
        size_t const want = degree_dist(rng);
        for (size_t k = 0; k < want; ++k) {
            // A duplicate edge is simply refused by Graph::connect (one wasted
            // lookup); with a random target out of `i` candidates that is rare.
            size_t const target = static_cast<size_t>(rng() % i);
            graph.connect(out[i]->guid(), out[target]->guid());
        }
    }
    return out;
}

// Two guids 1-2 outgoing hops apart: has_path / shortest_path then do bounded work.
struct near_pair {
    kimix::Guid from;
    kimix::Guid to;
};

// Pre-sample near pairs OUTSIDE the measured loop, so a reader only looks one up.
kimix::vector<near_pair>
sample_near_pairs(Graph const &graph, kimix::vector<Vertex *> const &vertices,
                  size_t wanted, uint32_t seed) {
    kimix::vector<near_pair> out;
    out.reserve(wanted);
    std::mt19937 rng(seed);
    size_t guard = 0;
    size_t const guard_limit = wanted * 64u + 64u;
    while (out.size() < wanted && guard < guard_limit && !vertices.empty()) {
        ++guard;
        Vertex *start = vertices[rng() % vertices.size()];
        if (start == nullptr) {
            continue;
        }
        auto deps = graph.dependencies(start->guid());
        if (deps.empty()) {
            continue;
        }
        near_pair pair;
        pair.from = start->guid();
        pair.to = deps[rng() % deps.size()];
        if ((rng() % 4u) == 0u) {
            // Every fourth pair is two hops out, so both BFS levels get exercised.
            auto second = graph.dependencies(pair.to);
            if (!second.empty()) {
                pair.to = second[rng() % second.size()];
            }
        }
        out.push_back(pair);
    }
    return out;
}

// Per-thread scratch for the bounded expansion (reused, never reallocated per op).
struct reach_scratch {
    kimix::vector<kimix::Guid> frontier;
    kimix::vector<kimix::Guid> next;
    kimix::vector<kimix::Guid> visited;
    // Epoch 1: reusable adjacency scratch, so the per-visit edges come out of one
    // inline buffer instead of a fresh kimix::vector (the expansion used to issue
    // ~30 allocating dependencies() calls per user op).
    graph_db::AdjacencyBuffer adjacency;
};

// Depth- and node-budget-bounded expansion over OUTGOING edges: the "show me the
// 3-hop neighbourhood, at most 64 nodes" query a UI issues through the public API.
// Returns the number of distinct vertices reached, `start` excluded.
size_t bounded_reach(Graph const &graph, kimix::Guid const &start, unsigned max_depth,
                     size_t node_budget, reach_scratch &scratch) {
    scratch.frontier.clear();
    scratch.visited.clear();
    scratch.frontier.push_back(start);
    scratch.visited.push_back(start);
    unsigned depth = 0;
    while (!scratch.frontier.empty() && depth < max_depth &&
           scratch.visited.size() < node_budget) {
        scratch.next.clear();
        for (auto const &guid : scratch.frontier) {
            // One shard SHARED lock, no allocation (epoch 1 form).
            if (!graph.dependencies(guid, scratch.adjacency)) {
                continue;
            }
            for (auto const &dep : scratch.adjacency) {
                bool seen = false;
                for (auto const &known : scratch.visited) {
                    if (known == dep) {
                        seen = true;
                        break;
                    }
                }
                if (seen) {
                    continue;
                }
                scratch.visited.push_back(dep);
                scratch.next.push_back(dep);
                if (scratch.visited.size() >= node_budget) {
                    break;
                }
            }
            if (scratch.visited.size() >= node_budget) {
                break;
            }
        }
        scratch.frontier.swap(scratch.next);
        ++depth;
    }
    return scratch.visited.size() - 1;
}

// ---------------------------------------------------------------------------
// READ fixture: one 100k-vertex graph, built once and only ever READ afterwards,
// so the four reader configurations can share it.
// ---------------------------------------------------------------------------

struct read_fixture {
    Graph graph;
    kimix::vector<kimix::Guid> sample; // random start guids
    kimix::vector<near_pair> pairs;    // near pairs for has_path / shortest_path

    read_fixture() {
        auto vertices = build_random_dag(graph, kReadVertices, kReadDegreeMin,
                                         kReadDegreeMax, 20240601u, kReadContentBytes);
        std::mt19937 rng(7u);
        sample.reserve(kGuidSampleCount);
        for (size_t i = 0; i < kGuidSampleCount && !vertices.empty(); ++i) {
            sample.push_back(vertices[rng() % vertices.size()]->guid());
        }
        pairs = sample_near_pairs(graph, vertices, kPairCount, 11u);
    }
};

read_fixture &read_data() {
    static read_fixture fixture; // function-local static: built once, on first use
    return fixture;
}

struct reader_scratch {
    kimix::vector<std::byte> content; // reused content buffer (no per-op allocation)
    reach_scratch reach;
    uint32_t rng_state{0x2545F491u};

    uint32_t next_random() noexcept {
        uint32_t x = rng_state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng_state = x != 0u ? x : 0x2545F491u;
        return rng_state;
    }
};

// ONE simulated-user read: the 7-call script described at the top of the file.
// Returns a value folded from every call's output, so nothing can be optimized away.
uint64_t one_user_op(read_fixture const &data, reader_scratch &scratch) {
    if (data.sample.empty()) {
        return 0;
    }
    kimix::Guid const guid = data.sample[scratch.next_random() % data.sample.size()];
    uint64_t acc = 0;
    // 1. guid -> Vertex* (graph SHARED lock, hash lookup)
    Vertex *vertex = data.graph.find(guid);
    if (vertex != nullptr) {
        // 2. content read (vertex SHARED lock)
        acc += vertex->content_size();
        vertex->content(scratch.content);
        acc += scratch.content.size();
    }
    // 3. outgoing edges (graph SHARED lock)
    auto deps = data.graph.dependencies(guid);
    acc += deps.size();
    // 4. undirected adjacency (graph SHARED lock)
    auto adjacent = data.graph.neighbors(guid);
    acc += adjacent.size();
    // 5. bounded 3-hop expansion over outgoing edges
    acc += bounded_reach(data.graph, guid, kReachDepth, kReachNodeBudget, scratch.reach);
    // 6./7. reachability + shortest path on a pre-sampled near pair
    if (!data.pairs.empty()) {
        near_pair const &pair = data.pairs[scratch.next_random() % data.pairs.size()];
        acc += data.graph.has_path(pair.from, pair.to) ? 1u : 0u;
        acc += data.graph.shortest_path(pair.from, pair.to).size();
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Threaded harness: plain std::threads, released together, each running its body
// until the shared deadline. Spawn latency (tens of microseconds) is inside the
// measured window and is negligible next to the 1 s windows used here.
// ---------------------------------------------------------------------------

struct thread_stat {
    uint64_t ops{0};
    uint64_t checksum{0};
    size_t thread_hash{0};
};

struct parallel_result {
    double seconds{0.0};
    uint64_t total_ops{0};
    uint64_t checksum{0};
    size_t distinct_threads{0};
    kimix::vector<thread_stat> per_thread;
};

template<typename Body>
parallel_result run_parallel(size_t thread_count, double seconds, Body &&body) {
    kimix::vector<thread_stat> stats(thread_count);
    kimix::vector<std::thread> threads;
    threads.reserve(thread_count);
    std::atomic<bool> go{false};
    for (size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back([&go, &body, &stats, i, seconds]() {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto const window = std::chrono::duration_cast<clock_type::duration>(
                std::chrono::duration<double>(seconds));
            auto const deadline = clock_type::now() + window;
            body(i, deadline, stats[i]);
        });
    }
    auto const start = clock_type::now();
    go.store(true, std::memory_order_release);
    for (auto &thread : threads) {
        thread.join();
    }
    auto const finish = clock_type::now();

    parallel_result result;
    result.seconds = std::chrono::duration<double>(finish - start).count();
    for (auto const &stat : stats) {
        result.total_ops += stat.ops;
        result.checksum += stat.checksum;
    }
    kimix::vector<size_t> ids;
    ids.reserve(stats.size());
    for (auto const &stat : stats) {
        ids.push_back(stat.thread_hash);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    result.distinct_threads = ids.size();
    result.per_thread = std::move(stats);
    return result;
}

// cores/2, cores, 2*cores — deduplicated and ordered, with a floor of 1 thread.
kimix::vector<size_t> thread_configs(size_t cores) {
    kimix::vector<size_t> out;
    auto add = [&out](size_t threads) {
        if (threads == 0) {
            threads = 1;
        }
        for (auto existing : out) {
            if (existing == threads) {
                return;
            }
        }
        out.push_back(threads);
    };
    add(cores / 2);
    add(cores);
    add(cores * 2);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// SERIALIZATION pass: build -> full save (warmup + measured) -> dual-stage load
// (warmup + measured) -> 1 % dirty -> incremental save. Prints its own [bench]
// lines and returns the numbers so the caller can assert the exact counts.
// ---------------------------------------------------------------------------

struct serialize_numbers {
    size_t vertices{0};
    size_t full_save_count{0};
    size_t full_save_bytes{0};
    double full_save_ms{0.0};
    double serializer_save_ms{0.0};
    size_t loaded_count{0};
    size_t loaded_skipped{0};
    double load_ms{0.0};
    double serializer_load_ms{0.0};
    size_t dirtied{0};
    size_t incremental_count{0};
    size_t incremental_bytes{0};
    double incremental_ms{0.0};
    bool ok{false};
};

serialize_numbers run_serialize_pass(size_t vertices, char const *tag, uint32_t seed) {
    serialize_numbers numbers;
    numbers.vertices = vertices;

    gtest::temp_dir_guard dir{"graph_db_bench"};
    if (!dir.ok()) {
        std::fprintf(stderr, "[bench] graphdb/serialize/%s: temp dir unavailable\n", tag);
        return numbers;
    }
    ColdStorage cold;
    if (!cold.open(dir.path())) {
        return numbers;
    }
    GraphSerializer serializer{cold};
    GraphDeserializer deserializer;
    Graph source;
    auto built = build_random_dag(source, vertices, kGraphDegreeMin,
                                  kGraphDegreeMax, seed, kSerializeContentBytes);
    if (source.size() != vertices || built.size() != vertices) {
        return numbers;
    }

    // Warmup save (untimed), then the measured one: the first pass pays the LMDB
    // page-allocation and mimalloc warm-up costs, which is exactly why the plan
    // asks for a warmup run.
    serializer.save(source);
    auto const save_start = clock_type::now();
    GraphSerializeStats saved = serializer.save(source);
    auto const save_end = clock_type::now();
    numbers.full_save_ms = elapsed_ms(save_start, save_end);
    numbers.serializer_save_ms = saved.save_ms;
    numbers.full_save_count = saved.saved_count;
    numbers.full_save_bytes = saved.bytes_written;
    {
        double const mbytes_per_s =
            saved.bytes_written / (numbers.full_save_ms > 0.0
                                       ? numbers.full_save_ms * 1e3
                                       : 1e3);
        char suffix[512];
        std::snprintf(suffix, sizeof(suffix),
                      "%zu vertices, %zu bytes, %.1f MB/s, serializer stats.save_ms=%.3f, "
                      "workers=%zu",
                      vertices, saved.bytes_written, mbytes_per_s, saved.save_ms,
                      saved.worker_count);
        print_elapsed((kimix::string{"graphdb/serialize/"} + tag + "/full_save").c_str(),
                      numbers.full_save_ms, suffix);
    }

    // Warmup load (untimed) into a throw-away graph, then the measured load.
    {
        Graph warmup_target;
        deserializer.load(cold, warmup_target);
    }
    {
        Graph target;
        auto const load_start = clock_type::now();
        GraphSerializeStats loaded = deserializer.load(cold, target);
        auto const load_end = clock_type::now();
        numbers.load_ms = elapsed_ms(load_start, load_end);
        numbers.serializer_load_ms = loaded.load_ms;
        numbers.loaded_count = loaded.loaded_count;
        numbers.loaded_skipped = loaded.skipped_count;
        double const mbytes_per_s =
            loaded.bytes_read / (numbers.load_ms > 0.0 ? numbers.load_ms * 1e3 : 1e3);
        char suffix[512];
        std::snprintf(suffix, sizeof(suffix),
                      "%zu vertices, %zu bytes read, %.1f MB/s, loaded=%zu, skipped=%zu, "
                      "serializer stats.load_ms=%.3f, workers=%zu",
                      vertices, loaded.bytes_read, mbytes_per_s, loaded.loaded_count,
                      loaded.skipped_count, loaded.load_ms, loaded.worker_count);
        print_elapsed((kimix::string{"graphdb/serialize/"} + tag + "/load").c_str(),
                      numbers.load_ms, suffix);
    }

    // 1 % dirty -> incremental save. Every vertex is clean after the full save, so
    // exactly the touched ones may be written (asserted by the caller).
    size_t const dirty_target = vertices / 100;
    auto payload = make_payload(0x51EDu, kSerializeContentBytes);
    for (size_t i = 0; i < dirty_target && i < built.size(); ++i) {
        stamp(payload, i);
        if (source.update_content(built[i]->guid(), payload)) {
            ++numbers.dirtied;
        }
    }
    auto const incremental_start = clock_type::now();
    GraphSerializeStats incremental = serializer.save_incremental(source);
    auto const incremental_end = clock_type::now();
    numbers.incremental_ms = elapsed_ms(incremental_start, incremental_end);
    numbers.incremental_count = incremental.saved_count;
    numbers.incremental_bytes = incremental.bytes_written;
    {
        char suffix[512];
        std::snprintf(suffix, sizeof(suffix),
                      "1%% of %zu vertices dirtied (%zu), written=%zu, %zu bytes",
                      vertices, numbers.dirtied, incremental.saved_count,
                      incremental.bytes_written);
        print_elapsed((kimix::string{"graphdb/serialize/"} + tag + "/incremental_1pct")
                          .c_str(),
                      numbers.incremental_ms, suffix);
    }

    numbers.ok = true;
    return numbers;
}

} // namespace

int main(int argc, char *argv[]) {
      boost::ut::detail::cfg::parse_arg_with_fallback(
          argc, const_cast<const char **>(argv));
      // Gated like the other benches (KIMIX_PDQSORT_BENCH, KIMIX_GREP_BENCH):
      // the fixture build plus the timed windows take ~3 minutes even in a
      // debug build, so a plain `xmake test` run skips the suite.
      const char *gate = std::getenv("KIMIX_GRAPH_DB_BENCH");
      if (gate == nullptr || gate[0] == '0') {
          std::fprintf(stderr, "[bench] KIMIX_GRAPH_DB_BENCH not set; skipping.\n");
          return 0;
      }
      // FIRST, before any suite can run: bind a scheduler for the whole process. The
    // benchmark threads are plain std::threads, but the serializer's save/load
    // fan-outs are kimix::fiber::parallel and need it bound on the calling thread.
    // Declared first so it outlives every suite Boost.UT runs from the _test
    // destructors at the end of main() (the fiber skill's root-main pattern).
    kimix::fiber::scheduler sched{};

    const size_t cores = hardware_threads();

    "bench_env_banner"_test = [cores] {
        expect(kimix::fiber::is_bound())
            << "the serializer's fan-out needs a scheduler bound to this thread";
        expect(ge(kimix::fiber::worker_thread_count(), 2u))
            << "a real fan-out must be possible, not the inline fallback";
        kimix::string cpu = kimix::cpu_name();
        // Keep only printable ASCII: the brand string comes straight from CPUID.
        for (auto &ch : cpu) {
            if (static_cast<unsigned char>(ch) < 0x20 ||
                static_cast<unsigned char>(ch) > 0x7E) {
                ch = ' ';
            }
        }
        while (!cpu.empty() && cpu.back() == ' ') {
            cpu.pop_back();
        }
        std::fprintf(stderr,
                     "[bench] graph_db bench: mode=%s cpu=\"%s\" hw_threads=%zu "
                     "scheduler_workers=%u graph_shards=%zu\n",
#if defined(NDEBUG)
                     "release",
#else
                     "debug",
#endif
                     cpu.c_str(), cores,
                     static_cast<unsigned>(kimix::fiber::worker_thread_count()),
                     graph_db::kGraphShardCount);
        std::fprintf(stderr,
                     "[bench] sizes: read_graph=%zu vertices (out-degree %zu..%zu, %zu B "
                     "content), reach=%u hops/%zu nodes, content pool=%zu, structural "
                     "ops=%zu, serialize=%zu + %zu, window=%.2f s\n",
                     kReadVertices, kReadDegreeMin, kReadDegreeMax, kReadContentBytes,
                     kReachDepth, kReachNodeBudget, kContentPoolVertices, kStructuralOps,
                     kSerializeVertices, kSerializeLargeVertices, kReadSeconds);
        kimix_bench::sink(cpu.size());
    };

    // -----------------------------------------------------------------------
    // 1. READ
    // -----------------------------------------------------------------------
    "bench_read_scaling"_test = [cores] {
        read_fixture &data = read_data();
        expect(eq(data.graph.size(), kReadVertices)) << "the read graph is complete";
        expect(!data.sample.empty()) << "the reader has start guids";
        expect(!data.pairs.empty()) << "has_path/shortest_path have near pairs";
        if (data.sample.empty() || data.pairs.empty()) {
            return;
        }

        // Single-thread baseline of the whole user op, and two per-call baselines
        // (epoch-1 profiling fodder: hash lookup vs vertex-lock content read).
        {
            reader_scratch scratch;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_user_op",
                [&data, &scratch, &sink] {
                    sink += one_user_op(data, scratch);
                },
                1, 0.0, 0.3);
            kimix_bench::sink(sink);
        }
        {
            size_t cursor = 0;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_find_only",
                [&data, &cursor, &sink] {
                    Vertex *vertex =
                        data.graph.find(data.sample[cursor % data.sample.size()]);
                    ++cursor;
                    sink += vertex != nullptr ? 1u : 0u;
                },
                1, 0.0, 0.3);
            kimix_bench::sink(sink);
        }
        {
            size_t cursor = 0;
            kimix::vector<std::byte> out;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_content_read_only",
                [&data, &cursor, &sink, &out] {
                    Vertex *vertex =
                        data.graph.find(data.sample[cursor % data.sample.size()]);
                    ++cursor;
                    if (vertex != nullptr) {
                        vertex->content(out); // vertex SHARED lock, no graph lock
                        sink += out.size();
                    }
                },
                1, 0.0, 0.3);
            kimix_bench::sink(sink);
        }
        // Per-call breakdown of the composite op: this is the epoch-1 profile
        // starting point (which of the 7 calls owns the whole user op?).
        {
            size_t cursor = 0;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_dependencies",
                [&data, &cursor, &sink] {
                    sink += data.graph
                                .dependencies(data.sample[cursor % data.sample.size()])
                                .size();
                    ++cursor;
                },
                1, 0.0, 0.2);
            kimix_bench::sink(sink);
        }
        {
            // Same call through the non-allocating form: the delta against
            // 1T_dependencies above is the allocation cost epoch 1 removed.
            size_t cursor = 0;
            graph_db::AdjacencyBuffer adjacency;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_dependencies_noalloc",
                [&data, &cursor, &sink, &adjacency] {
                    if (data.graph
                            .dependencies(data.sample[cursor % data.sample.size()],
                                          adjacency)) {
                        sink += adjacency.size();
                    }
                    ++cursor;
                },
                1, 0.0, 0.2);
            kimix_bench::sink(sink);
        }
        {
            size_t cursor = 0;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_neighbors",
                [&data, &cursor, &sink] {
                    sink += data.graph
                                .neighbors(data.sample[cursor % data.sample.size()])
                                .size();
                    ++cursor;
                },
                1, 0.0, 0.2);
            kimix_bench::sink(sink);
        }
        {
            size_t cursor = 0;
            reach_scratch reach;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_bounded_reach_3hop_64",
                [&data, &cursor, &sink, &reach] {
                    sink += bounded_reach(data.graph,
                                          data.sample[cursor % data.sample.size()],
                                          kReachDepth, kReachNodeBudget, reach);
                    ++cursor;
                },
                1, 0.0, 0.2);
            kimix_bench::sink(sink);
        }
        {
            size_t cursor = 0;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_has_path_near",
                [&data, &cursor, &sink] {
                    near_pair const &pair = data.pairs[cursor % data.pairs.size()];
                    ++cursor;
                    sink += data.graph.has_path(pair.from, pair.to) ? 1u : 0u;
                },
                1, 0.0, 0.2);
            kimix_bench::sink(sink);
        }
        {
            size_t cursor = 0;
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/read/1T_shortest_path_near",
                [&data, &cursor, &sink] {
                    near_pair const &pair = data.pairs[cursor % data.pairs.size()];
                    ++cursor;
                    sink += data.graph.shortest_path(pair.from, pair.to).size();
                },
                1, 0.0, 0.2);
            kimix_bench::sink(sink);
        }

        // Lock-isolation probe: `find` alone under the same thread counts. It takes
        // the graph SHARED lock and nothing else, so comparing it against the 1T
        // baseline isolates graph-lock contention from allocation and traversal.
        for (size_t threads : thread_configs(cores)) {
            auto result = run_parallel(
                threads, kFindSeconds,
                [&data](size_t index, clock_type::time_point deadline,
                        thread_stat &stat) {
                    stat.thread_hash = current_thread_hash();
                    size_t cursor = index * 7919u;
                    uint64_t acc = 0;
                    uint64_t ops = 0;
                    while (clock_type::now() < deadline) {
                        Vertex *vertex =
                            data.graph.find(data.sample[cursor % data.sample.size()]);
                        ++cursor;
                        acc += vertex != nullptr ? 1u : 0u;
                        ++ops;
                    }
                    stat.ops = ops;
                    stat.checksum = acc;
                });
            if (threads >= 2) {
                expect(ge(result.distinct_threads, size_t{2}))
                    << "the find-only probe really ran on distinct OS threads";
            }
            char suffix[256];
            std::snprintf(suffix, sizeof(suffix),
                          "%zu threads, %zu distinct OS thread ids, %llu find() calls "
                          "(one SHARD shared lock only)",
                          threads, result.distinct_threads,
                          static_cast<unsigned long long>(result.total_ops));
            kimix::string name = kimix::format("graphdb/read/{}T_find_only", threads);
            print_rate(name.c_str(), static_cast<double>(result.total_ops),
                       result.seconds, suffix);
            kimix_bench::sink(result.total_ops + result.checksum);
        }

        // Simulated users: cores/2, cores, 2*cores plain threads, 1 s each.
        for (size_t threads : thread_configs(cores)) {
            auto result = run_parallel(
                threads, kReadSeconds,
                [&data](size_t index, clock_type::time_point deadline,
                        thread_stat &stat) {
                    stat.thread_hash = current_thread_hash();
                    reader_scratch scratch;
                    scratch.rng_state = 0x9E3779B9u ^
                                        static_cast<uint32_t>(stat.thread_hash) ^
                                        static_cast<uint32_t>(index * 2654435761u);
                    uint64_t acc = 0;
                    uint64_t ops = 0;
                    while (clock_type::now() < deadline) {
                        acc += one_user_op(data, scratch);
                        ++ops;
                    }
                    stat.ops = ops;
                    stat.checksum = acc;
                });
            expect(gt(result.total_ops, uint64_t{0})) << "the readers did work";
            if (threads >= 2) {
                expect(ge(result.distinct_threads, size_t{2}))
                    << "the reader bodies really ran on distinct OS threads";
            }
            char suffix[256];
            std::snprintf(suffix, sizeof(suffix),
                          "%zu threads, %zu distinct OS thread ids, %llu user-ops (7 graph "
                          "calls each), one shard lock per vertex visit",
                          threads, result.distinct_threads,
                          static_cast<unsigned long long>(result.total_ops));
            kimix::string name = kimix::format("graphdb/read/{}T_user_op", threads);
            print_rate(name.c_str(), static_cast<double>(result.total_ops),
                       result.seconds, suffix);
            kimix_bench::sink(result.total_ops + result.checksum);
        }
    };

    // -----------------------------------------------------------------------
    // 2a. WRITE — structural (create + connect), single thread by contract
    // -----------------------------------------------------------------------
    "bench_write_structural"_test = [] {
        Graph graph;
        std::mt19937 rng(20240605u);
        kimix::vector<kimix::Guid> guids;
        guids.reserve(kStructuralOps);
        auto payload = make_payload(3u, kSerializeContentBytes);

        auto const start = clock_type::now();
        size_t created = 0;
        size_t connects = 0;
        for (size_t i = 0; i < kStructuralOps; ++i) {
            Vertex *vertex = graph.create_vertex(payload); // graph UNIQUE lock
            if (vertex == nullptr) {
                break;
            }
            guids.push_back(vertex->guid());
            ++created;
            if (i > 0) {
                size_t const target = static_cast<size_t>(rng() % i);
                if (graph.connect(vertex->guid(), guids[target])) { // graph UNIQUE lock
                    ++connects;
                }
            }
        }
        auto const finish = clock_type::now();
        double const seconds = std::chrono::duration<double>(finish - start).count();

        expect(eq(created, kStructuralOps)) << "every create_vertex succeeded";
        expect(eq(connects, kStructuralOps - 1)) << "every connect succeeded";
        expect(eq(graph.size(), kStructuralOps));
        char suffix[256];
        std::snprintf(suffix, sizeof(suffix),
                      "%zu create_vertex + %zu connect on a fresh graph, ONE thread "
                      "(single-writer contract), 1 vertex + 1 edge per op",
                      created, connects);
        print_rate("graphdb/write/structural_create_connect",
                   static_cast<double>(created), seconds, suffix);
        kimix_bench::sink(created + connects);
    };

    // -----------------------------------------------------------------------
    // 2b. WRITE — level-2 content updates on disjoint partitions
    // -----------------------------------------------------------------------
    "bench_write_content_updates"_test = [cores] {
        Graph graph;
        // The pool is built HERE, on the bench thread, before any worker exists.
        // update_content is a level-2 operation and graph.h exempts it from the
        // single-writer rule, so the updaters need no ownership hand-over — and
        // they only ever touch their own disjoint slice.
        auto pool = build_random_dag(graph, kContentPoolVertices, kGraphDegreeMin,
                                     kGraphDegreeMax, 20240606u,
                                     kSerializeContentBytes);
        expect(eq(graph.size(), kContentPoolVertices)) << "the content pool is complete";
        if (pool.size() != kContentPoolVertices) {
            return;
        }

        // Single-thread baseline (one update_content per op, warm pool).
        {
            size_t cursor = 0;
            auto payload = make_payload(13u, kContentBytes);
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/write/content_update/1T",
                [&graph, &pool, &payload, &cursor, &sink] {
                    stamp(payload, cursor);
                    Vertex *vertex = pool[cursor % pool.size()];
                    ++cursor;
                    if (graph.update_content(vertex->guid(), payload)) {
                        ++sink;
                    }
                },
                1, 0.0, 0.3);
            kimix_bench::sink(sink);
        }

        for (size_t threads : thread_configs(cores)) {
            auto result = run_parallel(
                threads, kWriteSeconds,
                [&graph, &pool, threads](size_t index, clock_type::time_point deadline,
                                         thread_stat &stat) {
                    stat.thread_hash = current_thread_hash();
                    // Disjoint partition: worker `index` owns one contiguous slice,
                    // so no two workers ever write the same vertex (the no-false-
                    // sharing payoff: different cache lines, different locks).
                    size_t const share = (pool.size() + threads - 1) / threads;
                    size_t const begin = std::min(pool.size(), index * share);
                    size_t const end = std::min(pool.size(), begin + share);
                    if (begin >= end) {
                        return;
                    }
                    auto payload = make_payload(index + 100u, kContentBytes);
                    uint64_t ops = 0;
                    uint64_t ok = 0;
                    size_t cursor = begin;
                    while (clock_type::now() < deadline) {
                        stamp(payload, ops);
                        Vertex *vertex = pool[cursor];
                        if (graph.update_content(vertex->guid(), payload)) {
                            ++ok;
                        }
                        if (++cursor >= end) {
                            cursor = begin;
                        }
                        ++ops;
                    }
                    stat.ops = ops;
                    stat.checksum = ok;
                });
            expect(gt(result.total_ops, uint64_t{0})) << "the updaters did work";
            if (threads >= 2) {
                expect(ge(result.distinct_threads, size_t{2}))
                    << "the updater bodies really ran on distinct OS threads";
            }
            char suffix[256];
            std::snprintf(suffix, sizeof(suffix),
                          "%zu threads, %zu distinct OS thread ids, %llu update_content "
                          "on disjoint %zu-vertex partitions (one shard shared lock for the "
                          "lookup, no structural lock)",
                          threads, result.distinct_threads,
                          static_cast<unsigned long long>(result.total_ops),
                          kContentPoolVertices);
            kimix::string name = kimix::format("graphdb/write/content_update/{}T", threads);
            print_rate(name.c_str(), static_cast<double>(result.total_ops),
                       result.seconds, suffix);
            kimix_bench::sink(result.total_ops + result.checksum);
        }
        // Isolate the level-2 path from the graph lock: `set_content` on an already
        // resolved Vertex* takes the vertex UNIQUE lock and nothing else. If THIS
        // scales but the number above does not, the cost left is the graph shared
        // lock in update_content's guid resolution — the epoch-1 target, and the
        // proof that the per-vertex locks themselves do not false-share.
        {
            size_t cursor = 0;
            auto payload = make_payload(17u, kContentBytes);
            uint64_t sink = 0;
            kimix_bench::run(
                "graphdb/write/vertex_set_content/1T",
                [&pool, &payload, &cursor, &sink] {
                    stamp(payload, cursor);
                    pool[cursor % pool.size()]->set_content(payload); // vertex lock only
                    ++cursor;
                    sink += payload.size();
                },
                1, 0.0, 0.3);
            kimix_bench::sink(sink);
        }
        for (size_t threads : thread_configs(cores)) {
            auto result = run_parallel(
                threads, kWriteSeconds,
                [&pool, threads](size_t index, clock_type::time_point deadline,
                                 thread_stat &stat) {
                    stat.thread_hash = current_thread_hash();
                    size_t const share = (pool.size() + threads - 1) / threads;
                    size_t const begin = std::min(pool.size(), index * share);
                    size_t const end = std::min(pool.size(), begin + share);
                    if (begin >= end) {
                        return;
                    }
                    auto payload = make_payload(index + 400u, kContentBytes);
                    uint64_t ops = 0;
                    size_t cursor = begin;
                    while (clock_type::now() < deadline) {
                        stamp(payload, ops);
                        pool[cursor]->set_content(payload); // vertex UNIQUE lock only
                        if (++cursor >= end) {
                            cursor = begin;
                        }
                        ++ops;
                    }
                    stat.ops = ops;
                    stat.checksum = ops;
                });
            if (threads >= 2) {
                expect(ge(result.distinct_threads, size_t{2}))
                    << "the vertex-lock writers really ran on distinct OS threads";
            }
            char suffix[256];
            std::snprintf(suffix, sizeof(suffix),
                          "%zu threads, %zu distinct OS thread ids, %llu "
                          "Vertex::set_content on disjoint partitions (vertex lock "
                          "only, NO shard lock)",
                          threads, result.distinct_threads,
                          static_cast<unsigned long long>(result.total_ops));
            kimix::string name =
                kimix::format("graphdb/write/vertex_set_content/{}T", threads);
            print_rate(name.c_str(), static_cast<double>(result.total_ops),
                       result.seconds, suffix);
            kimix_bench::sink(result.total_ops + result.checksum);
        }
    };

    // -----------------------------------------------------------------------
    // 2c. WRITE — mixed: one structural writer + content updaters (disjoint sets)
    // -----------------------------------------------------------------------
    "bench_write_mixed"_test = [cores] {
        Graph graph;
        auto pool = build_random_dag(graph, kMixedPoolVertices, kGraphDegreeMin,
                                     kGraphDegreeMax, 20240607u,
                                     kSerializeContentBytes);
        expect(eq(graph.size(), kMixedPoolVertices)) << "the mixed pool is complete";
        if (pool.size() != kMixedPoolVertices) {
            return;
        }
        size_t const updaters = cores;
        size_t const total_threads = 1 + updaters; // slot 0 is the structural writer

        // Baseline for the mixed number below: the SAME structural loop, alone, in
        // the same 1 s window. mixed_structural / structural_alone_1s is the
        // writer-starvation factor the updaters cause.
        {
            auto payload = make_payload(778u, kSerializeContentBytes);
            size_t creates = 0;
            size_t edges = 0;
            kimix::Guid previous{};
            bool have_previous = false;
            auto const start = clock_type::now();
            auto const deadline =
                start + std::chrono::duration_cast<clock_type::duration>(
                            std::chrono::duration<double>(kMixedSeconds));
            while (clock_type::now() < deadline) {
                Vertex *vertex = graph.create_vertex(payload);
                if (vertex == nullptr) {
                    break;
                }
                ++creates;
                if (have_previous && graph.connect(vertex->guid(), previous)) {
                    ++edges;
                }
                previous = vertex->guid();
                have_previous = true;
            }
            auto const finish = clock_type::now();
            double const seconds = std::chrono::duration<double>(finish - start).count();
            char suffix[256];
            std::snprintf(suffix, sizeof(suffix),
                          "same create+connect loop with NO concurrent updaters, %zu "
                          "creates + %zu edges in the %.2f s window",
                          creates, edges, kMixedSeconds);
            print_rate("graphdb/write/structural_alone_1s",
                       static_cast<double>(creates), seconds, suffix);
            kimix_bench::sink(creates + edges);
        }

        auto result = run_parallel(
            total_threads, kMixedSeconds,
            [&graph, &pool, updaters](size_t index, clock_type::time_point deadline,
                                      thread_stat &stat) {
                stat.thread_hash = current_thread_hash();
                if (index == 0) {
                    // STRUCTURAL WRITER. reset_writer_thread() is the documented
                    // test helper: the pool above was built by the bench thread, so
                    // this thread takes the structural ownership over before its
                    // first structural write. It only ever touches the vertices it
                    // creates — never the updaters' pool.
                    graph.reset_writer_thread();
                    auto payload = make_payload(777u, kSerializeContentBytes);
                    kimix::Guid previous{};
                    bool have_previous = false;
                    uint64_t creates = 0;
                    uint64_t edges = 0;
                    while (clock_type::now() < deadline) {
                        Vertex *vertex = graph.create_vertex(payload);
                        if (vertex == nullptr) {
                            break;
                        }
                        ++creates;
                        if (have_previous && graph.connect(vertex->guid(), previous)) {
                            ++edges;
                        }
                        previous = vertex->guid();
                        have_previous = true;
                    }
                    stat.ops = creates;
                    stat.checksum = edges;
                    return;
                }
                // CONTENT UPDATER over its own slice of the prebuilt pool.
                size_t const worker = index - 1;
                size_t const share = (pool.size() + updaters - 1) / updaters;
                size_t const begin = std::min(pool.size(), worker * share);
                size_t const end = std::min(pool.size(), begin + share);
                if (begin >= end) {
                    return;
                }
                auto payload = make_payload(worker + 900u, kContentBytes);
                uint64_t ops = 0;
                uint64_t ok = 0;
                size_t cursor = begin;
                while (clock_type::now() < deadline) {
                    stamp(payload, ops);
                    Vertex *vertex = pool[cursor];
                    if (graph.update_content(vertex->guid(), payload)) {
                        ++ok;
                    }
                    if (++cursor >= end) {
                        cursor = begin;
                    }
                    ++ops;
                }
                stat.ops = ops;
                stat.checksum = ok;
            });

        if (result.per_thread.empty()) {
            return;
        }
        uint64_t content_ops = 0;
        uint64_t content_ok = 0;
        size_t content_threads = 0;
        for (size_t i = 1; i < result.per_thread.size(); ++i) {
            content_ops += result.per_thread[i].ops;
            content_ok += result.per_thread[i].checksum;
            ++content_threads;
        }
        expect(gt(result.per_thread[0].ops, uint64_t{0}))
            << "the structural writer did work";
        expect(gt(content_ops, uint64_t{0})) << "the content updaters did work";
        if (total_threads >= 2) {
            expect(ge(result.distinct_threads, size_t{2}))
                << "the mixed phase really ran on distinct OS threads";
        }

        {
            uint64_t const structural_ops =
                result.per_thread[0].ops + result.per_thread[0].checksum;
            char suffix[320];
            std::snprintf(suffix, sizeof(suffix),
                          "1 structural writer thread + %zu updaters running "
                          "concurrently, %zu distinct OS thread ids, %llu creates + %llu "
                          "edges; every structural write takes the endpoint shard(s) UNIQUE, "
                          "so this is the writer-starvation number",
                          content_threads, result.distinct_threads,
                          static_cast<unsigned long long>(result.per_thread[0].ops),
                          static_cast<unsigned long long>(result.per_thread[0].checksum));
            print_rate("graphdb/write/mixed_structural",
                       static_cast<double>(structural_ops), result.seconds, suffix);
        }
        {
            char suffix[320];
            std::snprintf(suffix, sizeof(suffix),
                          "%zu updater threads on %zu prebuilt vertices (%llu "
                          "acknowledged), disjoint from the writer's vertex set",
                          content_threads, kMixedPoolVertices,
                          static_cast<unsigned long long>(content_ok));
            print_rate("graphdb/write/mixed_content_update",
                       static_cast<double>(content_ops), result.seconds, suffix);
        }
        kimix_bench::sink(result.total_ops + result.checksum + content_ok);
    };

    // -----------------------------------------------------------------------
    // 3. SERIALIZATION — 100k and the large pass
    // -----------------------------------------------------------------------
    "bench_serialize_100k"_test = [] {
        serialize_numbers numbers = run_serialize_pass(kSerializeVertices, "100k", 20240608u);
        expect(numbers.ok) << "the 100k serialization pass completed";
        if (!numbers.ok) {
            return;
        }
        expect(eq(numbers.full_save_count, kSerializeVertices))
            << "a full save writes every vertex";
        expect(gt(numbers.full_save_bytes, size_t{0})) << "the save wrote bytes";
        expect(eq(numbers.loaded_count, kSerializeVertices)) << "every vertex loads back";
        expect(eq(numbers.loaded_skipped, size_t{0})) << "a clean store skips nothing";
        expect(eq(numbers.dirtied, kSerializeVertices / 100)) << "1% was dirtied";
        expect(eq(numbers.incremental_count, kSerializeVertices / 100))
            << "an incremental save writes exactly the dirty vertices";
        expect(lt(numbers.incremental_bytes, numbers.full_save_bytes))
            << "the incremental save is a fraction of the full one";
        kimix_bench::sink(numbers.incremental_bytes + numbers.full_save_bytes);
    };

    "bench_serialize_large"_test = [] {
        serialize_numbers numbers =
            run_serialize_pass(kSerializeLargeVertices, "large", 20240609u);
        expect(numbers.ok) << "the large serialization pass completed";
        if (!numbers.ok) {
            return;
        }
        expect(eq(numbers.full_save_count, kSerializeLargeVertices));
        expect(eq(numbers.loaded_count, kSerializeLargeVertices));
        expect(eq(numbers.incremental_count, kSerializeLargeVertices / 100))
            << "1% dirty at the large size too";
        kimix_bench::sink(numbers.incremental_bytes + numbers.full_save_bytes);
    };
}