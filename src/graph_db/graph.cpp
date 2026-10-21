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

Vertex *Graph::_resolve(kimix::Guid const &guid) const noexcept {
    size_t const shard_index = _shard_of(guid);
    std::shared_lock<std::shared_mutex> lock(_shards[shard_index].mutex);
    return _shards[shard_index]._find(guid);
}

kimix::unique_ptr<Vertex> Graph::remove_vertex(kimix::Guid const &guid) {
    return remove_vertex(guid, nullptr);
}

kimix::unique_ptr<Vertex>
Graph::remove_vertex(kimix::Guid const &guid,
                     kimix::vector<kimix::unique_ptr<Edge>> *removed_edges) {
    if (!_check_writer()) {
        return nullptr;
    }
    size_t const own_shard = _shard_of(guid);
    kimix::unique_ptr<Vertex> victim;
    // The cascade's work list: one entry per incident EDGE, holding the edge id,
    // the neighbour guid and whether the victim was its origin (so the neighbour's
    // list to clean is decided without touching the edge record again).
    struct Cascade {
        kimix::Guid edge;
        kimix::Guid neighbour;
        bool victim_is_origin;
    };
    kimix::vector<Cascade> cascade;
    {
        // 1. Detach the victim from its own shard's map, copying out every
        //    incident edge. Only the OWN shard is locked, so no second shard can
        //    be held here.
        std::unique_lock<std::shared_mutex> lock(_shards[own_shard].mutex);
        auto index = _shards[own_shard].vertices.find(guid);
        if (!index) {
            return nullptr;
        }
        victim = std::move(index.value());
        if (!victim) {
            return nullptr;
        }
        cascade.reserve(victim->_out_links.size() + victim->_in_links.size() +
                        victim->_depending_guids.size() + victim->_depended_guids.size());
        // The link lists are authoritative (one entry per EDGE, so parallel edges
        // and self loops are covered exactly once each). The guid projections are
        // collected as well, defensively: a vertex whose two forms disagreed
        // would otherwise leave a dangling entry behind, and resolving a
        // neighbour that no longer has the link is a no-op below.
        for (auto const &link : victim->_out_links) {
            cascade.push_back(Cascade{link.edge, link.neighbour, true});
        }
        for (auto const &link : victim->_in_links) {
            cascade.push_back(Cascade{link.edge, link.neighbour, false});
        }
        _shards[own_shard].vertices.remove(guid);
        _size.fetch_sub(1, std::memory_order_relaxed);
        // Tombstone: a save never deletes a stored key, so the removal has to be
        // recorded for the next save to write (see graph.h).
        _removed_vertices.push_back(guid);
        _bump_structure(); // the victim is gone: it is not a root (nor anything) now
    }

    // 2. Drop the incident edge RECORDS. One edge shard lock at a time, taken
    //    AFTER the vertex shard lock above has been released (level 2 -> 3). A
    //    transaction keeps the removed objects so an abort can put them back.
    for (auto const &item : cascade) {
        kimix::unique_ptr<Edge> removed = _edges.erase(item.edge);
        if (removed) {
            // Tombstone every cascaded edge too, or the next load would bring it
            // back with a dangling endpoint.
            _removed_edges.push_back(item.edge);
            if (removed_edges != nullptr) {
                removed_edges->push_back(std::move(removed));
            }
        }
    }

    // 2b. The key index entry (level 4, one shard, after every higher level is
    //     released). A vertex without a key owns no entry.
    if (victim->has_key()) {
        kimix::Guid removed;
        _keys.erase(victim->key(), removed);
    }

    // 3. Cascade: unlink the victim from every neighbour's link list and guid
    //    projection, ONE shard lock at a time (never two), which is what keeps the
    //    deadlock-freedom argument trivial. The victim stays alive (it is the
    //    unique_ptr above, and it is returned to the caller), so even a reader
    //    that raced the detach can never touch freed memory: at worst it sees the
    //    edge for one more visit.
    for (auto const &item : cascade) {
        if (item.neighbour == guid) {
            // A self loop lives entirely on the victim, which is already detached.
            continue;
        }
        size_t const other_shard = _shard_of(item.neighbour);
        // A neighbour may well live in the victim's OWN shard
        // (1/kGraphShardCount of them do): the lock above was released, so
        // re-taking it here is a fresh acquisition, not recursion.
        std::unique_lock<std::shared_mutex> lock(_shards[other_shard].mutex);
        Vertex *other = _shards[other_shard]._find(item.neighbour);
        if (other == nullptr || other == victim.get()) {
            continue;
        }
        // The neighbour's OUT list holds an edge whose origin is the victim; its
        // IN list an edge whose target is the victim.
        if (item.victim_is_origin) {
            other->_remove_in_link(item.edge, victim.get());
        } else {
            other->_remove_out_link(item.edge, victim.get());
        }
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

// ---------------------------------------------------------------------------
// Keyed vertex creation / translation (P2)
// ---------------------------------------------------------------------------

GdiStatus Graph::create_vertex(VertexSpec const &spec, kimix::Guid &out_guid) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    bool const keyed = spec.key_label != kNoLabel;
    if (keyed && spec.external_id.empty()) {
        // GDI's VertexKey.Validate: the label must not be empty, and an id of
        // zero length names nothing.
        return GdiStatus::error_empty_name;
    }
    if (!keyed && !spec.external_id.empty()) {
        return GdiStatus::error_argument; // an id without a label names nothing
    }

    // The key probe happens BEFORE the structural write, under one key shard
    // shared lock (level 4 alone — the hierarchy forbids taking it while a
    // higher level is held, and nothing is held here).
    if (keyed) {
        VertexKeyView const view{spec.key_label, spec.external_id};
        kimix::Guid existing;
        if (_keys.find(view, existing)) {
            if (_options.key_mode == KeyMode::report_warning) {
                // GDI-RMA's under-enforced uniqueness: the caller gets the
                // EXISTING vertex plus the warning, and nothing is created.
                out_guid = existing;
                return GdiStatus::warning_non_unique_id;
            }
            return GdiStatus::error_non_unique_id;
        }
    }

    kimix::unique_ptr<Vertex> vertex{new Vertex(kimix::Guid{true}, spec.content)};
    if (keyed) {
        vertex->_set_key(spec.key_label, ExternalId::from_bytes(spec.external_id));
    }
    for (auto label : spec.labels) {
        vertex->_labels_unlocked_push(label);
    }
    if (spec.properties != nullptr) {
        vertex->_properties_unlocked_set(*spec.properties);
    }
    Vertex *raw = vertex.get();
    kimix::Guid const guid = raw->guid();
    Shard &shard = _shards[_shard_of(guid)];
    {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        auto inserted = shard.vertices.try_emplace(guid, std::move(vertex));
        if (!inserted.second) {
            return GdiStatus::error_intern; // guid collision: astronomically unlikely
        }
        _size.fetch_add(1, std::memory_order_relaxed);
        _bump_structure();
    }
    if (keyed) {
        // Level 4 AFTER the vertex shard has been released.
        GdiStatus const status = _keys.insert(raw->key(), guid);
        if (is_error(status)) {
            // A concurrent create won the race between the probe and now: undo the
            // insertion instead of leaving an unindexed vertex behind.
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            shard.vertices.remove(guid);
            _size.fetch_sub(1, std::memory_order_relaxed);
            _bump_structure();
            return status;
        }
    }
    out_guid = guid;
    return GdiStatus::ok;
}

GdiStatus Graph::get_or_create_vertex(VertexSpec const &spec, kimix::Guid &out_guid,
                                      bool &created) {
    created = false;
    if (spec.key_label != kNoLabel) {
        VertexKeyView const view{spec.key_label, spec.external_id};
        kimix::Guid existing;
        if (_keys.find(view, existing)) {
            // MERGE semantics: a match is returned untouched — no label, no
            // property is applied (the C# reference does exactly this).
            out_guid = existing;
            return GdiStatus::ok;
        }
    }
    GdiStatus const status = create_vertex(spec, out_guid);
    if (is_error(status)) {
        return status;
    }
    created = true;
    return GdiStatus::ok;
}

GdiStatus Graph::translate_vertex_id(VertexKeyView const &key, kimix::Guid &out_guid,
                                     bool &found) const {
    found = false;
    if (key.label == kNoLabel) {
        return GdiStatus::error_empty_name;
    }
    kimix::Guid existing;
    if (!_keys.find(key, existing)) {
        // GDI's TranslateVertexID answers "no such key" with a null uid, not an
        // error; the caller distinguishes the two through `found`.
        return GdiStatus::ok;
    }
    out_guid = existing;
    found = true;
    return GdiStatus::ok;
}

GdiStatus Graph::free_vertex(kimix::Guid const &guid, bool guarded) {
    if (guarded) {
        if (!contains(guid)) {
            return GdiStatus::error_vertex;
        }
        if (has_incident_edges(guid, EdgeOrientation::both)) {
            // GDI's FreeVertexGuarded: ErrorEdge and NOTHING removed.
            return GdiStatus::error_edge;
        }
    }
    kimix::unique_ptr<Vertex> removed = remove_vertex(guid);
    if (!removed) {
        return GdiStatus::error_vertex;
    }
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// First-class edges (P3)
// ---------------------------------------------------------------------------

bool Graph::_has_out_edge_locked(Vertex const *origin,
                                 kimix::Guid const &target) noexcept {
    for (auto const &link : origin->_out_links) {
        if (link.neighbour == target) {
            return true;
        }
    }
    return false;
}

GdiStatus Graph::_create_edge_locked(Vertex *origin, Vertex *target,
                                     EdgeDirectionType direction,
                                     kimix::span<const LabelId> labels,
                                     PropertyList const *properties, bool via_gdi_api,
                                     EdgeId &out_edge) {
    if (origin == nullptr || target == nullptr) {
        return GdiStatus::error_vertex;
    }
    bool const self_loop = origin == target;
    if (self_loop && !(via_gdi_api && _options.allow_self_edges)) {
        // The legacy contract: from == to is never an edge.
        return GdiStatus::error_edge;
    }
    if (!via_gdi_api || !_options.allow_parallel_edges) {
        if (_has_out_edge_locked(origin, target->guid())) {
            return GdiStatus::error_edge;
        }
    }
    EdgeId const id{kimix::Guid{true}};
    kimix::unique_ptr<Edge> edge{new Edge(id, origin->guid(), target->guid(), direction)};
    for (auto label : labels) {
        if (label == kNoLabel) {
            return GdiStatus::error_label;
        }
        edge->_labels.push_back(label);
    }
    if (properties != nullptr) {
        for (auto const &entry : *properties) {
            edge->_properties.emplace(entry.name, entry.value);
        }
    }
    // Record first, then the adjacency: an adjacency entry that outlives its
    // record would be a dangling edge for every reader, while a record inserted a
    // moment earlier is invisible until a link names it.
    if (!_edges.insert(std::move(edge))) {
        return GdiStatus::error_intern;
    }
    // The two vertex uniques flush a reader that is in the middle of a payload
    // read; for a self loop origin == target and the mutex is taken ONCE
    // (shared_atomic_mutex is not recursive).
    origin->lock_unique();
    if (!self_loop) {
        target->lock_unique();
    }
    origin->_add_out_link(id, target);
    target->_add_in_link(id, origin);
    _bump_structure(); // `target` gained an incoming edge, so it left the root set
    if (!self_loop) {
        target->unlock_unique();
    }
    origin->unlock_unique();
    out_edge = id;
    return GdiStatus::ok;
}

GdiStatus Graph::edge_create(kimix::Guid const &origin, kimix::Guid const &target,
                             EdgeDirectionType direction, kimix::span<const LabelId> labels,
                             PropertyList const *properties, kimix::Guid &out_edge) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    if (direction != EdgeDirectionType::directed &&
        direction != EdgeDirectionType::undirected) {
        return GdiStatus::error_edge_orientation;
    }
    size_t const origin_shard = _shard_of(origin);
    size_t const target_shard = _shard_of(target);
    if (!_options.allow_self_edges && origin == target) {
        return GdiStatus::error_edge;
    }
    if (origin_shard == target_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[origin_shard].mutex);
        Vertex *from_vertex = _shards[origin_shard]._find(origin);
        Vertex *to_vertex = _shards[origin_shard]._find(target);
        return _create_edge_locked(from_vertex, to_vertex, direction, labels, properties,
                                   /*via_gdi_api=*/true, out_edge);
    }
    // Two shards: UNIQUE in ascending shard order (level 2), then the edge shard
    // inside _create_edge_locked (level 3).
    size_t const first = origin_shard < target_shard ? origin_shard : target_shard;
    size_t const second = origin_shard < target_shard ? target_shard : origin_shard;
    std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
    std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
    Vertex *from_vertex = _shards[origin_shard]._find(origin);
    Vertex *to_vertex = _shards[target_shard]._find(target);
    return _create_edge_locked(from_vertex, to_vertex, direction, labels, properties,
                               /*via_gdi_api=*/true, out_edge);
}

