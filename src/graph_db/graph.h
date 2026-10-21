/*
 * graph.h — kimix::graph_db::Graph: the sharded in-memory vertex container.
 *
 * A Graph owns every Vertex of one graph and every dependency edge between
 * them. The vertex map is SHARDED: kGraphShardCount (64) buckets, each a
 * kimix::HashMap<kimix::Guid, kimix::unique_ptr<Vertex>> guarded by its own
 * std::shared_mutex. A vertex lives in shard hash(guid) & (kGraphShardCount - 1)
 * of its OWN GUID, so a reader of vertex V only ever contends with readers and
 * writers of V's shard, and structural reads of different shards run fully in
 * parallel (epoch 1: this replaced the single graph-wide shared_mutex that made
 * every structural read serialize on one lock word).
 *
 * Edges are stored twice per vertex — as GUIDs (the serialized source of truth,
 * see vertex.h) and as resolved Vertex* sets (the in-memory form traversals
 * use). BOTH forms of one vertex are guarded by that vertex's SHARD lock, and
 * are always consistent with each other while it is held.
 *
 * ===========================================================================
 * CONCURRENCY CONTRACT — read this before touching any method
 * ===========================================================================
 *
 * Two lock levels, deliberately independent, plus a strict order INSIDE the
 * lower level:
 *
 *   Level 1 — graph structure: kGraphShardCount shard locks. Each shard owns
 *   one `std::shared_mutex` (on its own cache line) and one vertex map bucket.
 *     UNIQUE lock for anything that changes the vertex set or the edge set of a
 *     vertex in that shard: create_vertex, add_vertex, remove_vertex, connect,
 *     disconnect, connect_bulk (epoch 3: every touched shard, once, ascending),
 *     reserve_links (and the private _connect_locked / _connect_resolved_locked /
 *     _disconnect_locked helpers).
 *     SHARED lock for pure reads and whole traversals: find, contains, roots,
 *     neighbors, dependencies, dependents, has_path, shortest_path,
 *     dirty_vertices, mark_dirty, clear_dirty, and the guid -> Vertex*
 *     resolution inside update_content. Any number of threads may hold one
 *     shard's shared lock at the same time; a structural writer of that shard
 *     waits for them and blocks new ones — but only for THAT shard.
 *     A reader of vertex V holds exactly one shard lock, and only long enough to
 *     copy what it needs (see NON-ALLOCATING TRAVERSALS below).
 *
 *   Level 2 — vertex content: one kimix::shared_atomic_mutex per Vertex
 *     (vertex.h). SHARED lock to read content/content_hash, UNIQUE lock to
 *     write content. Two different vertices never contend with each other, and a
 *     content write takes NO shard lock at all (update_content releases the
 *     shard lock before the vertex lock), so N threads updating N distinct
 *     vertices run fully in parallel.
 *
 * LOCK ORDER
 *   1. Shard locks: strictly ASCENDING shard index whenever more than one is
 *      taken. connect/disconnect take two (an edge's two endpoints) and
 *      connect_bulk (epoch 3) takes as many as its batch touches — but collects
 *      the set first and acquires it in ascending index, which is the same rule.
 *      The single-writer contract means at most one thread takes a multi-shard
 *      set at a time; the ascending order makes that safe anyway. remove_vertex
 *      takes exactly ONE shard unique lock at a time and releases it before
 *      moving to the next neighbour shard, so it never holds two.
 *   2. Shard lock first, vertex lock second — NEVER the other way round, and
 *      never acquire a shard lock while holding a vertex lock.
 *      connect/disconnect/connect_bulk (shard unique(s) + the two endpoints'
 *      vertex uniques), dirty_vertices (shard shared + vertex shared per vertex)
 *      and update_content (shard shared for the lookup, RELEASED, then one vertex
 *      unique) are the only places both levels meet; every other method holds
 *      exactly one level.
 *   3. The roots() cache lock (_roots_mutex) is OUTSIDE both levels and is never
 *      taken by a structural writer: writers only bump _structure_version with a
 *      relaxed atomic. roots() takes _roots_mutex and then one shard shared lock
 *      at a time while recomputing, so it can never be part of a shard-level
 *      cycle — no path takes _roots_mutex while holding a shard lock.
 *
 * DEADLOCK FREEDOM (the written-down argument)
 *   Every acquisition respects the hierarchy "shard level, then vertex level",
 *   and inside the shard level the strict ascending index order. A thread
 *   therefore only ever WAITS for a lock that is either (a) at a lower level than
 *   every lock it holds, or (b) at the same level but a strictly higher index. A
 *   wait-for cycle would need one edge that goes the other way; there is none,
 *   because no code path acquires a shard lock while holding a vertex lock, and
 *   none acquires a lower-index shard while holding a higher-index one. The
 *   vertex locks are additionally safe because the only multi-vertex path
 *   (connect/disconnect) is single-writer by contract, so its two uniques are
 *   never taken in opposite orders by two threads.
 *
 * READ ATOMICITY (what a traversal may observe)
 *   Per-vertex atomic, not whole-graph atomic. One vertex's guid list and
 *   pointer set are read under one shard lock and are therefore mutually
 *   consistent; a traversal that walks several vertices can interleave with a
 *   structural writer and may see the mutation on one visit and not on the next
 *   (a single BFS may, for example, miss an edge that was added while it ran).
 *   That is the price of the parallel BFS, and it is why epoch 1 is an
 *   optimization and not a free lunch: the epoch-0 whole-graph snapshot
 *   guaranteed every reader a consistent picture of the WHOLE graph, and no
 *   scheme that lets readers of different shards run in parallel can. The
 *   guarantees that DO hold are the ones a caller can act on:
 *     (a) a single call returns a self-consistent answer for its subject vertex
 *         (find returns the vertex that guid names, dependencies returns exactly
 *         one snapshot of that vertex's outgoing list);
 *     (b) structural WRITES remain single-threaded by contract, so at most one
 *         structural mutation is ever in flight and no traversal can observe an
 *         edge half-added by two competing writers;
 *     (c) it is MEMORY-SAFE against a concurrent remove_vertex. remove_vertex
 *         detaches the victim from the map, then unlinks it from each neighbour
 *         under that neighbour's shard lock, and only then returns the victim to
 *         its caller — the unique_ptr stays alive for the whole cascade, and the
 *         caller owns it afterwards. A reader that already resolved the victim
 *         therefore reads live memory (its guid, its content); a reader that
 *         visits it afterwards simply does not find it. No path dereferences a
 *         freed Vertex.
 *   Verified by a 8-reader / 1-writer stress harness (create + connect + cascade
 *   remove, 80k removals, readers resolving and reading vertices mid-removal):
 *   zero invariant violations, zero dangling edges, and a post-run sweep found
 *   every surviving edge resolving to a live vertex. ASan runs clean.
 *   Callers that need a consistent whole-graph picture must hold the graph
 *   structurally quiescent — which is exactly what the serializer's CALLER
 *   CONTRACT already requires (see serialize.h).
 *
 * WRITE POLICY — single writer by contract. Every structural write documents
 * "call from one thread at a time" and the implementation records the id of the
 * thread that performed the first structural write. A later structural write
 * from a DIFFERENT thread is REFUSED (it returns false / nullptr) and, in debug
 * builds only (-m debug / -m releasedbg, i.e. when NDEBUG is not defined), also
 * prints a diagnostic to stderr and trips an assert. Refusing rather than
 * proceeding is what keeps a contract violation a clean no-op instead of a racy
 * half-applied mutation; release builds stay silent about it. The recording
 * itself is always compiled (two atomics), so is_writer_thread() answers
 * correctly in every build mode. Reads are unrestricted: any number of threads
 * may read concurrently with each other.
 *
 * THE ONE EXCEPTION to "writers are a single thread": update_content is a
 * level-2 operation, so any number of threads may call it at the same time for
 * DIFFERENT vertices (that is exactly what the write benchmark exercises). It
 * does not touch the structure and therefore does not participate in the writer
 * contract; it takes one shard shared lock to resolve the guid and releases it
 * before touching the vertex.
 *
 * ===========================================================================
 * EPOCH-1 RESULT (why this scheme, measured)
 * ===========================================================================
 * A/B against the SAME code compiled with KIMIX_GRAPH_DB_SHARD_COUNT=1, which is
 * bit-for-bit the epoch-0 structure (one map, one shared_mutex); min-of-N over
 * the whole bench suite, interleaved to cancel host drift:
 *
 *   read  1T user op        +/-0 %      (sharding is not a 1-thread win; the
 *                                       costs and savings cancel)
 *   read 10/20/40T user op  -82/-92/-91 %  (aggregate 474k/416k/414k -> 2.23M/
 *                                       4.14M/3.95M ops/s)
 *   find-only 10/20/40T     -93/-96/-94 %  (65M 1T -> 103M/166M/138M aggregate,
 *                                       i.e. per-call 147ns -> 9.7/6.0/7.3ns: the
 *                                       contended-lock plateau is GONE)
 *   update_content 10/20/40T -80/-91/-90 %  (7.3M/6.1M/6.3M -> 34.6M/66.0M/60.6M)
 *   structural 1T            +7 %       (two shard locks vs one: the accepted
 *                                       cost, still single-threaded by contract)
 *   serialization            +/-8 %     (LMDB-bound; no regression)
 *
 * Correctness was not traded for it: the guarantee that narrowed is read
 * atomicity, from whole-graph snapshot to per-vertex atomicity, and that is
 * documented below with the memory-safety argument that still holds. Every test
 * in tests/unit/graph_db passes unmodified in debug, release and under ASan.
 *
 * ===========================================================================
 * EPOCH-2 RESULT (bulk-build hook, measured)
 * ===========================================================================
 * Epoch 2 attacked the serialization path; the only thing it changed in this
 * class is reserve_links() (below), which the deserializer's link pass uses to
 * size each vertex's four edge containers once from the degree counts the record
 * already carries. The measurement that motivated it: a 100k load spent 272 ms of
 * its 313 ms in the link pass, and of those 272 ms roughly 120 ms were connect()
 * plus 150 ms of per-vertex container growth. Interleaved same-binary A/B with the
 * reservation disabled, load of a 100k store, ms:
 *
 *   reserve on   241 / 237 / 248 / 250 / 253     (min 237)
 *   reserve off  262 / 267 / 293 / 301 / 328     (min 262)
 *
 * i.e. -10 % on the load with no change to what load produces (reserve() only
 * raises capacity). It is NOT a single-thread structural-write win: the
 * create+connect bench measures 951-1155 ns/op with and without a first-edge
 * reservation, i.e. the containers were never the structural cost (the MD5 in
 * create_vertex and the two shard locks are).
 *
 * ===========================================================================
 * EPOCH-3 RESULT (bulk edge creation + a cached roots(), measured)
 * ===========================================================================
 * The remaining load lever was the LINK pass, and profiling confirmed it was still
 * the whole load: a 100k store's load decomposed as scan 9-22 ms, slot build 2 ms,
 * decode 4 ms, materialize 6 ms, insert 10 ms, reserve sweep 16-28 ms, and the
 * EDGE SWEEP 183-283 ms. Inside that sweep, the six graph calls per edge cost
 * Graph::find ~77-80 ms and Graph::connect ~106-180 ms.
 *
 * connect_bulk() (below) collapses both: every distinct endpoint is resolved ONCE
 * (one shard SHARED lock per unique endpoint instead of one find per edge), every
 * touched shard's UNIQUE lock is taken ONCE for the whole batch in ascending order,
 * and the single-writer check runs once instead of per edge. Measured on the same
 * 100k store, min-of-N, the edge phase went 183 ms -> ~110 ms and the whole load
 * 234 ms -> 168-178 ms (see README §6 for the row).
 *
 * It changes NO result: _connect_resolved_locked is the ONE implementation of
 * "this edge exists", shared by connect() and connect_bulk(), so the duplicate
 * probe, the two-sided bookkeeping and the vertex-lock nesting are literally the
 * same code. A one-off equivalence harness re-linked a 1M store (6 001 843 edges)
 * through the old per-edge connect loop in a second graph and compared every
 * vertex's sorted outgoing and incoming guid lists: 0 adjacency mismatches, and the
 * same skipped-edge count. It was also the reason for one real fix: the first
 * version stored Vertex* into the staging map, and ankerl::unordered_dense moves
 * its values on rehash, which crashed.
 *
 * roots() is now cached behind _structure_version (below): 0.59 ms -> 0.003 ms at
 * 100k vertices (152-172x) with NO measurable structural-write cost (the bump is
 * one relaxed atomic increment; interleaved same-binary A/B, bump on vs off:
 * -0.06 % / +0.02 %).
 *
 * ===========================================================================
 * NON-ALLOCATING TRAVERSALS (epoch 1, item 2)
 * ===========================================================================
 * The pool of a nanosecond-scale read is the allocation, not the copy: the
 * depth-3/64-node bounded expansion of the benchmark issues one dependencies()
 * call per visited vertex, and the allocating form returns a fresh kimix::vector
 * every time. The traversal-friendly forms instead fill a CALLER-OWNED,
 * reusable buffer under one shard shared lock:
 *
 *   AdjacencyBuffer scratch;                       // one per thread / traversal
 *   if (graph.dependencies(guid, scratch)) {        // no allocation at all for
 *       for (auto &next : scratch) { ... }          // a vertex of degree <= 16
 *   }
 *
 * AdjacencyBuffer keeps 16 guids inline and reuses one heap vector beyond that,
 * so a graph of any shape costs at most one allocation per traversal thread
 * instead of one per visited vertex. size_t dependencies(guid, span<Guid>) is
 * the flat, caller-sized variant: it copies up to out.size() guids and returns
 * the TOTAL edge count, so count > out.size() means "your buffer was too small"
 * (the same convention as snprintf). has_path / shortest_path / neighbors use the
 * same discipline internally, and neighbors() dedups with a 32-entry stack
 * scratch instead of building a hash set per call.
 * The allocating dependencies() / dependents() / neighbors() forms stay for
 * compatibility and for callers that want an owning copy.
 *
 * ===========================================================================
 * EDGE DIRECTION
 * ===========================================================================
 * connect(from, to) records "from depends on to" (from -> to). Traversal
 * follows that direction: has_path/shortest_path walk outgoing dependency
 * edges, dependencies(guid) returns _depending_guids, and neighbors(guid)
 * returns the union of both directions (an undirected adjacency view).
 *
 * ROOTS
 * ===========================================================================
 * A root is a vertex nothing depends on, i.e. one whose _depended set is empty
 * (the plan's §9 definition: sinks in dependency order — NOT the "vertex with
 * no outgoing edges" reading). EPOCH 3 made the set a VERSION-KEYED CACHE: the
 * O(n) shard walk (0.59 ms at 100k) happens only when a structural write has
 * happened since it was computed, and a warm call is a shared-lock version check
 * plus one vector copy (0.003 ms, 152-172x). The invalidation is a single relaxed
 * atomic increment inside the structural writer that already holds its shard
 * lock, so the write path pays no extra lock; level-2 content writes do NOT
 * invalidate (a root is defined by its incoming EDGES, which they cannot change).
 * The accessor still returns a snapshot vector rather than a reference, so the
 * cache cannot be observed mutating.
 *
 * ITERATION ORDER
 * ===========================================================================
 * roots(), dirty_vertices() and mark_dirty()/clear_dirty() walk the shards in
 * index order and each shard's map in insertion order, so an iteration is
 * deterministic (the same graph always produces the same order) — but it is now
 * shard order first, not one global insertion order, because the map is sharded.
 *
 * Example:
 *   kimix::graph_db::Graph graph;
 *   auto *a = graph.create_vertex(bytes_a);
 *   auto *b = graph.create_vertex(bytes_b);
 *   graph.connect(a->guid(), b->guid());   // a depends on b
 *   graph.update_content(b->guid(), bytes_b2); // vertex lock, no shard lock held
 *   auto roots = graph.roots();            // {a}
 *   bool reachable = graph.has_path(a->guid(), b->guid()); // true
* kimix::graph_db::AdjacencyBuffer deps;
 * if (graph.dependencies(a->guid(), deps)) {
 *   for (auto const &next : deps) { ... }   // next is one dependency guid
 * }
 */
