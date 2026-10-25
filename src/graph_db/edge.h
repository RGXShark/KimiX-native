/*
 * edge.h — first-class edges (GDI §2.4 / §2.14).
 *
 * Before this header an edge WAS its two endpoints: a vertex kept a
 * unordered_set<Vertex*> plus a guid vector, and "an edge" was the fact that
 * both lists contained each other. That cannot express what GDI needs — a stable
 * edge uid, a per-edge direction, labels, properties, endpoint mutation, parallel
 * edges between the same ordered pair — so an edge is now an object:
 *
 *   Edge        the record: uid, origin, target, direction, labels, properties,
 *               dirty flag.
 *   EdgeTable   the sharded uid -> Edge map (same idiom as Graph's vertex map).
 *   EdgeLink    one entry of a vertex's adjacency: (edge uid, neighbour uid).
 *   EdgeRecord  the plain read-back a GDI read returns (uid + endpoints +
 *               direction), copyable and independent of any lock.
 *
 * Both adjacency directions are kept on the vertex ({edge, neighbour} in
 * _out_links / _in_links), so an oriented read is a scan of ONE list and needs
 * no second lookup to decide incidence.
 *
 * LOCKING (see README.md for the full hierarchy)
 *   An Edge's fields and payload are guarded by its OWN SHARD (the EdgeTable
 *   shard of its uid), NOT by a per-edge mutex: an edge record is small and a
 *   mutex would cost a cache line per edge on a multi-million-edge store. The
 *   consequence is documented and accepted — two writes to two different edges
 *   that land in the same shard serialize on that shard's lock — because every
 *   structural write is single-writer by contract anyway; only READS run
 *   concurrently, and a read takes the shard shared.
 *   Endpoint mutation takes the affected vertex shards AND the edge shard, in
 *   the hierarchy's order (vertex shards ascending, then edge shards), which is
 *   what keeps the adjacency lists consistent with the record.
 *
 * MULTI-EDGES: create_edge allows several edges between one ordered pair (and a
 * self-loop, both options-gated). Reachability is unaffected: a vertex's
 * neighbour-dedup sets are maintained separately, so has_path/shortest_path still
 * see each neighbour once.
 *
 * Example:
 *   kimix::graph_db::EdgeTable edges;
 *   kimix::graph_db::EdgeId id{true};
 *   edges.create(id, origin_guid, target_guid,
 *                kimix::graph_db::EdgeDirectionType::directed);
 *   kimix::graph_db::EdgeRecord record;
 *   edges.record(id, record);            // copy under the shard's shared lock
 */
#pragma once

#include "graph_db_api.h"
#include "binary_le.h"
#include "property.h"
#include "status.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/stl/memory.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <array>
#include <atomic> // std::atomic<size_t> _size: MSVC pulls it in transitively, gcc does not
#include <cstddef>
#include <cstdint>
#include <shared_mutex>

namespace kimix::graph_db {

// A stable edge identity: a 16-byte guid, exactly like a vertex uid, and it
// survives save/load verbatim (which is what makes GDI's
// SupportsEdgeByStableUid honest).
using EdgeId = kimix::Guid;

// GDI_EDGE_DIRECTED / GDI_EDGE_UNDIRECTED (gdi.h: 259 / 260).
enum class EdgeDirectionType : uint8_t {
    directed = 0,
    undirected = 1,
};

// Read orientation of a neighborhood query (GDI_GetEdgesOfVertex). Deliberately a
// DIFFERENT type from EdgeDirectionType, like the C# side, so mixing up a write
// direction and a read orientation is a compile error.
enum class EdgeOrientation : uint8_t {
    outgoing = 0,
    incoming = 1,
    both = 2,
};

[[nodiscard]] KIMIX_GRAPH_DB_API char const *edge_direction_name(EdgeDirectionType direction) noexcept;
[[nodiscard]] KIMIX_GRAPH_DB_API char const *edge_orientation_name(EdgeOrientation orientation) noexcept;

// Shards of the edge table (independent constant; graph.h static_asserts it
// matches the vertex shard count so the lock-order story stays one story).
inline constexpr size_t kEdgeShardCount = 64;
static_assert((kEdgeShardCount & (kEdgeShardCount - 1)) == 0,
              "kEdgeShardCount must be a power of two (the shard index is a mask)");

/*
 * One adjacency entry of a vertex: the edge's uid plus the vertex at its other
 * end. 32 bytes, which is what the adjacency of a vertex costs per incident edge
 * (the epoch-2 guid list was 16) — measured and recorded in README.md.
 */
struct KIMIX_GRAPH_DB_API EdgeLink {
    EdgeId edge;
    kimix::Guid neighbour;