GdiStatus Graph::_free_edge_locked(EdgeId const &edge) {
    EdgeRecord record_copy;
    if (!_edges.record(edge, record_copy)) {
        return GdiStatus::error_uid;
    }
    kimix::unique_ptr<Edge> removed = _edges.erase(edge);
    if (!removed) {
        return GdiStatus::error_uid;
    }
    _removed_edges.push_back(edge);
    // Unlink both sides, one vertex shard at a time (ascending when they differ;
    // a self loop touches its single shard once).
    size_t const origin_shard = _shard_of(record_copy.origin);
    size_t const target_shard = _shard_of(record_copy.target);
    if (origin_shard == target_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[origin_shard].mutex);
        Vertex *origin = _shards[origin_shard]._find(record_copy.origin);
        Vertex *target = _shards[origin_shard]._find(record_copy.target);
        if (origin != nullptr) {
            origin->_remove_out_link(edge, target);
        }
        if (target != nullptr) {
            target->_remove_in_link(edge, origin);
        }
    } else {
        size_t const first = origin_shard < target_shard ? origin_shard : target_shard;
        size_t const second = origin_shard < target_shard ? target_shard : origin_shard;
        {
            std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
            std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
            Vertex *origin = _shards[origin_shard]._find(record_copy.origin);
            Vertex *target = _shards[target_shard]._find(record_copy.target);
            if (origin != nullptr) {
                origin->_remove_out_link(edge, target);
            }
            if (target != nullptr) {
                target->_remove_in_link(edge, origin);
            }
        }
    }
    _bump_structure(); // the target may be a root now
    return GdiStatus::ok;
}