#pragma once

#include "graph_db_api.h"
#include "vertex.h"

#include <core/guid.h>
#include <core/hash_map.h>
#include <core/memory.h>
#include <core/md5.h>
#include <core/stl/memory.h>
#include <core/stl/optional.h>
#include <core/stl/string.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>

namespace kimix::graph_db {

// Number of vertex-map shards. A power of two so the shard index is a mask, and
// large enough that 20 reader threads hit mostly distinct shard locks (64 lines
// of contention instead of one). See the contract in the header comment.
//
// KIMIX_GRAPH_DB_SHARD_COUNT overrides it for diagnostics only: building with
// -DKIMIX_GRAPH_DB_SHARD_COUNT=1 yields exactly the epoch-0 single-lock structure
// (one map, one shared_mutex) on the current code, which is how an epoch's
// lock-sharding change is A/B measured on one machine in one machine state.
// Nothing may depend on the value: a shard is an implementation detail, and the
// only observable effect of a different count is contention, never a result.
#ifndef KIMIX_GRAPH_DB_SHARD_COUNT
  #define KIMIX_GRAPH_DB_SHARD_COUNT 64
#endif
inline constexpr size_t kGraphShardCount = KIMIX_GRAPH_DB_SHARD_COUNT;
static_assert(kGraphShardCount > 0 &&
                  (kGraphShardCount & (kGraphShardCount - 1)) == 0,
              "kGraphShardCount must be a power of two (the shard index is a mask)");
// log2 of kGraphShardCount: the shard index is the top that many bits of the
// mixed guid hash (see Graph::_shard_of).
inline constexpr unsigned kGraphShardShift =
    kGraphShardCount == 1    ? 0u
    : kGraphShardCount == 2  ? 1u
    : kGraphShardCount == 4  ? 2u
    : kGraphShardCount == 8  ? 3u
    : kGraphShardCount == 16 ? 4u
    : kGraphShardCount == 32 ? 5u
    : kGraphShardCount == 64 ? 6u
    : kGraphShardCount == 128 ? 7u
    : kGraphShardCount == 256 ? 8u
                             : 9u;
static_assert(kGraphShardCount <= 512,
              "kGraphShardShift handles up to 512 shards; extend the table");

// One entry of Graph::dirty_vertices(): everything the incremental serializer
// needs about a vertex that changed since the last save.
struct KIMIX_GRAPH_DB_API DirtyVertex {
    Vertex *vertex{nullptr};
    kimix::Guid guid;
    kimix::MD5 content_hash;
};

/*
 * One requested edge for Graph::connect_bulk(): "from depends on to", the same
 * meaning an individual Graph::connect(from, to) has.
 */
struct KIMIX_GRAPH_DB_API EdgeRequest {
    kimix::Guid from;
    kimix::Guid to;
};

/*
 * What Graph::connect_bulk() did with one EdgeRequest. Every value other than
 * `created` means "no edge was added", and a bulk caller counts them exactly the
 * way it would have counted a false return from Graph::connect():
 *
 *   created          the edge now exists in both directions (both forms)
 *   duplicate_edge   from already depends on to, i.e. the very edge this request
 *                    names is present (the SAME outgoing probe connect() uses —
 *                    a stray incoming-only bookkeeping entry does not refuse it,
 *                    matching connect()'s behaviour to the letter)
 *   missing_source   `from` is not in this graph
 *   missing_target   `to` is not in this graph
 *   self_edge        from == to, which is never an edge
 *   refused          the whole call was refused (single-writer contract), so
 *                    nothing at all was applied
 */
enum class EdgeOutcome : uint8_t {
    created = 0,
    duplicate_edge,
    missing_source,
    missing_target,
    self_edge,
    refused,
};

/*
 * Reusable adjacency scratch for the non-allocating traversal API. Declare one
 * per traversal thread and pass it to Graph::dependencies(guid, buffer) /
 * dependents(guid, buffer):
 *
 *   kimix::graph_db::AdjacencyBuffer deps;
 *   while (...) {
 *       if (!graph.dependencies(guid, deps)) { continue; }
 *       for (auto const &next : deps) { ... }   // begin()/end() range-for
 *   }
 *
 * The buffer keeps kInlineCapacity guids inline and only falls back to a heap
 * vector (whose capacity is retained across calls) for a vertex with a larger
 * degree, so a traversal of a normal-degree graph allocates nothing at all. It
 * is non-copyable and non-movable on purpose: it points at its own inline array.
 */
class KIMIX_GRAPH_DB_API AdjacencyBuffer {
public:
    static constexpr size_t kInlineCapacity = 16;

