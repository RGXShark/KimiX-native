/*
 * graph.cpp — sharded, lock-acquiring implementations of the Graph container.
 *
 * Every public method acquires the shard lock(s) it needs EXACTLY once per
 * vertex it visits and then calls the private _locked/_find helpers: a
 * std::shared_mutex is not recursive (the Windows SRW lock behind it does not
 * support re-acquisition from the same thread), so re-acquiring the lock of a
 * shard that is already held would deadlock. The helpers below are therefore
 * private and never lock.
 *
 * Lock ordering (see the contract in graph.h):
 *   1. shard locks in strict ASCENDING index order when more than one is taken;
 *   2. shard lock first, vertex lock second, never the inverse.
 * connect/disconnect take at most two shard uniques (ascending) plus the two
 * endpoints' vertex uniques; remove_vertex takes ONE shard unique at a time and
 * releases it before moving to the next neighbour shard; update_content takes
 * one shard shared lock, RELEASES it, then the vertex unique; dirty_vertices
 * takes one shard shared lock plus the visited vertices' shared locks.
 *
 * Traversals copy the guid list of each visited vertex under that vertex's
 * shard shared lock into a reused AdjacencyBuffer and release the lock before
 * walking to the next vertex, so a BFS holds no lock between visits and
 * traversals of different shards proceed fully in parallel.
 *
 * Cycle safety: has_path and shortest_path are BFS over an explicit visited
 * set, so a dependency cycle (A -> B -> A) terminates instead of looping.
 */
#include "graph.h"

  #include <algorithm>
  #include <cassert>
  #include <cstdio>
  #include <functional>
  #include <mutex>
  #include <thread>
  #include <utility>