GdiStatus Graph::edge_free(kimix::Guid const &edge) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    return _free_edge_locked(edge);
}

// One endpoint move, shared by edge_set_origin and edge_set_target. Everything
// happens under ONE ascending lock set: the shards of every vertex involved
// (level 2) and then the edge's own shard (level 3), so a reader can never see
// the record and the adjacency disagree.
GdiStatus Graph::_move_edge_endpoint(EdgeId const &edge, kimix::Guid const &endpoint,
                                     bool move_origin) {
    EdgeRecord record_copy;
    if (!_edges.record(edge, record_copy)) {
        return GdiStatus::error_uid;
    }
    kimix::Guid const new_origin = move_origin ? endpoint : record_copy.origin;
    kimix::Guid const new_target = move_origin ? record_copy.target : endpoint;
    if (new_origin == record_copy.origin && new_target == record_copy.target) {
        return GdiStatus::ok; // already there: no mutation, no dirty flag
    }

    // The distinct vertex shards this move touches, by ascending index.
    std::array<uint8_t, kGraphShardCount> touched{};
    touched[_shard_of(record_copy.origin)] = 1;
    touched[_shard_of(record_copy.target)] = 1;
    touched[_shard_of(new_origin)] = 1;
    touched[_shard_of(new_target)] = 1;
    std::array<std::unique_lock<std::shared_mutex>, kGraphShardCount> held;
    for (size_t shard = 0; shard < kGraphShardCount; ++shard) {
        if (touched[shard] != 0) {
            held[shard] = std::unique_lock<std::shared_mutex>(_shards[shard].mutex);
        }
    }
    Vertex *old_origin = _shards[_shard_of(record_copy.origin)]._find(record_copy.origin);
    Vertex *old_target = _shards[_shard_of(record_copy.target)]._find(record_copy.target);
    Vertex *origin_vertex = _shards[_shard_of(new_origin)]._find(new_origin);
    Vertex *target_vertex = _shards[_shard_of(new_target)]._find(new_target);
    if (origin_vertex == nullptr || target_vertex == nullptr) {
        // Endpoint mutation is validated against the live graph: GDI raises
        // ErrorVertex for a uid that does not exist, and nothing is moved.
        return GdiStatus::error_vertex;
    }

    // Level 3: the edge's own shard, taken AFTER every vertex shard.
    size_t const edge_shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> edge_lock(_edges.shard_mutex(edge_shard));
    Edge *record = _edges._find_locked(edge_shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }

    // Move the adjacency: an edge contributes exactly one out link (on its
    // origin) and one in link (on its target). Removing by edge id and re-adding
    // keeps parallel edges and self loops correct without special cases.
    if (old_origin != nullptr) {
        old_origin->_remove_out_link(edge, old_target);
    }
    if (old_target != nullptr) {
        old_target->_remove_in_link(edge, old_origin);
    }
    origin_vertex->_add_out_link(edge, target_vertex);
    target_vertex->_add_in_link(edge, origin_vertex);

    record->_origin = new_origin;
    record->_target = new_target;
    record->mark_dirty();
    _bump_structure(); // the new target gained an incoming edge (a former root may differ)
    return GdiStatus::ok;
}