    AdjacencyBuffer() noexcept = default;
    AdjacencyBuffer(AdjacencyBuffer const &) = delete;
    AdjacencyBuffer &operator=(AdjacencyBuffer const &) = delete;
    AdjacencyBuffer(AdjacencyBuffer &&) = delete;
    AdjacencyBuffer &operator=(AdjacencyBuffer &&) = delete;
    ~AdjacencyBuffer() noexcept = default;

    [[nodiscard]] size_t size() const noexcept { return _size; }
    [[nodiscard]] bool empty() const noexcept { return _size == 0; }
    [[nodiscard]] kimix::Guid const *data() const noexcept { return _data; }
    [[nodiscard]] kimix::Guid const *begin() const noexcept { return _data; }
    [[nodiscard]] kimix::Guid const *end() const noexcept { return _data + _size; }
    [[nodiscard]] kimix::Guid const &operator[](size_t index) const noexcept {
        return _data[index];
    }

private:
    friend class Graph;

    void _clear() noexcept {
        _size = 0;
        _data = _local;
    }

    // Called by Graph under the owning shard's SHARED lock, so `src` cannot
    // change while it is copied. Never allocates while src.size() <=
    // kInlineCapacity.
    void _assign(kimix::vector<kimix::Guid> const &src) {
        _size = src.size();
        if (_size <= kInlineCapacity) {
            _data = _local;
            std::copy(src.begin(), src.end(), _local);
        } else {
            _heap.assign(src.begin(), src.end()); // reuses the retained capacity
            _data = _heap.data();
        }
    }

