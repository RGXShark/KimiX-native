/*
 * database.h — kimix::graph_db::Database: the GDI-shaped backend facade.
 *
 * One Database owns one in-memory Graph, one Schema, one ColdStorage and one
 * GraphOptions set, and exposes the GDI command vocabulary as
 * GdiStatus-returning methods. It is the type a Semevia.Gdi.Native adapter binds
 * to (the C ABI in graph_c_api.h is a thin wrapper over exactly this class), and
 * it is what the report's §2.16 "session / multi-graph object" asks for: a named
 * graph catalog slots in here later without changing this surface.
 *
 * WHY A SEPARATE FACADE: Graph stays the container (sharded vertex map, edge
 * table, key index, traversals, its own lock contract). Everything that needs
 * the SCHEMA — name interning, declaration checks, error semantics, the
 * (label, external_id) uniqueness policy, filtered reads, transactions — lives
 * here, because it is policy, not storage. That keeps graph.h reviewable and
 * makes the GDI error surface exist in exactly one place.
 *
 * IDs VS NAMES: the entity-level commands take INTERNED IDS (LabelId /
 * PropNameId) because that is what an entity stores and it keeps a write
 * allocation-free; names are resolved once, through the schema
 * (Schema::resolve_label / resolve_property_type), by whoever has the names in
 * hand — the C ABI does, so the managed side keeps speaking names. Read-backs
 * return NAMES (the schema owns the name table), which is what a caller sees.
 *
 * ERROR SEMANTICS are copied from Semevia.Gdi's in-memory reference backend
 * (Backends/InMemory/InMemoryGdiBackend.cs), one method at a time:
 *   create_vertex    ErrorEmptyName / ErrorNonUniqueId (or WarningNonUniqueId +
 *                    the existing uid in warn mode) / ErrorLabel / ErrorPropertyType
 *   get_or_create    match returns the uid UNTOUCHED (created=false)
 *   translate        ok + found=false for a miss (never an error)
 *   free_vertex      ErrorVertex when unknown; guarded also ErrorEdge
 *   add_property     ErrorPropertyExists / ErrorPropertyType / ErrorWrongType
 *   set_property     ErrorNoProperty (+ the same type checks)
 *   update_property  UPSERT, no error for a miss
 *   remove_props     absent names ignored
 *   add_label        ErrorLabel when already present or undeclared
 *   remove_label     ErrorLabel when absent
 *   create_edge      ErrorVertex / ErrorEdge (self-loop when forbidden)
 *   get_edges        ErrorArgument for cap < 0; cap == 0 is the count probe
 *   transactions     ErrorTransaction / ErrorReadOnlyTransaction / ErrorSizeLimit
 *
 * PERSISTENCE is the save/dirty model of serialize.h, not a per-commit fsync:
 * persist(full=false) writes exactly the dirty records. Transaction commit only
 * persists when GraphOptions::persist_on_commit is set (a GDI caller that wants
 * durability per commit asks for it explicitly).
 *
 * Example:
 *   kimix::graph_db::Database db;
 *   db.open("graph.cold", kimix::graph_db::GraphOptions{});
 *   kimix::graph_db::LabelId person{};
 *   db.schema().create_label("person", person);
 *   kimix::graph_db::Database::CreateResult created;
 *   db.create_vertex(kimix::graph_db::VertexKeyView{person, id_bytes}, {}, nullptr, created);
 *   db.persist(false);   // incremental save
 */
#pragma once

#include "capability.h"
#include "edge.h"
#include "graph.h"
#include "graph_db_api.h"
#include "graph_db_options.h"
#include "key.h"
#include "property.h"
#include "query.h"
#include "schema.h"
#include "serialize.h"
#include "status.h"
#include "utils.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/stl/filesystem.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

class Transaction;

// One typed property with its NAME resolved (what a read-back returns, and what
// a write takes when the caller already has the name).
struct KIMIX_GRAPH_DB_API NamedProperty {
    kimix::string name;
    PropertyValue value;
};
using NamedPropertyList = kimix::vector<NamedProperty>;