GdiStatus Graph::edge_set_origin(kimix::Guid const &edge, kimix::Guid const &origin) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    return _move_edge_endpoint(edge, origin, /*move_origin=*/true);
}

GdiStatus Graph::edge_set_target(kimix::Guid const &edge, kimix::Guid const &target) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    return _move_edge_endpoint(edge, target, /*move_origin=*/false);
}

// ---------------------------------------------------------------------------
// Legacy edge API (connect / disconnect / connect_bulk), now implemented
// THROUGH the edge table so the two worlds cannot drift apart
// ---------------------------------------------------------------------------

bool Graph::_connect_locked(Shard &from_shard, Shard &to_shard,
                            kimix::Guid const &from, kimix::Guid const &to) {
    Vertex *from_vertex = from_shard._find(from);
    Vertex *to_vertex = to_shard._find(to);
    EdgeId created;
    // via_gdi_api = false: one directed edge per ordered pair, no self loop —
    // exactly the epoch-1..3 contract.
    return _create_edge_locked(from_vertex, to_vertex, EdgeDirectionType::directed, {}, nullptr,
                               /*via_gdi_api=*/false, created) == GdiStatus::ok;
}

bool Graph::_disconnect_locked(Shard &from_shard, Shard &to_shard,
                               kimix::Guid const &from, kimix::Guid const &to) {
    Vertex *from_vertex = from_shard._find(from);
    if (from_vertex == nullptr) {
        return false;
    }
    kimix::Guid const to_guid = to;
    for (auto const &link : from_vertex->_out_links) {
        if (link.neighbour != to_guid) {
            continue;
        }
        // The caller holds both endpoint shards UNIQUE, so the unlink below is
        // performed on the resolved vertices directly; _free_edge_locked would
        // re-acquire them (not recursive).
        EdgeId const edge = link.edge;
        kimix::unique_ptr<Edge> removed = _edges.erase(edge);
        if (!removed) {
            continue;
        }
        from_vertex->_remove_out_link(edge, to_shard._find(to_guid));
        Vertex *to_vertex = to_shard._find(to_guid);
        if (to_vertex != nullptr) {
            to_vertex->_remove_in_link(edge, from_vertex);
        }
        _bump_structure(); // `to` lost an incoming edge, so it may be a root now
        return true;
    }
    return false;
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
        std::unique_lock<std::shared_mutex> lock(_shards[from_shard].mutex);
        return _connect_locked(_shards[from_shard], _shards[from_shard], from, to);
    }
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