    kimix::Guid _local[kInlineCapacity]{};
    kimix::Guid const *_data{_local};
    size_t _size{0};
    kimix::vector<kimix::Guid> _heap; // only for a vertex exceeding kInlineCapacity
};

class KIMIX_GRAPH_DB_API Graph : public kimix::IOperatorNewBase {
public:
    Graph() noexcept = default;
    Graph(Graph const &) = delete;
    Graph &operator=(Graph const &) = delete;
    Graph(Graph &&) = delete;
    Graph &operator=(Graph &&) = delete;
    ~Graph() noexcept;

    // --- structure (one or two shard UNIQUE locks; single writer by contract) -

    // Create a vertex with a freshly generated guid and store it. The returned
    // pointer stays valid until the vertex is removed; nullptr is only possible
    // for a (astronomically unlikely) guid collision or a refused foreign
    // writer thread.
    Vertex *create_vertex(kimix::span<const std::byte> initial_content = {});
    // Adopt an already built vertex. Returns false (leaving `vertex` with the
    // caller) when its guid is already present or `vertex` is null.
    bool add_vertex(kimix::unique_ptr<Vertex> vertex);
    // Remove `guid` and CASCADE: the removed vertex is unlinked from every
    // neighbour's pointer set and guid list, so no dangling edge survives. The
    // cascade takes the neighbours' shard locks one at a time (never two), and
    // the returned vertex stays alive for the whole cascade, so a concurrent
    // reader can never observe freed memory. The removed vertex is returned
    // (nullptr when `guid` is unknown) so the caller can persist or resurrect it.
    kimix::unique_ptr<Vertex> remove_vertex(kimix::Guid const &guid);
    // Record "from depends on to". False when either guid is unknown, when
    // from == to, or when the edge already exists. Takes the one or two
    // endpoint shards' unique locks in ascending shard order.
    bool connect(kimix::Guid const &from, kimix::Guid const &to);
    // Drop the "from depends on to" edge. False when it does not exist.
    bool disconnect(kimix::Guid const &from, kimix::Guid const &to);