    [[nodiscard]] bool operator==(EdgeLink const &other) const noexcept {
        return edge == other.edge && neighbour == other.neighbour;
    }
};

/*
 * The read-back form of an edge: what GDI's GdiEdgeRecord carries (uid, both
 * endpoints, direction) and nothing the reader has to lock for. A copy is
 * independent of the table's state.
 */
struct KIMIX_GRAPH_DB_API EdgeRecord {
    EdgeId id;
    kimix::Guid origin;
    kimix::Guid target;
    EdgeDirectionType direction{EdgeDirectionType::directed};
    kimix::vector<LabelId> labels;
    PropertyList properties;

    [[nodiscard]] bool is_self_loop() const noexcept { return origin == target; }
};

/*
 * One edge. Fields are public to Graph/EdgeTable (which own the locking); there
 * is no meaningful "partially valid" state, so there is no setter soup either.
 */
class KIMIX_GRAPH_DB_API Edge {
public:
    Edge() noexcept = default;
    Edge(EdgeId id, kimix::Guid origin, kimix::Guid target,
         EdgeDirectionType direction) noexcept
        : _id(id), _origin(origin), _target(target), _direction(direction) {
    }
    Edge(Edge const &) = delete;
    Edge &operator=(Edge const &) = delete;
    Edge(Edge &&) = delete;
    Edge &operator=(Edge &&) = delete;
    ~Edge() noexcept = default;

    [[nodiscard]] EdgeId const &id() const noexcept { return _id; }
    [[nodiscard]] kimix::Guid const &origin() const noexcept { return _origin; }
    [[nodiscard]] kimix::Guid const &target() const noexcept { return _target; }
    [[nodiscard]] EdgeDirectionType direction() const noexcept { return _direction; }
    [[nodiscard]] bool is_dirty() const noexcept { return _dirty; }
    void mark_dirty() noexcept { _dirty = true; }
    void clear_dirty() noexcept { _dirty = false; }

    // Payload assignment for an edge that is not published yet (the loader and
    // Graph::edge_create fill a fresh edge through this). Same contract as
    // Vertex::assign_payload: never on an edge the table already holds.
    void assign_payload(kimix::span<const LabelId> labels, PropertyList const &properties) {
        _labels.clear();
        for (auto label : labels) {
            if (label != kNoLabel) {
                _labels.push_back(label);
            }
        }
        _properties.clear();
        for (auto const &entry : properties) {
            _properties.emplace(entry.name, entry.value);
        }
    }

    // A "neighbour" of `vertex`: the other endpoint. `vertex` must be one of the
    // two endpoints (the callers only ask that way).
    [[nodiscard]] kimix::Guid neighbour_of(kimix::Guid const &vertex) const noexcept {
        return _origin == vertex ? _target : _origin;
    }

private:
    friend class EdgeTable;
    friend class Graph;

    EdgeId _id;
    kimix::Guid _origin;
    kimix::Guid _target;
    EdgeDirectionType _direction{EdgeDirectionType::directed};
    kimix::vector<LabelId> _labels;
    PropertyBag _properties;
    bool _dirty{true};
};

/*
 * The sharded edge store. Public methods take exactly one shard lock and copy
 * what they return; the mutating entry points that need several locks (an
 * endpoint move) live on Graph, which owns the vertex shards too.
 */
class KIMIX_GRAPH_DB_API EdgeTable : public kimix::IOperatorNewBase {
public:
    EdgeTable() noexcept = default;
    EdgeTable(EdgeTable const &) = delete;
    EdgeTable &operator=(EdgeTable const &) = delete;
    EdgeTable(EdgeTable &&) = delete;
    EdgeTable &operator=(EdgeTable &&) = delete;
    ~EdgeTable() noexcept;