size_t Graph::connect_bulk(kimix::span<EdgeRequest const> edges,
                           kimix::span<EdgeOutcome> outcomes) noexcept {
    size_t const count = edges.size();
    bool const want_outcomes = outcomes.size() >= count;
    if (count == 0) {
        return 0;
    }
    if (!_check_writer()) {
        if (want_outcomes) {
            for (size_t i = 0; i < count; ++i) {
                outcomes[i] = EdgeOutcome::refused;
            }
        }
        return 0;
    }

    // Resolve every DISTINCT endpoint once (one shard shared lock per unique
    // endpoint instead of one Graph::find per edge). Ids stay valid under growth
    // because the dense table only moves its own values (epoch-3 lesson).
    struct Endpoint {
        Vertex *vertex;
        uint32_t shard;
    };
    kimix::vector<Endpoint> endpoints;
    endpoints.reserve(count / 4 + 16);
    kimix::unordered_map<kimix::Guid, uint32_t> resolved;
    resolved.reserve(count / 4 + 16);
    kimix::Guid last_guid{};
    uint32_t last_id = kNoEndpoint;
    auto resolve = [this, &endpoints, &resolved, &last_guid,
                    &last_id](kimix::Guid const &guid) -> uint32_t {
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
            return kNoEndpoint;
        }
        uint32_t const id = static_cast<uint32_t>(endpoints.size());
        endpoints.push_back(Endpoint{vertex, static_cast<uint32_t>(shard_index)});
        resolved.emplace(guid, id);
        last_guid = guid;
        last_id = id;
        return id;
    };

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

    size_t created = 0;
    for (size_t i = 0; i < count; ++i) {
        if (pending[i] == 0) {
            continue;
        }
        EdgeId edge_id;
        if (_create_edge_locked(endpoints[from_ends[i]].vertex,
                                endpoints[to_ends[i]].vertex,
                                EdgeDirectionType::directed, {}, nullptr,
                                /*via_gdi_api=*/false, edge_id) == GdiStatus::ok) {
            ++created;
        } else {
            verdicts[i] = EdgeOutcome::duplicate_edge; // already present (or refused)
        }
    }
    if (want_outcomes) {
        for (size_t i = 0; i < count; ++i) {
            outcomes[i] = verdicts[i];
        }
    }
    return created;
}