namespace kimix::graph_db {

namespace {

inline uint64_t current_thread_hash() noexcept {
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

// Distinguishes "graph edge endpoints are the same shard" without comparing
// pointers (std::array elements are contiguous, so indices are the identity).
inline constexpr size_t kNoShard = static_cast<size_t>(-1);

// "no endpoint id" for connect_bulk's staging index (real ids start at 0).
inline constexpr uint32_t kNoEndpoint = static_cast<uint32_t>(-1);


} // namespace

// ---------------------------------------------------------------------------
// Shard
// ---------------------------------------------------------------------------

Graph::Shard::Shard() noexcept = default;

Vertex *Graph::Shard::_find(kimix::Guid const &guid) const noexcept {
    auto index = vertices.find(guid);
    if (!index) {
        return nullptr;
    }
    return index.value().get();
}

Graph::~Graph() noexcept = default;

// ---------------------------------------------------------------------------
// Write-contract bookkeeping
// ---------------------------------------------------------------------------

bool Graph::_check_writer() noexcept {
    uint64_t const mine = current_thread_hash();
    if (!_writer_recorded.load(std::memory_order_acquire)) {
        // First structural write of this graph: record the owner. A benign race
        // (two threads arriving together) is still caught by the compare below.
        _writer_thread_hash.store(mine, std::memory_order_release);
        _writer_recorded.store(true, std::memory_order_release);
        return true;
    }
    uint64_t const owner = _writer_thread_hash.load(std::memory_order_acquire);
    if (owner == mine) {
        return true;
    }
#ifndef NDEBUG
    // Debug-only enforcement: the contract says structural writes come from one
    // thread at a time, and a violation is a bug in the CALLER. Assert so the
    // debug/ASan test runs fail loudly; a release build keeps going with the
    // documented "single writer" assumption.
    std::fprintf(stderr,
                 "[kimix][graph_db] single-writer contract violated: structural "
                 "write from thread %llu, graph owned by thread %llu\n",
                 static_cast<unsigned long long>(mine),
                 static_cast<unsigned long long>(owner));
    assert(false && "kimix::graph_db::Graph: structural writes must come from one thread");
#endif
    return false;
}

bool Graph::is_writer_thread() const noexcept {
    if (!_writer_recorded.load(std::memory_order_acquire)) {
        return true;
    }
    return _writer_thread_hash.load(std::memory_order_acquire) == current_thread_hash();
}

void Graph::reset_writer_thread() noexcept {
    _writer_recorded.store(false, std::memory_order_release);
    _writer_thread_hash.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Structure (shard UNIQUE locks, ascending order)
// ---------------------------------------------------------------------------

Vertex *Graph::create_vertex(kimix::span<const std::byte> initial_content) {
    if (!_check_writer()) {
        return nullptr;
    }
    // `new Vertex` picks Vertex's own over-aligned operator new (mimalloc), so
    // the cache-line aligned lock region is guaranteed; a unique_ptr built by
    // hand rather than kimix::make_unique keeps that choice explicit.
    kimix::unique_ptr<Vertex> vertex{new Vertex(kimix::Guid{true}, initial_content)};
    Vertex *raw = vertex.get();
    kimix::Guid guid = raw->guid();
    Shard &shard = _shards[_shard_of(guid)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto inserted = shard.vertices.try_emplace(guid, std::move(vertex));
    if (!inserted.second) {
        // A guid collision is astronomically unlikely; report it as "not added"
        // rather than corrupting the map. Retry semantics belong to the caller.
        return nullptr;
    }
    _size.fetch_add(1, std::memory_order_relaxed);
    _bump_structure(); // a new vertex is (transiently) a root: invalidate roots()
    return raw;
}

bool Graph::add_vertex(kimix::unique_ptr<Vertex> vertex) {
    if (!vertex) {
        return false;
    }
    if (!_check_writer()) {
        return false;
    }
    kimix::Guid guid = vertex->guid();
    Shard &shard = _shards[_shard_of(guid)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto inserted = shard.vertices.try_emplace(guid, std::move(vertex));
    if (!inserted.second) {
        return false;
    }
    _size.fetch_add(1, std::memory_order_relaxed);
    _bump_structure(); // an adopted vertex is (transiently) a root
    return true;
}

kimix::unique_ptr<Vertex> Graph::remove_vertex(kimix::Guid const &guid) {
    if (!_check_writer()) {
        return nullptr;
    }
    size_t const own_shard = _shard_of(guid);
    kimix::unique_ptr<Vertex> victim;
    kimix::vector<kimix::Guid> neighbours;
    {
        // 1. Detach the victim from its own shard's map, copying out the guids of
        //    every neighbour it is linked to. Only the OWN shard is locked, so no
        //    second shard can be held here.
        std::unique_lock<std::shared_mutex> lock(_shards[own_shard].mutex);
        auto index = _shards[own_shard].vertices.find(guid);
        if (!index) {
            return nullptr;
        }
        victim = std::move(index.value());
        if (!victim) {
            return nullptr;
        }
        // `_depending_guids` are the outgoing targets (they hold `victim` in their
        // _depended set), `_depended_guids` the incoming sources (they hold it in
        // their _depending set). Both directions have to be unlinked, and the two
        // lists may legitimately share a guid (A -> B and B -> A), which is why
        // both removal helpers are called per neighbour below — each is a no-op
        // when the edge is absent.
        //
        // BOTH forms are collected, not just the guids: `connect` keeps the guid
        // lists and the pointer sets in lockstep, so the union of the two pointer
        // sets is a superset of the union of the two guid lists, and a vertex the
        // two forms disagree about would otherwise be missed by the cascade (a
        // dangling Vertex* in a neighbour's set — the one outcome the cascade
        // exists to prevent). Resolving a neighbour that no longer has the edge is
        // simply a no-op in the loop below, so the wider set costs a little
        // redundancy and buys an invariant that does not rely on that in-lockstep
        // assumption.
        neighbours.reserve(victim->_depending.size() + victim->_depended.size());
        for (auto *other : victim->_depending) {
            if (other != nullptr) {
                neighbours.push_back(other->guid());
            }
        }
        for (auto *other : victim->_depended) {
            if (other != nullptr) {
                neighbours.push_back(other->guid());
            }
        }
        for (auto const &other : victim->_depending_guids) {
            neighbours.push_back(other);
        }
        for (auto const &other : victim->_depended_guids) {
            neighbours.push_back(other);
        }
        _shards[own_shard].vertices.remove(guid);
        _size.fetch_sub(1, std::memory_order_relaxed);
        _bump_structure(); // the victim is gone: it is not a root (nor anything) now
    }
    // 2. Cascade: unlink the victim from every neighbour's pointer set and guid
    //    list, ONE shard lock at a time (never two), which is what keeps the
    //    deadlock-freedom argument trivial. The victim stays alive (it is the
    //    unique_ptr above, and it is returned to the caller), so even a reader
    //    that raced the detach can never touch freed memory: at worst it sees the
    //    edge for one more visit.
    for (auto const &other_guid : neighbours) {
        size_t const other_shard = _shard_of(other_guid);
        // A neighbour may well live in the victim's OWN shard (1/kGraphShardCount
        // of them do): the lock above was released, so re-taking it here is a
        // fresh acquisition, not recursion.
        std::unique_lock<std::shared_mutex> lock(_shards[other_shard].mutex);
        Vertex *other = _shards[other_shard]._find(other_guid);
        if (other == nullptr || other == victim.get()) {
            continue;
        }
        other->_remove_depended(victim.get());
        other->_remove_depending(victim.get());
    }
    victim->_clear_links();
    // The cascade's LAST bump. Every neighbour above lost an edge to a vertex that
    // no longer exists, so the root set changed in both directions; this bump is
    // what makes the whole cascade again visible to roots() (the detach's own bump
    // only covered the removal). A reader of the cache can therefore never stash a
    // half-cascaded answer under the final version: the version it stores is the
    // one it read BEFORE its scan, which is older than this one.
    _bump_structure();
    return victim;
}

bool Graph::_connect_resolved_locked(Vertex *from_vertex, Vertex *to_vertex) {
    // The ONE implementation of "this edge exists", shared by connect() (which
    // resolves its two endpoints per edge) and connect_bulk() (which resolves
    // each endpoint once per batch). The callers hold the endpoint shards'
    // UNIQUE locks, so the duplicate probe and the four container updates below
    // cannot interleave with a reader or with another structural write.
    if (from_vertex == nullptr || to_vertex == nullptr || from_vertex == to_vertex) {
        return false;
    }
    // Edge bookkeeping is graph structure: both endpoints' shard locks are held
    // UNIQUE, so the two guid lists and the two pointer sets would already be
    // safe. The two vertex uniques are taken on top of that because the contract
    // in graph.h says so — they also flush a reader that is in the middle of a
    // content read of either endpoint, which keeps the "shard lock, then vertex
    // lock" nesting exercised and tested. from != to is guaranteed by the
    // caller, so the two uniques are never the same mutex
    // (shared_atomic_mutex is not recursive).
    from_vertex->lock_unique();
    to_vertex->lock_unique();
    if (!from_vertex->_add_depending_if_absent(to_vertex)) {
        // The probe and the insert are ONE hash operation, so an edge that is
        // already present is discovered here instead of by a separate find().
        // connect() and connect_bulk() therefore agree on "already present"
        // without either of them paying for two lookups.
        to_vertex->unlock_unique();
        from_vertex->unlock_unique();
        return false;
    }
    to_vertex->_add_depended(from_vertex);
    _bump_structure(); // `to_vertex` gained an incoming edge, so it left the root set
    to_vertex->unlock_unique();
    from_vertex->unlock_unique();
    return true;
}

bool Graph::_connect_locked(Shard &from_shard, Shard &to_shard,
                            kimix::Guid const &from, kimix::Guid const &to) {
    Vertex *from_vertex = from_shard._find(from);
    Vertex *to_vertex = to_shard._find(to);
    return _connect_resolved_locked(from_vertex, to_vertex);
}

bool Graph::_disconnect_locked(Shard &from_shard, Shard &to_shard,
                               kimix::Guid const &from, kimix::Guid const &to) {
    Vertex *from_vertex = from_shard._find(from);
    Vertex *to_vertex = to_shard._find(to);
    if (from_vertex == nullptr || to_vertex == nullptr) {
        return false;
    }
    if (from_vertex->_depending.find(to_vertex) == from_vertex->_depending.end()) {
        return false;
    }
    from_vertex->lock_unique();
    to_vertex->lock_unique();
    from_vertex->_remove_depending(to_vertex);
    to_vertex->_remove_depended(from_vertex);
    _bump_structure(); // `to_vertex` lost an incoming edge, so it may be a root now
    to_vertex->unlock_unique();
    from_vertex->unlock_unique();
    return true;
}

bool Graph::connect(kimix::Guid const &from, kimix::Guid const &to) {
    if (from == to) {
        return false;
    }
    if (!_check_writer()) {
        return false;
    }
    size_t const from_shard = _shard_of(from);
    size_t const to_shard = _shard_of(to);
    if (from_shard == to_shard) {
        // Both endpoints live in one shard: ONE unique lock serves both, and no
        // ordering question arises. (Self-recursion on a shared_mutex would
        // deadlock, which is exactly why this case is special-cased.)
        std::unique_lock<std::shared_mutex> lock(_shards[from_shard].mutex);
        return _connect_locked(_shards[from_shard], _shards[from_shard], from, to);
    }
    // Two shards: UNIQUE in ascending shard order (the lock-order rule in
    // graph.h). Only connect/disconnect ever take two, and the single-writer
    // contract keeps a second thread out of this pair anyway.
    size_t const first = from_shard < to_shard ? from_shard : to_shard;
    size_t const second = from_shard < to_shard ? to_shard : from_shard;
    std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
    std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
    return _connect_locked(_shards[from_shard], _shards[to_shard], from, to);
}

bool Graph::disconnect(kimix::Guid const &from, kimix::Guid const &to) {
    if (!_check_writer()) {
        return false;
    }
    size_t const from_shard = _shard_of(from);
    size_t const to_shard = _shard_of(to);
    if (from_shard == to_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[from_shard].mutex);
        return _disconnect_locked(_shards[from_shard], _shards[from_shard], from, to);
    }
    size_t const first = from_shard < to_shard ? from_shard : to_shard;
    size_t const second = from_shard < to_shard ? to_shard : from_shard;
    std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
    std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
    return _disconnect_locked(_shards[from_shard], _shards[to_shard], from, to);
}

// ---------------------------------------------------------------------------
// Bulk edge creation (epoch 3): the loader's link pass
// ---------------------------------------------------------------------------

size_t Graph::connect_bulk(kimix::span<EdgeRequest const> edges,
                           kimix::span<EdgeOutcome> outcomes) noexcept {
    size_t const count = edges.size();
    bool const want_outcomes = outcomes.size() >= count;
    if (count == 0) {
        return 0;
    }
    // ONE single-writer check for the WHOLE batch, not one per edge: the caller
    // of a bulk load is a single-threaded phase by contract (graph.h, WRITE
    // POLICY), and per-edge recording at ~600k edges is pure overhead.
    if (!_check_writer()) {
        if (want_outcomes) {
            for (size_t i = 0; i < count; ++i) {
                outcomes[i] = EdgeOutcome::refused;
            }
        }
        return 0;
    }

    // --- 1. resolve every DISTINCT endpoint once ----------------------------
    // Two parallel structures, indexed by a dense endpoint id:
    //   endpoints[id]      the resolved (Vertex*, shard) pair
    //   resolved[guid]     guid -> id
    // The pairing is deliberate: ankerl::unordered_dense stores its values in a
    // dense array, so a rehash INVALIDATES every pointer into `resolved`, but it
    // never moves `endpoints`, so an id stays valid for the whole call. (Storing
    // Endpoint* into `resolved` and reusing them across a growth was measured to
    // crash - that is why the ids exist.)
    //
    // The load's link pass passes ~2xN endpoints for N edges with heavy reuse
    // (every vertex appears once per incident edge), so this collapses ~600k
    // Graph::find calls into ~100k lookups: one SHARED shard lock per distinct
    // endpoint instead of one per edge.
    //
    // The vertices stay alive for the whole call: removal is a structural write
    // and the single-writer contract excludes it from running concurrently with
    // this one (graph.h - the same argument update_content makes for its lookup).
    struct Endpoint {
        Vertex *vertex;
        uint32_t shard;
    };
    kimix::vector<Endpoint> endpoints;
    endpoints.reserve(count / 4 + 16); // hint only; ids are stable under growth
    // A quarter of the edge count covers the load's shape (a 6M-edge store names
    // ~1M distinct endpoints) without over-reserving; a shape that shares less
    // simply grows the table once, which costs a rehash of ids and nothing else.
    kimix::unordered_map<kimix::Guid, uint32_t> resolved;
    resolved.reserve(count / 4 + 16);
    kimix::Guid last_guid{};
    uint32_t last_id = kNoEndpoint;
    auto resolve = [this, &endpoints, &resolved, &last_guid,
                    &last_id](kimix::Guid const &guid) -> uint32_t {
        // The link pass emits one vertex's whole outgoing list at a time, so a
        // one-entry (guid, id) memo removes the second hash probe for the ~2
        // consecutive edges of the same source, for the cost of two compares.
        if (last_id != kNoEndpoint && last_guid == guid) {
            return last_id;
        }
        auto known = resolved.find(guid);
        if (known != resolved.end()) {
            last_guid = guid;
            last_id = known->second;
            return last_id;
        }
        size_t const shard_index = _shard_of(guid);
        Vertex *vertex = nullptr;
        {
            std::shared_lock<std::shared_mutex> lock(_shards[shard_index].mutex);
            vertex = _shards[shard_index]._find(guid);
        }
        if (vertex == nullptr) {
            return kNoEndpoint; // unknown endpoint: the edge is skipped, counted
        }
        uint32_t const id = static_cast<uint32_t>(endpoints.size());
        endpoints.push_back(Endpoint{vertex, static_cast<uint32_t>(shard_index)});
        resolved.emplace(guid, id);
        last_guid = guid;
        last_id = id;
        return id;
    };

    // Resolve pass: one (cached) lookup per request endpoint. Self edges and
    // unknown endpoints are decided HERE, before any structural lock is taken, so
    // a store full of dangling dependencies costs a batch of shared lookups and
    // nothing else.
    kimix::vector<uint32_t> from_ends(count, kNoEndpoint);
    kimix::vector<uint32_t> to_ends(count, kNoEndpoint);
    kimix::vector<EdgeOutcome> verdicts(count, EdgeOutcome::created);
    kimix::vector<uint8_t> pending(count, 0);
    for (size_t i = 0; i < count; ++i) {
        if (edges[i].from == edges[i].to) {
            verdicts[i] = EdgeOutcome::self_edge;
            continue;
        }
        uint32_t const from_id = resolve(edges[i].from);
        if (from_id == kNoEndpoint) {
            verdicts[i] = EdgeOutcome::missing_source;
            continue;
        }
        uint32_t const to_id = resolve(edges[i].to);
        if (to_id == kNoEndpoint) {
            verdicts[i] = EdgeOutcome::missing_target;
            continue;
        }
        from_ends[i] = from_id;
        to_ends[i] = to_id;
        pending[i] = 1;
    }

    // --- 2. take every touched shard UNIQUE, in ascending index order, ONCE -
    // The batch-wide lock set is the union of the pending edges' endpoint shards,
    // so an edge whose two endpoints share a shard needs no ordering decision and
    // no edge pays an acquire/release pair. The set is COLLECTED first and then
    // acquired by ascending index, which is exactly what graph.h's
    // deadlock-freedom argument requires: a bulk caller waits only for locks at
    // its own level with a strictly higher index. One unique_lock slot per shard
    // takes each at most once (std::shared_mutex is not recursive, so the second
    // edge of a shard must NOT re-acquire it) and the array's destruction
    // releases them all.
    std::array<uint8_t, kGraphShardCount> touched{};
    for (size_t i = 0; i < count; ++i) {
        if (pending[i] == 0) {
            continue;
        }
        touched[endpoints[from_ends[i]].shard] = 1;
        touched[endpoints[to_ends[i]].shard] = 1;
    }
    std::array<std::unique_lock<std::shared_mutex>, kGraphShardCount> held;
    for (size_t shard = 0; shard < kGraphShardCount; ++shard) {
        if (touched[shard] != 0) {
            held[shard] = std::unique_lock<std::shared_mutex>(_shards[shard].mutex);
        }
    }

    // --- 3. create the edges ------------------------------------------------
    // Same effect as connect(), with the endpoints already resolved and the shard
    // locks already held: _connect_resolved_locked performs the identical
    // duplicate test and the identical two-sided bookkeeping, in the same order,
    // so the resulting guid lists, pointer sets and reverse edges are exactly what
    // the per-edge connect loop produced.
    size_t created = 0;
    for (size_t i = 0; i < count; ++i) {
        if (pending[i] == 0) {
            continue;
        }
        if (_connect_resolved_locked(endpoints[from_ends[i]].vertex,
                                     endpoints[to_ends[i]].vertex)) {
            ++created;
        } else {
            verdicts[i] = EdgeOutcome::duplicate_edge; // already present
        }
    }
    if (want_outcomes) {
        for (size_t i = 0; i < count; ++i) {
            outcomes[i] = verdicts[i];
        }
    }
    return created;
}

bool Graph::reserve_links(kimix::Guid const &guid, size_t outgoing, size_t incoming) {
    if (!_check_writer()) {
        return false;
    }
    // One shard UNIQUE lock (the vertex's own), i.e. exactly the locking level the
    // edge lists live at: the containers being reserved are graph structure, and
    // no second shard is ever involved, so the ascending-index rule is vacuous
    // here. Taking it unique rather than shared is deliberate - reserve() mutates
    // the containers, so a concurrent reader must not walk them meanwhile.
    Shard &shard = _shards[_shard_of(guid)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return false;
    }
    vertex->reserve_links(outgoing, incoming);
    return true;
}

// ---------------------------------------------------------------------------
// Level-2 write: vertex lock ONLY (one shard shared lock for the lookup)
// ---------------------------------------------------------------------------

bool Graph::update_content(kimix::Guid const &guid,
                           kimix::span<const std::byte> content) {
    // Epoch 1: the lookup takes the SHARED lock of the vertex's OWN shard and
    // releases it before the vertex unique lock is taken and before the MD5 is
    // recomputed. Two threads updating two different vertices therefore contend
    // only when their guids happen to share a shard (1/64 of the pairs) — epoch 0
    // serialized all of them on the one graph lock, which is what made this path
    // 17x slower than Vertex::set_content on the same disjoint partitions.
    // Removal is a structural write and the single-writer contract excludes it
    // from running concurrently with an update (see graph.h), so the Vertex*
    // resolved here cannot be freed underneath the vertex lock below.
    Shard &shard = _shards[_shard_of(guid)];
    Vertex *vertex = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        vertex = shard._find(guid);
    }
    if (vertex == nullptr) {
        return false;
    }
    vertex->set_content(content); // vertex UNIQUE lock, shared readers excluded
    return true;
}

// ---------------------------------------------------------------------------
// Reads (one shard SHARED lock each)
// ---------------------------------------------------------------------------

bool Graph::_is_root(Vertex const &vertex) noexcept {
    return vertex._depended.empty() && vertex._depended_guids.empty();
}

Vertex *Graph::find(kimix::Guid const &guid) const {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    return shard._find(guid);
}

bool Graph::contains(kimix::Guid const &guid) const {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    return shard._find(guid) != nullptr;
}

kimix::vector<Vertex *> Graph::_compute_roots() const {
    kimix::vector<Vertex *> out;
    for (auto const &shard : _shards) {
        // One shard shared lock at a time: shards are visited sequentially, so
        // this never holds two and never blocks a writer of another shard.
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (auto const &entry : shard.vertices) {
            Vertex *vertex = entry.second.get();
            if (vertex != nullptr && _is_root(*vertex)) {
                out.push_back(vertex);
            }
        }
    }
    return out;
}

kimix::vector<Vertex *> Graph::roots() const {
    // Two-level read, mirroring the graph's own design:
    //   1. Take the cache SHARED and compare the structural version. That is the
    //      whole cost of a warm call: one shared lock (readers never block each
    //      other) plus a copy of the cached vector, which is what the caller
    //      wants anyway. The version is read AFTER the lock, so a writer that
    //      bumped it (under its shard lock, before releasing it) is visible.
    //   2. Only a stale cache takes it UNIQUE and recomputes. The version stored
    //      is the one read at step 1, i.e. the state _compute_roots() is about to
    //      observe from the start: a structural write landing mid-scan bumps the
    //      counter again (the writer's bump does not need this mutex), so the
    //      cache is left stale and the next call recomputes instead of caching a
    //      half-updated answer.
    {
        std::shared_lock<std::shared_mutex> lock(_roots_mutex);
        if (_roots_valid) {
            uint64_t const version = _structure_version.load(std::memory_order_relaxed);
            if (_cached_root_version == version) {
                return _cached_roots;
            }
        }
    }
    std::unique_lock<std::shared_mutex> lock(_roots_mutex);
    uint64_t const version = _structure_version.load(std::memory_order_relaxed);
    if (_roots_valid && _cached_root_version == version) {
        return _cached_roots; // another reader recomputed while we waited
    }
    _cached_roots = _compute_roots();
    _cached_root_version = version;
    _roots_valid = true;
    return _cached_roots;
}

// ---------------------------------------------------------------------------
// Adjacency (allocating and non-allocating forms)
// ---------------------------------------------------------------------------

kimix::vector<kimix::Guid> Graph::_copy_adjacency(kimix::Guid const &guid,
                                                  bool outgoing) const {
    // The allocating form copies the guid list straight into the returned vector
    // under one shard shared lock: one allocation, no intermediate buffer.
    kimix::vector<kimix::Guid> out;
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return out;
    }
    out = outgoing ? vertex->_depending_guids : vertex->_depended_guids;
    return out;
}

kimix::vector<kimix::Guid> Graph::dependencies(kimix::Guid const &guid) const {
    return _copy_adjacency(guid, true);
}

kimix::vector<kimix::Guid> Graph::dependents(kimix::Guid const &guid) const {
    return _copy_adjacency(guid, false);
}

bool Graph::_read_adjacency(kimix::Guid const &guid, bool outgoing,
                            AdjacencyBuffer &out) const noexcept {
    out._clear();
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return false;
    }
    out._assign(outgoing ? vertex->_depending_guids : vertex->_depended_guids);
    return true;
}

size_t Graph::_read_adjacency(kimix::Guid const &guid, bool outgoing,
                              kimix::span<kimix::Guid> out) const noexcept {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return 0;
    }
    auto const &source = outgoing ? vertex->_depending_guids : vertex->_depended_guids;
    size_t const copy = std::min(source.size(), out.size());
    std::copy(source.begin(), source.begin() + copy, out.begin());
    return source.size();
}