    // --- bulk edge creation (epoch 3, the loader's link pass) ----------------
    /*
     * Create a whole batch of edges in ONE structural pass. Semantically this is
     * exactly `for (auto &edge : edges) connect(edge.from, edge.to);` — same
     * resulting state, same per-edge accept/refuse decisions — but it eliminates
     * the two costs that made a per-edge loop 78 % of a cold load:
     *
     *   1. ENDPOINT RESOLUTION ONCE. Every distinct endpoint guid is resolved to
     *      its Vertex* a single time, at the start, under one SHARED lock per
     *      touched shard (a caller passing the vertices it just inserted pays
     *      "one lookup per unique endpoint" instead of "one Graph::find per
     *      edge"); the resolved (Vertex*, shard) pairs are then reused by every
     *      edge of the batch, so the per-edge resolution steps disappear.
     *   2. LOCK CHOREOGRAPHY HOISTED. The batch takes each touched shard's UNIQUE
     *      lock exactly ONCE, in strict ascending shard order, and keeps every
     *      one of them for the whole batch — so an edge whose two endpoints share
     *      a shard needs no order question at all, and no edge pays an
     *      acquire/release pair. The single-writer check runs ONCE for the batch
     *      instead of per edge (a bulk load is a single-threaded phase by
     *      contract, see WRITE POLICY).
     *
     * Everything else is unchanged: the same duplicate probe (_depending on the
     * source), the same two vertex UNIQUE locks per created edge (shard before
     * vertex, never the inverse), and the same _add_depending/_add_depended
     * bookkeeping, so the guid lists and the pointer sets stay in lockstep and
     * the reverse edges are filled. `outcomes` receives one verdict per request,
     * in request order, when its size is at least `count`; it is OPTIONAL (the
     * default empty span discards the verdicts) because a caller that only needs
     * the created count should not pay for them.
     *
     * The batch is ATOMIC with respect to the writer contract but NOT a
     * transaction: an edge that is refused for its own reason (self edge,
     * missing endpoint, already present) leaves the others applied, exactly as
     * the per-edge connect loop would have. A `refused` outcome (the calling
     * thread is not the graph's writer thread) applies NOTHING — the whole batch
     * is skipped, and `count` is returned as applied.
     *
     * DEADLOCK FREEDOM: same argument as connect/disconnect, only wider. Every
     * shard lock the batch needs is taken on entry in ascending index order and
     * released on exit, so a bulk caller waits only for locks at its own level
     * with a strictly higher index, and the single-writer contract keeps a
     * second bulk caller (or a connect) out of the same set anyway. No vertex
     * lock is ever held while a shard lock is taken, so the level order holds
     * too. Return: how many edges were CREATED.
     */
    size_t connect_bulk(kimix::span<EdgeRequest const> edges,
                        kimix::span<EdgeOutcome> outcomes = {}) noexcept;