GdiStatus Graph::edge_set_direction(kimix::Guid const &edge, EdgeDirectionType direction) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    if (direction != EdgeDirectionType::directed &&
        direction != EdgeDirectionType::undirected) {
        return GdiStatus::error_edge_orientation;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    record->_direction = direction;
    record->mark_dirty();
    return GdiStatus::ok;
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
    // A full save rewrites the edge family too (they are separate records now).
    _edges.mark_dirty();
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
    _edges.clear_dirty();
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

// ---------------------------------------------------------------------------
// Loader-only adoption
// ---------------------------------------------------------------------------

GdiStatus Graph::adopt_vertex(kimix::unique_ptr<Vertex> vertex) {
    if (!vertex) {
        return GdiStatus::error_argument;
    }
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    kimix::Guid const guid = vertex->guid();
    bool const keyed = vertex->has_key();
    VertexKey const key = keyed ? vertex->key() : VertexKey{};
    Shard &shard = _shards[_shard_of(guid)];
    {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        if (!shard.vertices.try_emplace(guid, std::move(vertex)).second) {
            return GdiStatus::error_vertex;
        }
        _size.fetch_add(1, std::memory_order_relaxed);
        _bump_structure();
    }
    restore_removed_vertex(guid);
    if (keyed) {
        // Level 4, after the vertex shard is released. A store whose records
        // disagree about uniqueness leaves the vertex in place but unindexed:
        // the load counts it and carries on rather than dropping a vertex whose
        // edges are still resolvable.
        (void)_keys.insert(key, guid);
    }
    return GdiStatus::ok;
}

GdiStatus Graph::adopt_edge(kimix::unique_ptr<Edge> edge) {
    if (!edge) {
        return GdiStatus::error_argument;
    }
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    EdgeId const id = edge->id();
    kimix::Guid const origin = edge->origin();
    kimix::Guid const target = edge->target();
    if (origin == target && !_options.allow_self_edges) {
        return GdiStatus::error_edge;
    }
    if (!_edges.insert(std::move(edge))) {
        return GdiStatus::error_vertex; // a duplicate edge uid: treated as a bad record
    }
    restore_removed_edge(id);
    // Link both sides, one ascending lock set for the two (or one) endpoint
    // shards. The endpoints were inserted by the vertex pass, so a missing one is
    // a dangling record, not a race.
    size_t const origin_shard = _shard_of(origin);
    size_t const target_shard = _shard_of(target);
    if (origin_shard == target_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[origin_shard].mutex);
        Vertex *origin_vertex = _shards[origin_shard]._find(origin);
        Vertex *target_vertex = _shards[origin_shard]._find(target);
        if (origin_vertex == nullptr || target_vertex == nullptr) {
            _edges.erase(id);
            return GdiStatus::error_vertex;
        }
        origin_vertex->_add_out_link(id, target_vertex);
        target_vertex->_add_in_link(id, origin_vertex);
    } else {
        size_t const first = origin_shard < target_shard ? origin_shard : target_shard;
        size_t const second = origin_shard < target_shard ? target_shard : origin_shard;
        std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
        std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
        Vertex *origin_vertex = _shards[origin_shard]._find(origin);
        Vertex *target_vertex = _shards[target_shard]._find(target);
        if (origin_vertex == nullptr || target_vertex == nullptr) {
            _edges.erase(id);
            return GdiStatus::error_vertex;
        }
        origin_vertex->_add_out_link(id, target_vertex);
        target_vertex->_add_in_link(id, origin_vertex);
    }
    _bump_structure();
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Edge payload (labels / properties): guarded by the edge's OWN SHARD
// ---------------------------------------------------------------------------

namespace {

// Build a PropertyList copy of a bag, so the edge read-back and the vertex
// read-back return the same shape.
void copy_bag(PropertyBag const &bag, PropertyList &out) {
    out.clear();
    out.reserve(bag.size());
    for (auto const &entry : bag) {
        PropertyEntry item;
        item.name = entry.first;
        item.value = entry.second;
        out.push_back(std::move(item));
    }
}

} // namespace

GdiStatus Graph::edge_add_label(kimix::Guid const &edge, LabelId label) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    if (label == kNoLabel) {
        return GdiStatus::error_label;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    if (std::find(record->_labels.begin(), record->_labels.end(), label) !=
        record->_labels.end()) {
        return GdiStatus::error_label; // already present (GDI AddLabel semantics)
    }
    record->_labels.push_back(label);
    record->mark_dirty();
    return GdiStatus::ok;
}

GdiStatus Graph::edge_remove_label(kimix::Guid const &edge, LabelId label) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    auto it = std::find(record->_labels.begin(), record->_labels.end(), label);
    if (it == record->_labels.end()) {
        return GdiStatus::error_label; // absent (GDI RemoveLabel semantics)
    }
    record->_labels.erase(it);
    record->mark_dirty();
    return GdiStatus::ok;
}