bool Graph::dependencies(kimix::Guid const &guid, AdjacencyBuffer &out) const noexcept {
    return _read_adjacency(guid, true, out);
}

bool Graph::dependents(kimix::Guid const &guid, AdjacencyBuffer &out) const noexcept {
    return _read_adjacency(guid, false, out);
}

size_t Graph::dependencies(kimix::Guid const &guid,
                           kimix::span<kimix::Guid> out) const noexcept {
    return _read_adjacency(guid, true, out);
}

size_t Graph::dependents(kimix::Guid const &guid,
                         kimix::span<kimix::Guid> out) const noexcept {
    return _read_adjacency(guid, false, out);
}

kimix::vector<kimix::Guid> Graph::neighbors(kimix::Guid const &guid) const {
    // Undirected adjacency = outgoing + incoming, deduplicated. Epoch 0 built a
    // kimix::unordered_set per call (an allocation + one XXH3 per guid) and copied
    // both lists into the result; the depth-3 expansion of the benchmark issues one
    // such call per user op, so this was a visible share of the op.
    //
    // Epoch 1: dedup into a STACK array first and materialise the vector once with
    // its exact size, i.e. exactly one allocation for a vertex whose combined
    // degree fits the array — which covers the whole 4..8 out-degree read graph and
    // any realistic hub-free shape. Only a vertex with a larger combined degree
    // falls back to the hash set, where the quadratic linear scan would start to
    // cost more than the hashing. Both paths produce the identical order: the
    // outgoing list first, then the incoming list, each skipping a guid already
    // emitted.
    constexpr size_t kStackDedup = 16;
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return {};
    }
    auto const &depending = vertex->_depending_guids;
    auto const &depended = vertex->_depended_guids;
    size_t const total = depending.size() + depended.size();
    if (total <= kStackDedup) {
        kimix::Guid deduped[kStackDedup];
        size_t count = 0;
        for (auto const &other : depending) {
            if (std::find(deduped, deduped + count, other) == deduped + count) {
                deduped[count++] = other;
            }
        }
        for (auto const &other : depended) {
            if (std::find(deduped, deduped + count, other) == deduped + count) {
                deduped[count++] = other;
            }
        }
        return kimix::vector<kimix::Guid>{deduped, deduped + count};
    }
    kimix::vector<kimix::Guid> out;
    out.reserve(total);
    kimix::unordered_set<kimix::Guid> seen;
    seen.reserve(total);
    for (auto const &other : depending) {
        if (seen.insert(other).second) {
            out.push_back(other);
        }
    }
    for (auto const &other : depended) {
        if (seen.insert(other).second) {
            out.push_back(other);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Traversals (one shard SHARED lock per VISIT, none held between visits)
// ---------------------------------------------------------------------------

namespace {

// Small-BFS visited set: the queue itself IS the visited set (every discovered
// vertex is pushed exactly once in discovery order), so a linear scan of it is
// both the cheapest and the smallest form for a bounded traversal. Only when the
// walk grows past kLinearVisitedLimit does `hash` take over, primed with the
// entries already discovered, so the membership test stays O(1) from then on.
// This removes one hash-set allocation + one hash per hop from has_path's inner
// loop, which is the common case (the benchmark's pairs are 1-2 hops apart).
constexpr size_t kLinearVisitedLimit = 32;

class VisitedSet {
public:
    void reserve(size_t n) {
        _hash.reserve(n < kLinearVisitedLimit ? kLinearVisitedLimit : n);
    }

    // Was `guid` already discovered? `queue` is the discovery order.
    [[nodiscard]] bool contains(kimix::vector<kimix::Guid> const &queue,
                                kimix::Guid const &guid) {
        if (!_hashed) {
            return std::find(queue.begin(), queue.end(), guid) != queue.end();
        }
        return _hash.find(guid) != _hash.end();
    }

    // Insert `guid` (the caller has already pushed it onto `queue`).
    void insert(kimix::vector<kimix::Guid> const &queue, kimix::Guid const &guid) {
        if (_hashed) {
            _hash.insert(guid);
            return;
        }
        if (queue.size() > kLinearVisitedLimit) {
            // Switch over: prime the hash with everything discovered so far.
            _hash.reserve(queue.size() * 2);
            for (auto const &known : queue) {
                _hash.insert(known);
            }
            _hashed = true;
        }
    }

private:
    kimix::unordered_set<kimix::Guid> _hash;
    bool _hashed{false};
};

} // namespace

bool Graph::has_path(kimix::Guid const &from, kimix::Guid const &to) const {
    // Epoch 0 did two upfront lookups ("unknown endpoint => false"). Neither is
    // needed for the walk: a BFS from an absent `from` discovers nothing, and an
    // absent `to` can never be reached. Only the zero-length case has to ask
    // whether `from` exists, so the common path pays ONE shard shared lock fewer
    // than epoch 0 (and the walk's first visit already re-resolves `from`).
    if (from == to) {
        return contains(from);
    }
    // Breadth-first walk over outgoing edges with an explicit visited set: a
    // dependency cycle is entered once per vertex, so the walk always
    // terminates. `cursor` walks the queue instead of erasing from its front,
    // which keeps the queue a plain vector.
    kimix::vector<kimix::Guid> queue;
    queue.reserve(16);
    VisitedSet visited;
    visited.reserve(16);
    AdjacencyBuffer adjacency; // reused: no per-visit allocation
    queue.push_back(from);
    visited.insert(queue, from);
    for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
        kimix::Guid const current = queue[cursor];
        if (!dependencies(current, adjacency)) {
            continue; // removed between the enqueue and the visit
        }
        for (auto const &next : adjacency) {
            if (next == to) {
                return true;
            }
            if (!visited.contains(queue, next)) {
                queue.push_back(next);
                visited.insert(queue, next);
            }
        }
    }
    return false;
}

kimix::vector<kimix::Guid>
Graph::shortest_path(kimix::Guid const &from, kimix::Guid const &to) const {
    // Same reasoning as has_path: an absent endpoint cannot appear in the
    // predecessor table, so a walk from an unknown `from` and a walk towards an
    // unknown `to` both return {} on their own. Only from == to needs a lookup.
    if (from == to) {
        return contains(from) ? kimix::vector<kimix::Guid>{from}
                              : kimix::vector<kimix::Guid>{};
    }
    // BFS with a predecessor table; the visited set doubles as the cycle guard,
    // so a cyclic graph terminates with the shortest simple path (or {}).
    kimix::vector<kimix::Guid> frontier;
    frontier.push_back(from);
    kimix::vector<kimix::Guid> next_frontier;
    kimix::unordered_map<kimix::Guid, kimix::Guid> predecessor;
    predecessor.reserve(32);
    predecessor.emplace(from, from);
    AdjacencyBuffer adjacency; // reused: no per-visit allocation
    bool found = false;
    while (!frontier.empty() && !found) {
        next_frontier.clear();
        for (auto const &current : frontier) {
            if (!dependencies(current, adjacency)) {
                continue;
            }
            for (auto const &next : adjacency) {
                if (predecessor.find(next) != predecessor.end()) {
                    continue;
                }
                predecessor.emplace(next, current);
                if (next == to) {
                    found = true;
                    break;
                }
                next_frontier.push_back(next);
            }
            if (found) {
                break;
            }
        }
        frontier.swap(next_frontier);
    }
    if (!found) {
        return {};
    }
    kimix::vector<kimix::Guid> path;
    kimix::Guid cursor = to;
    while (cursor != from) {
        path.push_back(cursor);
        auto it = predecessor.find(cursor);
        if (it == predecessor.end()) {
            return {};
        }
        cursor = it->second;
    }
    path.push_back(from);
    std::reverse(path.begin(), path.end());
    return path;
}

// ---------------------------------------------------------------------------
// Dirty bookkeeping
// ---------------------------------------------------------------------------

void Graph::mark_dirty() noexcept {
    for (auto const &shard : _shards) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (auto const &entry : shard.vertices) {
            if (entry.second) {
                entry.second->mark_dirty();
            }
        }
    }
}

void Graph::clear_dirty() noexcept {
    for (auto const &shard : _shards) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (auto const &entry : shard.vertices) {
            if (entry.second) {
                entry.second->clear_dirty();
            }
        }
    }
}

kimix::vector<DirtyVertex> Graph::dirty_vertices() const {
    kimix::vector<DirtyVertex> out;
    for (auto const &shard : _shards) {
        // Shard SHARED (map), then each vertex's SHARED lock for the hash: the
        // documented high-to-low order, never the inverse.
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        out.reserve(out.size() + shard.vertices.size());
        for (auto const &entry : shard.vertices) {
            Vertex *vertex = entry.second.get();
            if (vertex == nullptr || !vertex->is_dirty()) {
                continue;
            }
            DirtyVertex item;
            item.vertex = vertex;
            item.guid = vertex->guid();
            item.content_hash = vertex->content_hash(); // vertex SHARED lock
            out.push_back(item);
        }
    }
    return out;
}

} // namespace kimix::graph_db