// Result of create_vertex: GDI's GdiVertexCreateResult.
struct KIMIX_GRAPH_DB_API VertexCreateResult {
    kimix::Guid uid;
    // GDI's NonUniqueIdWarning: set only on the warning_non_unique_id path, where
    // `uid` is the EXISTING vertex (GraphOptions::key_mode == report_warning).
    bool non_unique_id_warning{false};
};

// Result of get_or_create_vertex: GDI's GdiVertexMergeResult.
struct KIMIX_GRAPH_DB_API VertexMergeResult {
    kimix::Guid uid;
    bool created{false};
};

// One row of create_vertices_bulk. The key owns its id bytes, so a caller may
// build the rows and let the values go out of scope.
struct KIMIX_GRAPH_DB_API VertexBulkRow {
    VertexKey key;
    kimix::vector<LabelId> labels;
    PropertyList properties;
    kimix::span<const std::byte> content;
};

// One row of create_edges_bulk.
struct KIMIX_GRAPH_DB_API EdgeBulkRow {
    kimix::Guid origin;
    kimix::Guid target;
    EdgeDirectionType direction{EdgeDirectionType::directed};
    kimix::vector<LabelId> labels;
    PropertyList properties;
};

// What a bulk call did (GDI's failed_rows / counts in one value).
struct KIMIX_GRAPH_DB_API BulkStats {
    size_t created{0};
    size_t failed{0};
    size_t warning{0};
};

// A label / property type as a caller sees it (name + its interned id).
struct KIMIX_GRAPH_DB_API NamedLabel {
    LabelId id{kNoLabel};
    kimix::string name;
};
using NamedLabelList = kimix::vector<NamedLabel>;

/*
 * The GDI backend. Single-writer by contract (Graph's rule, surfaced as
 * error_state on a refused write); any number of threads may read concurrently.
 * One Database per engine worker, like Semevia.Gdi's IGraphBackend.
 */
class KIMIX_GRAPH_DB_API Database : public kimix::IOperatorNewBase {
public:
    Database() noexcept;
    explicit Database(GraphOptions const &options) noexcept;
    Database(Database const &) = delete;
    Database &operator=(Database const &) = delete;
    Database(Database &&) = delete;
    Database &operator=(Database &&) = delete;
    ~Database() noexcept;