bool Graph::edge_labels(kimix::Guid const &edge, kimix::vector<LabelId> &out) const {
    return _edges.labels(edge, out);
}

GdiStatus Graph::edge_add_property(kimix::Guid const &edge, PropNameId name,
                                   PropertyValue value) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    if (record->_properties.find(name) != record->_properties.end()) {
        return GdiStatus::error_property_exists;
    }
    record->_properties.emplace(name, std::move(value));
    record->mark_dirty();
    return GdiStatus::ok;
}

GdiStatus Graph::edge_set_property(kimix::Guid const &edge, PropNameId name,
                                   PropertyValue value) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    auto it = record->_properties.find(name);
    if (it == record->_properties.end()) {
        return GdiStatus::error_no_property;
    }
    it->second = std::move(value);
    record->mark_dirty();
    return GdiStatus::ok;
}

GdiStatus Graph::edge_put_property(kimix::Guid const &edge, PropNameId name,
                                   PropertyValue value) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    auto it = record->_properties.find(name);
    if (it != record->_properties.end()) {
        it->second = std::move(value);
    } else {
        record->_properties.emplace(name, std::move(value));
    }
    record->mark_dirty();
    return GdiStatus::ok;
}

GdiStatus Graph::edge_remove_properties(kimix::Guid const &edge,
                                        kimix::span<PropNameId const> names) {
    if (!_check_writer()) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    for (auto name : names) {
        // Removing an absent property is not an error (GDI RemoveProperties).
        record->_properties.erase(name);
    }
    record->mark_dirty();
    return GdiStatus::ok;
}

bool Graph::edge_properties(kimix::Guid const &edge, PropertyList &out) const {
    size_t const shard = EdgeTable::shard_of(edge);
    std::shared_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge const *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return false;
    }
    copy_bag(record->_properties, out);
    return true;
}

bool Graph::edge_property(kimix::Guid const &edge, PropNameId name,
                          PropertyValue &out) const {
    return _edges.property(edge, name, out);
}

bool Graph::edge_record(kimix::Guid const &edge, EdgeRecord &out) const {
    return _edges.record(edge, out);
}

bool Graph::edge_endpoints(kimix::Guid const &edge, kimix::Guid &origin,
                           kimix::Guid &target) const {
    return _edges.endpoints(edge, origin, target);
}