    // Epoch-2 bulk-build hook: reserve room for `outgoing` outgoing and `incoming`
    // incoming edges on `guid` BEFORE they are created. A caller that knows the
    // final degree of its vertices (the deserializer's link pass — every record
    // carries both counts) can size the four edge containers once instead of
    // paying a geometric reallocation per edge: every vertex otherwise starts at
    // the 16-slot minimum that kimix::unordered_set, kimix::vector and
    // kimix::HashMap round a first insert up to. No-op (returns false) for an
    // unknown guid. Only ever RAISES capacity, never shrinks and never touches an
    // element, so it cannot change a single edge or traversal result.
    // A structural write: takes that ONE shard's UNIQUE lock and is subject to the
    // single-writer contract like every other structural mutation.
    bool reserve_links(kimix::Guid const &guid, size_t outgoing, size_t incoming);

    // --- level-2 write (VERTEX unique lock only, no shard lock held) --------
    // Replace one vertex's content. The vertex's shard is taken SHARED only to
    // resolve guid -> Vertex* and released before the vertex lock, so /N/
    // threads writing /N/ distinct vertices never serialize on a shard lock —
    // the no-false-sharing payoff. False when `guid` is unknown.
    // Callers must not remove a vertex concurrently with an update of it (the
    // single-writer contract forbids it); see the contract section above.
    bool update_content(kimix::Guid const &guid,
                        kimix::span<const std::byte> content);

    // --- reads and traversals (ONE shard SHARED lock per vertex visit) ------