    // Insert `edge` (adopting it). False when the uid is already present (an
    // astronomically unlikely collision, reported instead of corrupting the map).
    bool insert(kimix::unique_ptr<Edge> edge);
    // Remove an edge, returning it to the caller (the transaction/journal path
    // needs to resurrect it). nullptr when the uid is unknown.
    kimix::unique_ptr<Edge> erase(EdgeId const &id);
    // Resolve a uid to its (still owned) Edge. The pointer is valid until the
    // edge is erased; taking it does not keep it alive.
    [[nodiscard]] Edge *find(EdgeId const &id) const;
    [[nodiscard]] bool contains(EdgeId const &id) const;
    [[nodiscard]] size_t size() const noexcept { return _size.load(std::memory_order_relaxed); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // --- reads that copy (one shard shared lock each) ------------------------

    // Full record: uid, endpoints, direction, labels, properties.
    [[nodiscard]] bool record(EdgeId const &id, EdgeRecord &out) const;
    // Endpoints + direction only (cheaper: no payload copy).
    [[nodiscard]] bool endpoints(EdgeId const &id, kimix::Guid &origin,
                                 kimix::Guid &target) const;
    [[nodiscard]] bool direction(EdgeId const &id, EdgeDirectionType &out) const;
    [[nodiscard]] bool labels(EdgeId const &id, kimix::vector<LabelId> &out) const;
    [[nodiscard]] bool properties(EdgeId const &id, PropertyList &out) const;
    [[nodiscard]] bool has_label(EdgeId const &id, LabelId label) const;
    [[nodiscard]] bool property(EdgeId const &id, PropNameId name, PropertyValue &out) const;

    // --- dirty bookkeeping (the incremental serializer) ----------------------

    // Every dirty edge record, copied out (shard by shard, one shared lock at a
    // time). Order: shard index, then map insertion order — deterministic.
    [[nodiscard]] kimix::vector<EdgeRecord> dirty_edges() const;
    void mark_dirty() noexcept;
    void clear_dirty() noexcept;
    // Clear the flag of exactly `id`, if it is still dirty. Used by the save path
    // after a successful transaction, mirroring the vertex rule ("only the
    // records this call wrote stop being dirty").
    void clear_dirty(EdgeId const &id) noexcept;

    // Reserve capacity before a bulk load (per-shard; only ever raises).
    void reserve(size_t entries);

    // Shard plumbing, exposed for Graph (which takes several locks at once) and
    // for the lock-order assertions in the tests. `shard_mutex` is LEVEL 3.
    [[nodiscard]] static size_t shard_of(EdgeId const &id) noexcept;
    [[nodiscard]] std::shared_mutex &shard_mutex(size_t shard) const noexcept {
        return _shards[shard].mutex;
    }

private:
    friend class Graph;

    struct Shard {
        Shard() noexcept = default;
        Shard(Shard const &) = delete;
        Shard &operator=(Shard const &) = delete;
        Shard(Shard &&) = delete;
        Shard &operator=(Shard &&) = delete;

        alignas(64) mutable std::shared_mutex mutex;
        kimix::unordered_map<EdgeId, kimix::unique_ptr<Edge>> edges;
    };

    // Lookup under the already-held shard lock (shared or unique).
    [[nodiscard]] Edge *_find_locked(size_t shard, EdgeId const &id) const noexcept;
    // Append one shard's ids (caller holds that shard's lock).
    void ids_of_shard(size_t shard, kimix::vector<EdgeId> &out) const;

    std::array<Shard, kEdgeShardCount> _shards;
    std::atomic<size_t> _size{0};
};

// ---------------------------------------------------------------------------
// Edge record codec ("e/" + edge uid -> "GDE2" record)
// ---------------------------------------------------------------------------

// Bytes an encoded edge record occupies.
[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_edge_record_size(EdgeRecord const &record) noexcept;
KIMIX_GRAPH_DB_API void encode_edge_record(EdgeRecord const &record,
                                           kimix::vector<std::byte> &out);
// Total decoder: a truncated / wrong-magic / impossible-count buffer is a
// `false`, never an out-of-bounds read.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_edge_record(kimix::span<const std::byte> bytes, EdgeRecord &out) noexcept;

} // namespace kimix::graph_db