void Graph::incident_edges(kimix::Guid const &vertex, EdgeOrientation orientation,
                           kimix::vector<EdgeId> &out) const {
    out.clear();
    Shard const &shard = _shards[_shard_of(vertex)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex const *record = shard._find(vertex);
    if (record == nullptr) {
        return;
    }
    // One list scan per requested direction: the link list already IS the
    // orientation, which is why an oriented read needs no incidence check.
    if (orientation == EdgeOrientation::outgoing || orientation == EdgeOrientation::both) {
        out.reserve(record->_out_links.size() + record->_in_links.size());
        for (auto const &link : record->_out_links) {
            out.push_back(link.edge);
        }
    }
    if (orientation == EdgeOrientation::incoming || orientation == EdgeOrientation::both) {
        out.reserve(record->_out_links.size() + record->_in_links.size());
        for (auto const &link : record->_in_links) {
            out.push_back(link.edge);
        }
    }
}

bool Graph::has_edge(kimix::Guid const &origin, kimix::Guid const &target) const {
    Shard const &shard = _shards[_shard_of(origin)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex const *record = shard._find(origin);
    if (record == nullptr) {
        return false;
    }
    return _has_out_edge_locked(record, target);
}

bool Graph::has_incident_edges(kimix::Guid const &vertex, EdgeOrientation orientation) const {
    Shard const &shard = _shards[_shard_of(vertex)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex const *record = shard._find(vertex);
    if (record == nullptr) {
        return false;
    }
    if ((orientation == EdgeOrientation::outgoing || orientation == EdgeOrientation::both) &&
        !record->_out_links.empty()) {
        return true;
    }
    if ((orientation == EdgeOrientation::incoming || orientation == EdgeOrientation::both) &&
        !record->_in_links.empty()) {
        return true;
    }
    return false;
}

kimix::vector<kimix::Guid> Graph::take_removed_vertices() {
    kimix::vector<kimix::Guid> out;
    out.swap(_removed_vertices);
    return out;
}

kimix::vector<EdgeId> Graph::take_removed_edges() {
    kimix::vector<EdgeId> out;
    out.swap(_removed_edges);
    return out;
}

void Graph::restore_removed_vertex(kimix::Guid const &guid) {
    auto it = std::find(_removed_vertices.begin(), _removed_vertices.end(), guid);
    if (it != _removed_vertices.end()) {
        _removed_vertices.erase(it);
    }
}

void Graph::restore_removed_edge(EdgeId const &edge) {
    auto it = std::find(_removed_edges.begin(), _removed_edges.end(), edge);
    if (it != _removed_edges.end()) {
        _removed_edges.erase(it);
    }
}

kimix::vector<kimix::Guid> Graph::vertex_guids() const {
    kimix::vector<kimix::Guid> out;
    for (auto const &shard : _shards) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        out.reserve(out.size() + shard.vertices.size());
        for (auto const &entry : shard.vertices) {
            out.push_back(entry.first);
        }
    }
    return out;
}

kimix::vector<EdgeId> Graph::edge_ids() const {
    kimix::vector<EdgeId> out;
    for (size_t shard_index = 0; shard_index < kEdgeShardCount; ++shard_index) {
        std::shared_lock<std::shared_mutex> lock(_edges.shard_mutex(shard_index));
        _edges.ids_of_shard(shard_index, out);
    }
    return out;
}

void Graph::edge_restore_payload(EdgeId const &edge, kimix::span<const LabelId> labels,
                                 PropertyList const &properties) {
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return;
    }
    record->_labels.clear();
    for (auto label : labels) {
        record->_labels.push_back(label);
    }
    record->_properties.clear();
    for (auto const &entry : properties) {
        record->_properties.emplace(entry.name, entry.value);
    }
    record->mark_dirty();
}

kimix::vector<EdgeRecord> Graph::dirty_edges() const {
    return _edges.dirty_edges();
}

bool Graph::link_counts(kimix::Guid const &guid, size_t &outgoing, size_t &incoming) const {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex const *record = shard._find(guid);
    if (record == nullptr) {
        return false;
    }
    outgoing = record->_out_links.size();
    incoming = record->_in_links.size();
    return true;
}

} // namespace kimix::graph_db