    // --- store lifecycle ----------------------------------------------------
    // Open (and create) the cold store in `dir` and load what it holds into this
    // database's graph. An empty path keeps the database purely in memory
    // (persist() then returns error_no_such_file). error_bad_file when the store
    // announces a newer format, error_state when it is already open.
    [[nodiscard]] GdiStatus open(kimix::filesystem::path const &dir,
                                 GraphOptions const &options);
    [[nodiscard]] GdiStatus open(kimix::filesystem::path const &dir);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return _cold.valid(); }
    // Write the graph into the store: full (every record) or incremental (only
    // the dirty ones). error_io when no store is open.
    [[nodiscard]] GdiStatus persist(bool full, GraphSerializeStats *out = nullptr);
    // Re-read the store into the (already open) database.
    [[nodiscard]] GdiStatus load_store(GraphSerializeStats *out = nullptr);
    [[nodiscard]] GraphSerializeStats const &last_stats() const noexcept { return _last_stats; }

    [[nodiscard]] Graph &graph() noexcept { return _graph; }
    [[nodiscard]] Graph const &graph() const noexcept { return _graph; }
    [[nodiscard]] Schema &schema() noexcept { return _schema; }
    [[nodiscard]] Schema const &schema() const noexcept { return _schema; }
    [[nodiscard]] ColdStorage &storage() noexcept { return _cold; }
    [[nodiscard]] GraphOptions const &options() const noexcept { return _options; }
    void set_options(GraphOptions const &options);
    [[nodiscard]] Capability capabilities() const noexcept { return current_capability(); }

    // --- vertices (§2.3, §2.8, §2.10) ---------------------------------------
    // Create a keyed (or, with key_label == kNoLabel, an unkeyed) vertex.
    // `labels` and `properties` are interned ids; `key_label` must be registered.
    [[nodiscard]] GdiStatus create_vertex(VertexKeyView key, kimix::span<const LabelId> labels,
                                          PropertyList const *properties,
                                          VertexCreateResult &out);
    [[nodiscard]] GdiStatus get_or_create_vertex(VertexKeyView key,
                                                 kimix::span<const LabelId> labels,
                                                 PropertyList const *properties,
                                                 VertexMergeResult &out);
    // Key -> uid. ok + found=false for a miss (never an error).
    [[nodiscard]] GdiStatus translate_vertex_id(VertexKeyView key, kimix::Guid &out_uid,
                                                bool &found) const;
    // Cascading delete (GDI FreeVertex): every incident edge goes with it.
    [[nodiscard]] GdiStatus free_vertex(kimix::Guid const &uid);
    // Guarded delete (GDI FreeVertexGuarded): error_edge while any edge exists.
    [[nodiscard]] GdiStatus free_vertex_guarded(kimix::Guid const &uid);
    // N keyed creates in ONE transaction. continue_on_error=false makes the batch
    // all-or-nothing (the first failure aborts it and the remaining rows are
    // marked error_state); true isolates the rows (per-row statuses, matching
    // CreateVerticesBulkAsync + failed_rows).
    [[nodiscard]] GdiStatus create_vertices_bulk(kimix::span<VertexBulkRow const> rows,
                                                bool continue_on_error,
                                                kimix::vector<VertexCreateResult> &results,
                                                kimix::vector<GdiStatus> *row_status,
                                                BulkStats &stats);
    [[nodiscard]] GdiStatus create_edges_bulk(kimix::span<EdgeBulkRow const> rows,
                                              bool continue_on_error,
                                              kimix::vector<kimix::Guid> &edge_ids,
                                              kimix::vector<GdiStatus> *row_status,
                                              BulkStats &stats);

    // --- vertex labels (§2.2) ----------------------------------------------
    [[nodiscard]] GdiStatus add_label(kimix::Guid const &uid, LabelId label);
    [[nodiscard]] GdiStatus remove_label(kimix::Guid const &uid, LabelId label);
    // Labels of a vertex, as NAMES: the key label first, then the rest (the
    // reference backend's GetLabelsOfVertex order).
    [[nodiscard]] GdiStatus get_labels(kimix::Guid const &uid,
                                       kimix::vector<kimix::string> &out) const;

    // --- vertex properties (§2.1) ------------------------------------------
    [[nodiscard]] GdiStatus add_property(kimix::Guid const &uid, PropNameId name,
                                         PropertyValue value);
    [[nodiscard]] GdiStatus set_property(kimix::Guid const &uid, PropNameId name,
                                         PropertyValue value);
    [[nodiscard]] GdiStatus update_property(kimix::Guid const &uid, PropNameId name,
                                            PropertyValue value);
    [[nodiscard]] GdiStatus remove_properties(kimix::Guid const &uid,
                                              kimix::span<const PropNameId> names);
    [[nodiscard]] GdiStatus get_properties(kimix::Guid const &uid,
                                           NamedPropertyList &out) const;
    // The declarations of the properties a vertex actually carries, sorted by name.
    [[nodiscard]] GdiStatus get_property_types(kimix::Guid const &uid,
                                               PropertyTypeList &out) const;

    // --- edges (§2.4, §2.14) ------------------------------------------------
    [[nodiscard]] GdiStatus edge_create(kimix::Guid const &origin, kimix::Guid const &target,
                                        EdgeDirectionType direction,
                                        kimix::span<const LabelId> labels,
                                        PropertyList const *properties, kimix::Guid &out_edge);
    [[nodiscard]] GdiStatus edge_free(kimix::Guid const &edge);
    [[nodiscard]] GdiStatus set_edge_origin(kimix::Guid const &edge, kimix::Guid const &origin);
    [[nodiscard]] GdiStatus set_edge_target(kimix::Guid const &edge, kimix::Guid const &target);
    [[nodiscard]] GdiStatus set_edge_direction(kimix::Guid const &edge,
                                               EdgeDirectionType direction);
    [[nodiscard]] GdiStatus get_edge_endpoints(kimix::Guid const &edge, kimix::Guid &origin,
                                               kimix::Guid &target) const;
    [[nodiscard]] GdiStatus add_edge_label(kimix::Guid const &edge, LabelId label);
    [[nodiscard]] GdiStatus remove_edge_label(kimix::Guid const &edge, LabelId label);
    [[nodiscard]] GdiStatus get_edge_labels(kimix::Guid const &edge,
                                            kimix::vector<kimix::string> &out) const;
    [[nodiscard]] GdiStatus add_edge_property(kimix::Guid const &edge, PropNameId name,
                                              PropertyValue value);
    [[nodiscard]] GdiStatus set_edge_property(kimix::Guid const &edge, PropNameId name,
                                              PropertyValue value);
    [[nodiscard]] GdiStatus update_edge_property(kimix::Guid const &edge, PropNameId name,
                                                 PropertyValue value);
    [[nodiscard]] GdiStatus remove_edge_properties(kimix::Guid const &edge,
                                                   kimix::span<const PropNameId> names);
    [[nodiscard]] GdiStatus get_edge_properties(kimix::Guid const &edge,
                                                NamedPropertyList &out) const;
    [[nodiscard]] GdiStatus get_edge_property_types(kimix::Guid const &edge,
                                                    PropertyTypeList &out) const;

    // --- filtered, oriented, paged reads (§2.6) -----------------------------
    // `constraint` may be an empty Constraint (matches everything) or a registered
    // handle. Edge reads filter the EDGES (their labels/properties); neighbour
    // reads filter the NEIGHBOUR VERTICES (GDI §5.10). cap == 0 is the count
    // probe: no items, the real total_count. cap < 0 is error_argument.
    [[nodiscard]] GdiStatus get_edges_of_vertex(kimix::Guid const &uid,
                                                Constraint const &constraint,
                                                EdgeOrientation orientation, int64_t cap,
                                                EdgePage &out) const;
    [[nodiscard]] GdiStatus get_neighbor_vertices(kimix::Guid const &uid,
                                                  Constraint const &constraint,
                                                  EdgeOrientation orientation, int64_t cap,
                                                  VertexPage &out) const;
    // Exact count (no cap). ExistsNeighbor == count_neighbors(...) > 0 with a
    // cap-1 short circuit.
    [[nodiscard]] GdiStatus count_neighbors(kimix::Guid const &uid,
                                            Constraint const &constraint,
                                            EdgeOrientation orientation, size_t &out) const;
    [[nodiscard]] GdiStatus exists_neighbor(kimix::Guid const &uid,
                                            Constraint const &constraint,
                                            EdgeOrientation orientation, bool &out) const;

    // --- constraints (§2.5) -------------------------------------------------
    [[nodiscard]] GdiStatus create_constraint(Constraint const &definition,
                                              ConstraintHandle &out);
    [[nodiscard]] GdiStatus add_label_condition(ConstraintHandle handle, LabelId label, GdiOp op);
    [[nodiscard]] GdiStatus add_property_condition(ConstraintHandle handle, PropNameId property,
                                                   GdiOp op, PropertyValue value,
                                                   uint32_t count = 1);
    [[nodiscard]] GdiStatus free_constraint(ConstraintHandle handle);

    // --- schema commands (§2.5) --------------------------------------------
    [[nodiscard]] GdiStatus create_label(kimix::string_view name, LabelId &out);
    [[nodiscard]] GdiStatus get_label_by_name(kimix::string_view name, LabelId &out,
                                              bool &found) const;
    [[nodiscard]] GdiStatus get_all_labels(NamedLabelList &out) const;
    // Rename. Entities are untouched (they store ids), so this is O(1).
    [[nodiscard]] GdiStatus update_label(LabelId id, kimix::string_view new_name);
    // Unregister AND strip the label from every vertex and edge (GDI FreeLabel).
    // Refuses (error_state) the key label of a live vertex: freeing it would
    // orphan the key index entry.
    [[nodiscard]] GdiStatus free_label(LabelId id);

    [[nodiscard]] GdiStatus create_property_type(PropertyTypeInfo const &info, PropNameId &out);
    [[nodiscard]] GdiStatus get_property_type_by_name(kimix::string_view name, PropNameId &out,
                                                      bool &found) const;
    [[nodiscard]] GdiStatus get_all_property_types(PropertyTypeList &out) const;
    [[nodiscard]] GdiStatus update_property_type(PropNameId id, PropertyTypeInfo const &info);
    // Unregister AND remove every value of that type from every vertex and edge.
    [[nodiscard]] GdiStatus free_property_type(PropNameId id);

    // --- transactions (§2.7) ------------------------------------------------
    // Auto-transaction: every command above applies immediately (the container's
    // own behaviour) and marks what it touched dirty.
    // An explicit transaction brackets several commands; abort() restores the
    // pre-begin state exactly. read_only (a GDI collective transaction) makes
    // every write return error_read_only_transaction.
    [[nodiscard]] GdiStatus begin_transaction(bool read_only = false);
    [[nodiscard]] GdiStatus commit();
    [[nodiscard]] GdiStatus abort();
    [[nodiscard]] GdiStatus begin_collective();
    [[nodiscard]] GdiStatus close_collective(bool commit_it);
    [[nodiscard]] bool in_transaction() const noexcept { return _tx != nullptr; }
    [[nodiscard]] bool transaction_is_read_only() const noexcept;