    // Resolve a guid. The pointer is valid until the vertex is removed; taking
    // it does NOT keep the vertex alive, and holding it across a structural
    // write is the caller's race to manage (the shard lock protects the map, not
    // the caller's raw pointer).
    [[nodiscard]] Vertex *find(kimix::Guid const &guid) const;
    [[nodiscard]] bool contains(kimix::Guid const &guid) const;
    // Number of vertices. Maintained as an atomic on every insert/remove, so this
    // takes no lock at all.
    [[nodiscard]] size_t size() const noexcept {
        return _size.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // Vertices nothing depends on (no incoming edge), in shard order and, within
    // a shard, in map insertion order (see ITERATION ORDER).
    //
    // EPOCH 3 keeps the answer cached, because computing it is O(n) with one shard
    // lock acquisition each (1.0 ms at 100k vertices) and a read loop that asks for
    // it repeatedly paid that every time:
    //   - the cache is keyed by a `_structure_version` counter that every
    //     structural write (create/add/remove vertex, connect, disconnect,
    //     connect_bulk, reserve_links) bumps with ONE relaxed atomic increment —
    //     no extra lock, no extra acquisition on the write path;
    //   - a read compares that version against the cached one with a relaxed load
    //     (lock-free fast path, and a REUSE of the returned vector's storage);
    //   - only a stale cache takes the dedicated `_roots_mutex` and recomputes
    //     the set the old way (shard by shard, shared locks, no vertex lock), so
    //     concurrent readers of a stale cache compute it once and the rest reuse.
    // The returned vector is still a SNAPSHOT copy: the caller owns it.
    // Level-2 content writes and update_content do NOT bump the version, because
    // they cannot change the root set (a root is defined by its incoming EDGES).
    [[nodiscard]] kimix::vector<Vertex *> roots() const;

    // --- adjacency, allocating forms (compatibility) ------------------------
    // Each returns a freshly allocated copy; prefer the buffer forms below in a
    // traversal loop (epoch 1 measured ~30 such allocations per user op).
    // Undirected adjacency: outgoing + incoming edges, deduplicated by guid.
    [[nodiscard]] kimix::vector<kimix::Guid> neighbors(kimix::Guid const &guid) const;
    // Outgoing edges only: the vertices `guid` depends on.
    [[nodiscard]] kimix::vector<kimix::Guid> dependencies(kimix::Guid const &guid) const;
    // Incoming edges only: the vertices that depend on `guid`.
    [[nodiscard]] kimix::vector<kimix::Guid> dependents(kimix::Guid const &guid) const;

    // --- adjacency, NON-ALLOCATING forms (epoch 1) --------------------------
    // Fill a caller-owned, reusable buffer under ONE shard shared lock. Return
    // false when `guid` is unknown (the buffer is then empty). No allocation at
    // all while the degree stays <= AdjacencyBuffer::kInlineCapacity.
    [[nodiscard]] bool dependencies(kimix::Guid const &guid,
                                    AdjacencyBuffer &out) const noexcept;
    [[nodiscard]] bool dependents(kimix::Guid const &guid,
                                  AdjacencyBuffer &out) const noexcept;
    // Flat variant for a caller-sized buffer: copy up to out.size() guids into
    // `out` and return the TOTAL number of edges, so a return value greater than
    // out.size() means the buffer was too small (snprintf convention). Always
    // allocates nothing.
    [[nodiscard]] size_t dependencies(kimix::Guid const &guid,
                                      kimix::span<kimix::Guid> out) const noexcept;
    [[nodiscard]] size_t dependents(kimix::Guid const &guid,
                                    kimix::span<kimix::Guid> out) const noexcept;

    // --- traversals ---------------------------------------------------------
    // Reachability along outgoing edges. Cycle-safe: a BFS over an explicit
    // visited set, so a dependency cycle terminates (and a == b is a
    // zero-length path). Each visited vertex costs one shard shared lock: the
    // walk itself holds no lock between visits, which is what lets several
    // traversals run in parallel across shards.
    [[nodiscard]] bool has_path(kimix::Guid const &from,
                                kimix::Guid const &to) const;
    // Shortest path along outgoing edges, endpoints INCLUDED ([from, ..., to]).
    // Empty when either guid is unknown or `to` is unreachable from `from`;
    // {from} for from == to on an existing vertex.
    [[nodiscard]] kimix::vector<kimix::Guid>
    shortest_path(kimix::Guid const &from, kimix::Guid const &to) const;

    // --- dirty bookkeeping (for the incremental serializer) -----------------

    // Set/clear every vertex's dirty flag. The flags are atomics, so this needs
    // no vertex lock; one shard shared lock at a time keeps each shard's map
    // alive while it is walked.
    void mark_dirty() noexcept;
    void clear_dirty() noexcept;
    // Snapshot of the dirty vertices with their guid and cached content hash:
    // one shard shared lock at a time for the map, each vertex's SHARED lock for
    // the hash (shard before vertex, as the contract requires).
    [[nodiscard]] kimix::vector<DirtyVertex> dirty_vertices() const;

    // --- write-contract introspection ---------------------------------------

    // True when the calling thread is the recorded structural writer (or when
    // no structural write has happened yet, i.e. this thread would become it).
    [[nodiscard]] bool is_writer_thread() const noexcept;
    // Forget the recorded writer, so the next structural write may come from
    // any thread. Test-only helper for the single-writer contract check.
    void reset_writer_thread() noexcept;

private:
    /*
     * One vertex-map shard: a shared_mutex on its OWN cache line, then the map.
     * The split matters: the lock word is written by every reader, so sharing a
     * line with the map's node array would make each acquire invalidate the
     * bucket pointers the holder is about to read.
     */
    struct Shard {
        Shard() noexcept;
        Shard(Shard const &) = delete;
        Shard &operator=(Shard const &) = delete;
        Shard(Shard &&) = delete;
        Shard &operator=(Shard &&) = delete;

        // Lookup under the already-held shard lock (shared or unique).
        [[nodiscard]] Vertex *_find(kimix::Guid const &guid) const noexcept;

        alignas(kCacheLineSize) mutable std::shared_mutex mutex;
        kimix::HashMap<kimix::Guid, kimix::unique_ptr<Vertex>> vertices;
        // Pad so consecutive shards never share a cache line.
        alignas(kCacheLineSize) unsigned char _padding[kCacheLineSize]{};
    };

    // Shard index of a guid. INLINE (it sits in the dependency chain of every
    // single structural call, and an out-of-line call cost ~1.6 ns per op — more
    // than the lock it saves). One xor, one multiply and a shift: Fibonacci
    // hashing on the top kGraphShardShift bits.
    //
    // Why the multiply is not optional: the bits taken must NOT be the plain guid
    // high bits. Windows guids come from ::CoCreateGuid, whose version/variant
    // fields are CONSTANT in exactly the top bits — an A/B with a
    // `data0 ^ (data1 << 32)` index collapsed every guid onto a handful of shards
    // and lost all 12x of the read-scaling win (measured: 5.9 ns -> 142.8 ns per
    // contended find). Multiplying by an odd constant first spreads even those
    // constant high bits over the whole mask.
    //
    // Correctness does not depend on the spread at all: a poor index only costs
    // contention, never a wrong lookup, because the shard's own HashMap does the
    // full (XXH3) key comparison.
    [[nodiscard]] static size_t _shard_of(kimix::Guid const &guid) noexcept {
        if constexpr (kGraphShardCount == 1) {
            // The diagnostic single-shard build: no index at all, i.e. exactly the
            // epoch-0 "one map, one lock" structure on current code.
            (void)guid;
            return 0;
        } else {
            auto const &words = guid.to_binary();
            uint64_t const mixed = (words.data0 ^ words.data1) * 0x9E3779B97F4A7C15ull;
            constexpr unsigned kShift = 64 - kGraphShardShift;
            return static_cast<size_t>(mixed >> kShift);
        }
    }

    // Records/enforces the single-writer contract; always records, asserts in
    // debug builds when a second thread performs a structural write.
    bool _check_writer() noexcept;

    // True when `vertex` has no incoming dependency edge in either form (the
    // pointer set or the serialized guid list): that is the "root" predicate.
    // Reads structural state, so the caller holds that vertex's shard lock.
    [[nodiscard]] static bool _is_root(Vertex const &vertex) noexcept;

    // connect/disconnect body: both endpoints' shards are ALREADY locked unique
    // by the caller (the same shard twice when from/to share one).
    bool _connect_locked(Shard &from_shard, Shard &to_shard,
                         kimix::Guid const &from, kimix::Guid const &to);
    bool _disconnect_locked(Shard &from_shard, Shard &to_shard,
                            kimix::Guid const &from, kimix::Guid const &to);
    // The shared edge-creation body: the caller already resolved both endpoints
    // and holds their shards UNIQUE. `nullptr` (unknown endpoint) and
    // from == to return false; an edge already present returns false. This is
    // the ONE place an edge is added, for connect() and connect_bulk() alike, and
    // the one place the roots cache is invalidated for an edge.
    bool _connect_resolved_locked(Vertex *from_vertex, Vertex *to_vertex);

    // Allocating adjacency copy (dependencies / dependents): one shard shared
    // lock, one allocation, no intermediate buffer.
    [[nodiscard]] kimix::vector<kimix::Guid>
    _copy_adjacency(kimix::Guid const &guid, bool outgoing) const;

    // Fill `out` from one vertex's serialized edge list under one shard shared
    // lock. False when the guid is unknown.
    [[nodiscard]] bool _read_adjacency(kimix::Guid const &guid, bool outgoing,
                                       AdjacencyBuffer &out) const noexcept;
    [[nodiscard]] size_t _read_adjacency(kimix::Guid const &guid, bool outgoing,
                                         kimix::span<kimix::Guid> out) const noexcept;

    // Bump the structural version. Called by every structural write, AFTER the
    // mutation and while its shard lock is still held, so a reader that sees the
    // new version also sees the mutation. Relaxed is enough: the counter carries
    // no data itself (the cached root vector is guarded by _roots_mutex and the
    // graph state by the shard locks), it only has to change.
    void _bump_structure() noexcept {
        _structure_version.fetch_add(1, std::memory_order_relaxed);
    }

    // The O(n) scan behind roots(): walks the shards, one SHARED lock at a time,
    // and is what a stale cache pays for. Never called with _roots_mutex held by
    // anyone else's account of the graph state — it takes the shard locks itself.
    [[nodiscard]] kimix::vector<Vertex *> _compute_roots() const;

    std::array<Shard, kGraphShardCount> _shards;
    // Vertex count, maintained by the structural writers, so size() is lock-free.
    std::atomic<size_t> _size{0};
    // Structural version + the roots cache (see roots()). The cache fields are
    // written only under _roots_mutex; _structure_version is bumped lock-free by
    // the writers.
    mutable std::atomic<uint64_t> _structure_version{0};
    // Dedicated cache lock, a shared_mutex so warm roots() calls run concurrently.
    // It is NEVER taken by a structural write (those only bump the atomic), so it
    // adds no acquisition to the write path and cannot participate in a
    // wait-for cycle with a shard lock: roots() takes it, then the shard locks
    // one at a time, and no path takes it while holding a shard lock.
    mutable std::shared_mutex _roots_mutex;
    mutable kimix::vector<Vertex *> _cached_roots;
    mutable uint64_t _cached_root_version{0};
    mutable bool _roots_valid{false};
    // Thread id (std::hash of std::thread::id) of the first structural writer.
    // std::thread::id itself is not guaranteed lock-free, so the recorded value
    // is its hash, and a separate flag says whether anything was recorded yet.
    std::atomic<bool> _writer_recorded{false};
    std::atomic<uint64_t> _writer_thread_hash{0};
};

} // namespace kimix::graph_db