private:
    friend class Transaction;

    // --- internals ----------------------------------------------------------
    // Resolve a guid to a vertex, or error_vertex.
    [[nodiscard]] GdiStatus _require_vertex(kimix::Guid const &uid, Vertex *&out) const;
    // Validate a property VALUE against its declared type: error_property_type
    // when undeclared, error_wrong_type when the kind/container/dtype disagree,
    // error_count when an array's length does not match the declaration.
    [[nodiscard]] GdiStatus _validate_property(PropNameId name, PropertyValue const &value) const;
    // Resolve the registered constraint (or an empty one for a zero handle).
    [[nodiscard]] GdiStatus _resolve_constraint(Constraint const &constraint,
                                                Constraint &out) const;
    // True when any live entity carries a value of this property type.
    [[nodiscard]] bool _property_values_exist(PropNameId name) const;
    // Strip a label / property type from every vertex and every edge. When a
    // transaction is open each affected entity's previous payload is journaled
    // first, so an abort puts the values back (a cascade is undoable).
    void _strip_label(LabelId label);
    void _strip_property_type(PropNameId name);
    // Journal the current payload of a vertex / edge (the undo of any payload
    // edit). Returns false when the journal is full.
    bool _journal_vertex_payload(kimix::Guid const &uid);
    bool _journal_edge_state(kimix::Guid const &edge);
    // True when a live vertex uses `label` as its key label.
    [[nodiscard]] bool _label_is_a_key(LabelId label) const;
    // Replay a transaction's journal in reverse (the abort path, and the bulk
    // batch's all-or-nothing rollback). Consumes the journal.
    [[nodiscard]] GdiStatus _rollback(Transaction &tx);

    Graph _graph;
    Schema _schema;
    ColdStorage _cold;
    GraphOptions _options;
    GraphSerializer _serializer;
    GraphDeserializer _deserializer;
    GraphSerializeStats _last_stats{};
    // The one open transaction (nullptr outside one). Created and owned by
    // begin_transaction(); the Database only tracks it so a second begin is an
    // error and so a write can check the read-only flag.
    Transaction *_tx{nullptr};
};

} // namespace kimix::graph_db
