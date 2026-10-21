/*
 * database.cpp — the GDI backend facade (see database.h for the contract and the
 * per-command error semantics it inherits from the C# reference backend).
 *
 * STRUCTURE
 *   Every command follows the same three steps, in this order:
 *     1. the transaction gate: a write inside a read-only (collective) scope is
 *        error_read_only_transaction, before anything else is looked at;
 *     2. validation: the schema lookup + the declaration checks, all of which
 *        happen BEFORE any state changes (so a refusal leaves nothing behind);
 *     3. the mutation, immediately followed by its undo record (if a transaction
 *        is open) — and, when the journal is full, an immediate manual undo plus
 *        error_size_limit.
 *   Reads take the locks they need through Graph / EdgeTable and never hold two
 *   shard locks at once: they copy the ids under one lock, then resolve each
 *   entity separately. A read may therefore observe an edge added between the two
 *   passes — the documented epoch-1 read-atomicity contract, unchanged.
 *
 * ROLLBACK (abort)
 *   The journal is replayed in REVERSE: an entity created is freed, an entity
 *   removed is re-adopted (with its payload and, for a vertex, the edges its
 *   cascade removed), an edited payload is overwritten with the saved one, and a
 *   schema registration is re-created with its EXACT id (a recycled id would
 *   dangle every entity that referenced it).
 */
#include "database.h"

#include "transaction.h"

#include <core/clock.h>
#include <core/stl/filesystem.h>

#include <algorithm>
#include <utility>

namespace kimix::graph_db {

namespace {

// GDI's count/size limit for one page: nothing enforces a maximum, but a caller
// that asks for a negative cap is rejected (the reference backend raises
// ErrorArgument), and an absurd cap is simply a request for everything.
constexpr int64_t kUnlimitedCap = 0x7fffffff;

[[nodiscard]] bool is_blank_name(kimix::string_view text) noexcept {
    for (char c : text) {
        if (c != ' ' && c != '//t' && c != '//n' && c != '//r') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_array_kind(PropertyKind kind) noexcept {
    return kind == PropertyKind::int64_array || kind == PropertyKind::double_array;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / store lifecycle
// ---------------------------------------------------------------------------

Database::Database() noexcept
    : _serializer(_cold) {
    _serializer.set_schema(&_schema);
}

Database::Database(GraphOptions const &options) noexcept
    : _options(options),
      _serializer(_cold) {
    _graph.set_options(_options);
    _serializer.set_schema(&_schema);
}

Database::~Database() noexcept {
    // An open transaction is aborted (its journal is replayed) before the graph
    // dies, so a caller that leaks one still gets a consistent graph for whoever
    // looks at it next.
    if (_tx != nullptr) {
        (void)abort();
    }
}

void Database::set_options(GraphOptions const &options) {
    _options = options;
    _graph.set_options(_options);
}

GdiStatus Database::open(kimix::filesystem::path const &dir) {
    return open(dir, _options);
}

GdiStatus Database::open(kimix::filesystem::path const &dir,
                         GraphOptions const &options) {
    if (is_open()) {
        return GdiStatus::error_state;
    }
    set_options(options);
    _serializer.set_schema(&_schema);
    if (dir.empty()) {
        // An in-memory database: no store, so persist() reports error_no_such_file
        // and nothing is ever read back. That is what a purely in-memory GDI
        // session wants.
        return GdiStatus::ok;
    }
    size_t const map_size = _options.cold_map_size != 0 ? _options.cold_map_size
                                                        : ColdStorage::default_map_size;
    if (!_cold.open(dir, map_size)) {
        return GdiStatus::error_io;
    }
    MetaRecord meta;
    if (_cold.read_meta(meta)) {
        _serializer.set_meta(meta);
    }
    return load_store();
}

void Database::close() noexcept {
    if (_tx != nullptr) {
        (void)abort();
    }
    _cold.close();
}

GdiStatus Database::persist(bool full, GraphSerializeStats *out) {
    if (!is_open()) {
        return GdiStatus::error_no_such_file;
    }
    // The schema and the options own the metadata a save stamps; hand them over so
    // a store is always self-describing.
    MetaRecord meta = _serializer.meta();
    meta.format_version = kFormatVersion;
    meta.graph_name = _options.graph_name;
    meta.schema_version = _schema.version();
    _serializer.set_meta(meta);
    GraphSerializeStats const stats = full ? _serializer.save(_graph)
                                           : _serializer.save_incremental(_graph);
    _last_stats = stats;
    if (out != nullptr) {
        *out = stats;
    }
    return stats.status;
}

GdiStatus Database::load_store(GraphSerializeStats *out) {
    if (!is_open()) {
        return GdiStatus::error_no_such_file;
    }
    GraphSerializeStats const stats = _deserializer.load(_cold, _graph, &_schema);
    _last_stats = stats;
    if (out != nullptr) {
        *out = stats;
    }
    return stats.status;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

GdiStatus Database::_require_vertex(kimix::Guid const &uid, Vertex *&out) const {
    out = _graph.find(uid);
    return out == nullptr ? GdiStatus::error_vertex : GdiStatus::ok;
}

GdiStatus Database::_validate_property(PropNameId name, PropertyValue const &value) const {
    if (name == kNoPropName || !_schema.has_property_type(name)) {
        return GdiStatus::error_property_type;
    }
    if (value.is_none()) {
        return GdiStatus::error_wrong_type;
    }
    PropertyTypeInfo const declaration = _schema.property_type(name);
    bool const wants_array = declaration.container == GdiContainerType::array;
    if (is_array_kind(value.kind()) != wants_array) {
        // A scalar where a fixed-length array is declared (or the inverse) is a
        // type error, not a count error.
        return GdiStatus::error_wrong_type;
    }
    if (value.data_type() != declaration.data) {
        return GdiStatus::error_wrong_type;
    }
    if (wants_array && value.element_count() != declaration.count) {
        return GdiStatus::error_count;
    }
    return GdiStatus::ok;
}

GdiStatus Database::_resolve_constraint(Constraint const &constraint,
                                        Constraint &out) const {
    if (constraint.conditions.empty()) {
        out = Constraint{};
        return GdiStatus::ok;
    }
    // A caller may either hand in a definition or (through the C ABI) a handle,
    // which is already resolved before this point. Here the definition itself is
    // validated: a label condition with an ordering op cannot be evaluated.
    for (auto const &condition : constraint.conditions) {
        if (condition.is_label()) {
            if (condition.op != GdiOp::equal && condition.op != GdiOp::not_equal) {
                return GdiStatus::error_op;
            }
        }
    }
    out = constraint;
    return GdiStatus::ok;
}

bool Database::_property_values_exist(PropNameId name) const {
    for (auto const &guid : _graph.vertex_guids()) {
        Vertex *vertex = _graph.find(guid);
        if (vertex == nullptr) {
            continue;
        }
        if (vertex->has_property(name)) {
            return true;
        }
    }
    for (auto const &edge : _graph.edge_ids()) {
        PropertyValue value;
        if (_graph.edge_property(edge, name, value)) {
            return true;
        }
    }
    return false;
}

bool Database::_label_is_a_key(LabelId label) const {
    for (auto const &guid : _graph.vertex_guids()) {
        Vertex *vertex = _graph.find(guid);
        if (vertex != nullptr && vertex->key_label() == label) {
            return true;
        }
    }
    return false;
}

bool Database::_journal_vertex_payload(kimix::Guid const &uid) {
    if (_tx == nullptr) {
        return true;
    }
    Vertex *vertex = _graph.find(uid);
    if (vertex == nullptr) {
        return true;
    }
    JournalEntry entry;
    entry.kind = JournalEntry::Kind::vertex_payload;
    entry.uid = uid;
    vertex->labels(entry.labels);
    entry.properties = vertex->property_list();
    return _tx->push(std::move(entry));
}

bool Database::_journal_edge_state(kimix::Guid const &edge) {
    if (_tx == nullptr) {
        return true;
    }
    EdgeRecord record;
    if (!_graph.edge_record(edge, record)) {
        return true;
    }
    JournalEntry entry;
    entry.kind = JournalEntry::Kind::edge_state;
    entry.uid = edge;
    entry.origin = record.origin;
    entry.target = record.target;
    entry.direction = record.direction;
    entry.labels = record.labels;
    entry.properties = record.properties;
    return _tx->push(std::move(entry));
}

void Database::_strip_label(LabelId label) {
    for (auto const &guid : _graph.vertex_guids()) {
        Vertex *vertex = _graph.find(guid);
        if (vertex == nullptr || !vertex->has_label(label)) {
            continue;
        }
        // Journal BEFORE the strip: the undo entry is the payload that still
        // carries the label.
        (void)_journal_vertex_payload(guid);
        (void)vertex->remove_label(label);
    }
    for (auto const &edge : _graph.edge_ids()) {
        kimix::vector<LabelId> labels;
        if (!_graph.edge_labels(edge, labels) ||
            std::find(labels.begin(), labels.end(), label) == labels.end()) {
            continue;
        }
        (void)_journal_edge_state(edge);
        (void)_graph.edge_remove_label(edge, label);
    }
}

void Database::_strip_property_type(PropNameId name) {
    kimix::vector<PropNameId> names;
    names.push_back(name);
    for (auto const &guid : _graph.vertex_guids()) {
        Vertex *vertex = _graph.find(guid);
        if (vertex == nullptr || !vertex->has_property(name)) {
            continue;
        }
        (void)_journal_vertex_payload(guid);
        (void)vertex->remove_properties(
            kimix::span<PropNameId const>{names.data(), names.size()});
    }
    for (auto const &edge : _graph.edge_ids()) {
        PropertyValue value;
        if (!_graph.edge_property(edge, name, value)) {
            continue;
        }
        (void)_journal_edge_state(edge);
        (void)_graph.edge_remove_properties(edge,
                                            kimix::span<PropNameId const>{names.data(),
                                                                          names.size()});
    }
}

// ---------------------------------------------------------------------------
// Vertices
// ---------------------------------------------------------------------------

GdiStatus Database::create_vertex(VertexKeyView key, kimix::span<const LabelId> labels,
                                  PropertyList const *properties, VertexCreateResult &out) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (key.label != kNoLabel) {
        if (!_schema.has_label(key.label)) {
            return GdiStatus::error_label;
        }
        if (key.external_id.empty()) {
            return GdiStatus::error_empty_name; // GDI's VertexKey.Validate
        }
    } else if (!key.external_id.empty()) {
        return GdiStatus::error_argument;
    }
    for (auto label : labels) {
        if (!_schema.has_label(label)) {
            return GdiStatus::error_label;
        }
    }
    if (properties != nullptr) {
        for (auto const &entry : *properties) {
            GdiStatus const status = _validate_property(entry.name, entry.value);
            if (is_error(status)) {
                return status;
            }
        }
    }

    Graph::VertexSpec spec;
    spec.key_label = key.label;
    spec.external_id = key.external_id;
    spec.labels = labels;
    spec.properties = properties;
    kimix::Guid uid;
    GdiStatus const status = _graph.create_vertex(spec, uid);
    if (is_error(status)) {
        return status; // error_non_unique_id / error_empty_name / error_argument
    }
    // The journal entry for a creation is just its uid: freeing it restores the
    // previous state exactly (the key index entry goes with it).
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_created;
        entry.uid = uid;
        if (!_tx->push(std::move(entry))) {
            (void)_graph.free_vertex(uid, /*guarded=*/false);
            return GdiStatus::error_size_limit;
        }
    }
    out.uid = uid;
    out.non_unique_id_warning = status == GdiStatus::warning_non_unique_id;
    return status;
}

GdiStatus Database::get_or_create_vertex(VertexKeyView key, kimix::span<const LabelId> labels,
                                         PropertyList const *properties,
                                         VertexMergeResult &out) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (key.label != kNoLabel && !_schema.has_label(key.label)) {
        return GdiStatus::error_label;
    }
    Graph::VertexSpec spec;
    spec.key_label = key.label;
    spec.external_id = key.external_id;
    spec.labels = labels;
    spec.properties = properties;
    kimix::Guid uid;
    bool created = false;
    GdiStatus status = _graph.get_or_create_vertex(spec, uid, created);
    if (is_error(status)) {
        return status;
    }
    if (created && _tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_created;
        entry.uid = uid;
        if (!_tx->push(std::move(entry))) {
            (void)_graph.free_vertex(uid, /*guarded=*/false);
            return GdiStatus::error_size_limit;
        }
    }
    out.uid = uid;
    out.created = created;
    return GdiStatus::ok;
}

GdiStatus Database::translate_vertex_id(VertexKeyView key, kimix::Guid &out_uid,
                                        bool &found) const {
    return _graph.translate_vertex_id(key, out_uid, found);
}

GdiStatus Database::free_vertex(kimix::Guid const &uid) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    kimix::vector<kimix::unique_ptr<Edge>> removed_edges;
    kimix::unique_ptr<Vertex> removed =
        _graph.remove_vertex(uid, _tx != nullptr ? &removed_edges : nullptr);
    if (!removed) {
        return GdiStatus::error_vertex;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_removed;
        entry.uid = uid;
        entry.labels.clear();
        removed->labels(entry.labels);
        entry.properties = removed->property_list();
        entry.vertex = std::move(removed);
        entry.removed_edges = std::move(removed_edges);
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return GdiStatus::ok;
}

GdiStatus Database::free_vertex_guarded(kimix::Guid const &uid) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (!_graph.contains(uid)) {
        return GdiStatus::error_vertex;
    }
    if (_graph.has_incident_edges(uid, EdgeOrientation::both)) {
        // GDI FreeVertexGuarded: ErrorEdge, and NOTHING removed.
        return GdiStatus::error_edge;
    }
    return free_vertex(uid);
}

GdiStatus Database::create_vertices_bulk(kimix::span<VertexBulkRow const> rows,
                                         bool continue_on_error,
                                         kimix::vector<VertexCreateResult> &results,
                                         kimix::vector<GdiStatus> *row_status,
                                         BulkStats &stats) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    results.clear();
    results.reserve(rows.size());
    if (row_status != nullptr) {
        row_status->clear();
        row_status->reserve(rows.size());
    }

    // ONE transaction for the whole batch: a local scope when the caller has none
    // open, so "N creates, one atomic unit" holds without the caller having to
    // begin anything (GDI's CreateVerticesBulkCommand shape).
    Transaction local(*this, /*read_only=*/false, _options.max_journal_bytes);
    Transaction *previous = _tx;
    bool const own_scope = previous == nullptr;
    if (own_scope) {
        _tx = &local;
    }

    bool aborted = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        VertexCreateResult result;
        GdiStatus status = GdiStatus::ok;
        if (aborted) {
            // Never attempted: reported, but not counted as a failure.
            status = GdiStatus::error_state;
        } else {
            status = create_vertex(VertexKeyView{rows[i].key.label,
                                                 rows[i].key.external_id.bytes()},
                                   kimix::span<const LabelId>{rows[i].labels.data(),
                                                              rows[i].labels.size()},
                                   &rows[i].properties, result);
            if (is_error(status)) {
                ++stats.failed;
            } else if (status == GdiStatus::warning_non_unique_id) {
                ++stats.created;
                ++stats.warning;
            } else {
                ++stats.created;
            }
        }
        if (is_error(status) && !aborted && !continue_on_error) {
            // All-or-nothing: the first failure aborts the whole batch and every
            // remaining row is marked error_state (never attempted). The batch
            // created NOTHING, so the counters are reset — only the row that
            // actually failed stays in `failed`.
            aborted = true;
            if (own_scope) {
                (void)_rollback(local);
                local.clear_journal();
            } else {
                (void)_rollback(*_tx);
                _tx->clear_journal();
            }
            stats.created = 0;
            stats.warning = 0;
        }
        results.push_back(result);
        if (row_status != nullptr) {
            row_status->push_back(status);
        }
    }
    if (own_scope) {
        _tx = previous;
        local.clear_journal();
    }
    return aborted ? GdiStatus::error_state : GdiStatus::ok;
}

GdiStatus Database::create_edges_bulk(kimix::span<EdgeBulkRow const> rows,
                                      bool continue_on_error,
                                      kimix::vector<kimix::Guid> &edge_ids,
                                      kimix::vector<GdiStatus> *row_status,
                                      BulkStats &stats) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    edge_ids.clear();
    edge_ids.reserve(rows.size());
    if (row_status != nullptr) {
        row_status->clear();
        row_status->reserve(rows.size());
    }
    bool aborted = false;
    for (auto const &row : rows) {
        kimix::Guid edge;
        GdiStatus status = GdiStatus::ok;
        if (aborted) {
            status = GdiStatus::error_state; // never attempted, not counted
        } else {
            status = edge_create(row.origin, row.target, row.direction,
                                 kimix::span<const LabelId>{row.labels.data(),
                                                            row.labels.size()},
                                 &row.properties, edge);
            if (is_error(status)) {
                ++stats.failed;
                if (!continue_on_error) {
                    aborted = true;
                }
            } else {
                ++stats.created;
            }
        }
        edge_ids.push_back(edge);
        if (row_status != nullptr) {
            row_status->push_back(status);
        }
    }
    return aborted ? GdiStatus::error_state : GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Vertex labels and properties
// ---------------------------------------------------------------------------

GdiStatus Database::add_label(kimix::Guid const &uid, LabelId label) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    if (!_schema.has_label(label)) {
        return GdiStatus::error_label;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        vertex->labels(entry.labels);
        entry.properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    // The vertex lock is taken by Vertex itself (level 5, no shard lock held).
    return vertex->add_label(label) ? GdiStatus::ok : GdiStatus::error_label;
}

GdiStatus Database::remove_label(kimix::Guid const &uid, LabelId label) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        vertex->labels(entry.labels);
        entry.properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return vertex->remove_label(label) ? GdiStatus::ok : GdiStatus::error_label;
}

GdiStatus Database::get_labels(kimix::Guid const &uid,
                               kimix::vector<kimix::string> &out) const {
    Vertex *vertex = nullptr;
    GdiStatus const status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    out.clear();
    // GDI order: the KEY label first, then the entity's own labels.
    if (vertex->key_label() != kNoLabel) {
        out.push_back(_schema.label_name(vertex->key_label()));
    }
    kimix::vector<LabelId> labels;
    vertex->labels(labels);
    for (auto label : labels) {
        if (label == vertex->key_label()) {
            continue; // already emitted first
        }
        out.push_back(_schema.label_name(label));
    }
    return GdiStatus::ok;
}

GdiStatus Database::add_property(kimix::Guid const &uid, PropNameId name,
                                 PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        vertex->labels(entry.labels);
        entry.properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return vertex->add_property(name, std::move(value)) ? GdiStatus::ok
                                                        : GdiStatus::error_property_exists;
}

GdiStatus Database::set_property(kimix::Guid const &uid, PropNameId name,
                                 PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        vertex->labels(entry.labels);
        entry.properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return vertex->set_property(name, std::move(value)) ? GdiStatus::ok
                                                        : GdiStatus::error_no_property;
}

GdiStatus Database::update_property(kimix::Guid const &uid, PropNameId name,
                                    PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        vertex->labels(entry.labels);
        entry.properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    (void)vertex->put_property(name, std::move(value)); // upsert: never an error
    return GdiStatus::ok;
}

GdiStatus Database::remove_properties(kimix::Guid const &uid,
                                      kimix::span<const PropNameId> names) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    Vertex *vertex = nullptr;
    GdiStatus status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::vertex_payload;
        entry.uid = uid;
        vertex->labels(entry.labels);
        entry.properties = vertex->property_list();
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    (void)vertex->remove_properties(names); // absent names are not an error
    return GdiStatus::ok;
}

GdiStatus Database::get_properties(kimix::Guid const &uid, NamedPropertyList &out) const {
    Vertex *vertex = nullptr;
    GdiStatus const status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    out.clear();
    PropertyList properties;
    vertex->properties(properties);
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        NamedProperty item;
        item.name = _schema.property_type(entry.name).name;
        item.value = entry.value;
        out.push_back(std::move(item));
    }
    return GdiStatus::ok;
}

GdiStatus Database::get_property_types(kimix::Guid const &uid, PropertyTypeList &out) const {
    Vertex *vertex = nullptr;
    GdiStatus const status = _require_vertex(uid, vertex);
    if (is_error(status)) {
        return status;
    }
    out.clear();
    PropertyList properties;
    vertex->properties(properties);
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        out.push_back(_schema.property_type(entry.name));
    }
    std::sort(out.begin(), out.end(),
              [](PropertyTypeInfo const &lhs, PropertyTypeInfo const &rhs) {
                  return lhs.name < rhs.name;
              });
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Edges (delegated to Graph, which owns the shard choreography)
// ---------------------------------------------------------------------------

GdiStatus Database::edge_create(kimix::Guid const &origin, kimix::Guid const &target,
                                EdgeDirectionType direction,
                                kimix::span<const LabelId> labels,
                                PropertyList const *properties, kimix::Guid &out_edge) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    for (auto label : labels) {
        if (!_schema.has_label(label)) {
            return GdiStatus::error_label;
        }
    }
    if (properties != nullptr) {
        for (auto const &entry : *properties) {
            GdiStatus const status = _validate_property(entry.name, entry.value);
            if (is_error(status)) {
                return status;
            }
        }
    }
    GdiStatus const status = _graph.edge_create(origin, target, direction, labels, properties,
                                               out_edge);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_created;
        entry.uid = out_edge;
        if (!_tx->push(std::move(entry))) {
            (void)_graph.edge_free(out_edge);
            return GdiStatus::error_size_limit;
        }
    }
    return GdiStatus::ok;
}

GdiStatus Database::edge_free(kimix::Guid const &edge) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        // Keep the record so an abort can put it back verbatim.
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_removed;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_free(edge);
}

GdiStatus Database::set_edge_origin(kimix::Guid const &edge, kimix::Guid const &origin) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_set_origin(edge, origin);
}

GdiStatus Database::set_edge_target(kimix::Guid const &edge, kimix::Guid const &target) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_set_target(edge, target);
}

GdiStatus Database::set_edge_direction(kimix::Guid const &edge,
                                       EdgeDirectionType direction) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_set_direction(edge, direction);
}

GdiStatus Database::get_edge_endpoints(kimix::Guid const &edge, kimix::Guid &origin,
                                       kimix::Guid &target) const {
    return _graph.edge_endpoints(edge, origin, target) ? GdiStatus::ok
                                                       : GdiStatus::error_uid;
}

GdiStatus Database::add_edge_label(kimix::Guid const &edge, LabelId label) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (!_schema.has_label(label)) {
        return GdiStatus::error_label;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_add_label(edge, label);
}

GdiStatus Database::remove_edge_label(kimix::Guid const &edge, LabelId label) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_remove_label(edge, label);
}

GdiStatus Database::get_edge_labels(kimix::Guid const &edge,
                                    kimix::vector<kimix::string> &out) const {
    out.clear();
    kimix::vector<LabelId> labels;
    if (!_graph.edge_labels(edge, labels)) {
        return GdiStatus::error_uid;
    }
    out.reserve(labels.size());
    for (auto label : labels) {
        out.push_back(_schema.label_name(label));
    }
    return GdiStatus::ok;
}

GdiStatus Database::add_edge_property(kimix::Guid const &edge, PropNameId name,
                                      PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    GdiStatus const status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_add_property(edge, name, std::move(value));
}

GdiStatus Database::set_edge_property(kimix::Guid const &edge, PropNameId name,
                                      PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    GdiStatus const status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_set_property(edge, name, std::move(value));
}

GdiStatus Database::update_edge_property(kimix::Guid const &edge, PropNameId name,
                                         PropertyValue value) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    GdiStatus const status = _validate_property(name, value);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_put_property(edge, name, std::move(value));
}

GdiStatus Database::remove_edge_properties(kimix::Guid const &edge,
                                           kimix::span<const PropNameId> names) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        EdgeRecord record;
        if (!_graph.edge_record(edge, record)) {
            return GdiStatus::error_uid;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::edge_state;
        entry.uid = edge;
        entry.origin = record.origin;
        entry.target = record.target;
        entry.direction = record.direction;
        entry.labels = record.labels;
        entry.properties = record.properties;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _graph.edge_remove_properties(edge, names);
}

GdiStatus Database::get_edge_properties(kimix::Guid const &edge,
                                        NamedPropertyList &out) const {
    out.clear();
    PropertyList properties;
    if (!_graph.edge_properties(edge, properties)) {
        return GdiStatus::error_uid;
    }
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        NamedProperty item;
        item.name = _schema.property_type(entry.name).name;
        item.value = entry.value;
        out.push_back(std::move(item));
    }
    return GdiStatus::ok;
}

GdiStatus Database::get_edge_property_types(kimix::Guid const &edge,
                                            PropertyTypeList &out) const {
    out.clear();
    PropertyList properties;
    if (!_graph.edge_properties(edge, properties)) {
        return GdiStatus::error_uid;
    }
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        out.push_back(_schema.property_type(entry.name));
    }
    std::sort(out.begin(), out.end(),
              [](PropertyTypeInfo const &lhs, PropertyTypeInfo const &rhs) {
                  return lhs.name < rhs.name;
              });
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Filtered, oriented, paged reads
// ---------------------------------------------------------------------------

GdiStatus Database::get_edges_of_vertex(kimix::Guid const &uid,
                                        Constraint const &constraint,
                                        EdgeOrientation orientation, int64_t cap,
                                        EdgePage &out) const {
    out = EdgePage{};
    if (cap < 0) {
        return GdiStatus::error_argument;
    }
    if (!_graph.contains(uid)) {
        return GdiStatus::error_vertex;
    }
    Constraint resolved;
    GdiStatus status = _resolve_constraint(constraint, resolved);
    if (is_error(status)) {
        return status;
    }
    kimix::vector<EdgeId> ids;
    _graph.incident_edges(uid, orientation, ids);
    kimix::vector<EdgeRecord> matches;
    matches.reserve(ids.size());
    for (auto const &id : ids) {
        EdgeRecord record;
        if (!_graph.edge_record(id, record)) {
            continue; // removed between the two passes (epoch-1 read atomicity)
        }
        // §2.6: an EDGE read filters the edge's own labels and properties.
        if (!kimix::graph_db::matches(resolved, record.labels, record.properties)) {
            continue;
        }
        matches.push_back(std::move(record));
    }
    out = make_page(std::move(matches), cap);
    return GdiStatus::ok;
}

GdiStatus Database::get_neighbor_vertices(kimix::Guid const &uid,
                                          Constraint const &constraint,
                                          EdgeOrientation orientation, int64_t cap,
                                          VertexPage &out) const {
    out = VertexPage{};
    if (cap < 0) {
        return GdiStatus::error_argument;
    }
    if (!_graph.contains(uid)) {
        return GdiStatus::error_vertex;
    }
    Constraint resolved;
    GdiStatus status = _resolve_constraint(constraint, resolved);
    if (is_error(status)) {
        return status;
    }
    kimix::vector<EdgeId> ids;
    _graph.incident_edges(uid, orientation, ids);
    kimix::vector<kimix::Guid> matches;
    kimix::unordered_set<kimix::Guid> seen;
    matches.reserve(ids.size());
    seen.reserve(ids.size());
    for (auto const &id : ids) {
        kimix::Guid origin;
        kimix::Guid target;
        if (!_graph.edge_endpoints(id, origin, target)) {
            continue;
        }
        kimix::Guid const neighbour = origin == uid ? target : origin;
        if (!seen.insert(neighbour).second) {
            continue; // a neighbour is returned once (multi-edges collapse)
        }
        Vertex *other = _graph.find(neighbour);
        if (other == nullptr) {
            continue; // a missing endpoint is skipped, not an error
        }
        // §5.10: a NEIGHBOURHOOD read filters the neighbour vertex, not the edge.
        kimix::vector<LabelId> labels;
        other->labels(labels);
        PropertyBag properties;
        PropertyList list = other->property_list();
        for (auto const &entry : list) {
            properties.emplace(entry.name, entry.value);
        }
        if (!kimix::graph_db::matches(resolved, labels, properties)) {
            continue;
        }
        matches.push_back(neighbour);
    }
    out = make_page(std::move(matches), cap);
    return GdiStatus::ok;
}

GdiStatus Database::count_neighbors(kimix::Guid const &uid, Constraint const &constraint,
                                    EdgeOrientation orientation, size_t &out) const {
    out = 0;
    VertexPage page;
    GdiStatus const status =
        get_neighbor_vertices(uid, constraint, orientation, kUnlimitedCap, page);
    if (is_error(status)) {
        return status;
    }
    out = page.total_count;
    return GdiStatus::ok;
}

GdiStatus Database::exists_neighbor(kimix::Guid const &uid, Constraint const &constraint,
                                    EdgeOrientation orientation, bool &out) const {
    out = false;
    // cap == 1 is the short circuit: GDI's ExistsNeighbor only needs a bool, so the
    // read stops at the first match (`total_count` is therefore not meaningful
    // here and is left at the page's own count).
    VertexPage page;
    GdiStatus const status = get_neighbor_vertices(uid, constraint, orientation, 1, page);
    if (is_error(status)) {
        return status;
    }
    out = page.total_count > 0;
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

GdiStatus Database::create_constraint(Constraint const &definition, ConstraintHandle &out) {
    return _schema.create_constraint(definition, out);
}

GdiStatus Database::add_label_condition(ConstraintHandle handle, LabelId label, GdiOp op) {
    if (!_schema.has_label(label)) {
        return GdiStatus::error_label;
    }
    return _schema.add_label_condition(handle, label, op);
}

GdiStatus Database::add_property_condition(ConstraintHandle handle, PropNameId property,
                                           GdiOp op, PropertyValue value, uint32_t count) {
    if (!_schema.has_property_type(property)) {
        return GdiStatus::error_property_type;
    }
    return _schema.add_property_condition(handle, property, op, std::move(value), count);
}

GdiStatus Database::free_constraint(ConstraintHandle handle) {
    return _schema.free_constraint(handle);
}

// ---------------------------------------------------------------------------
// Schema commands
// ---------------------------------------------------------------------------

GdiStatus Database::create_label(kimix::string_view name, LabelId &out) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    LabelId id = kNoLabel;
    GdiStatus const status = _schema.create_label(name, id);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::label_created;
        entry.label = id;
        if (!_tx->push(std::move(entry))) {
            (void)_schema.free_label(id);
            return GdiStatus::error_size_limit;
        }
    }
    out = id;
    return GdiStatus::ok;
}

GdiStatus Database::get_label_by_name(kimix::string_view name, LabelId &out,
                                      bool &found) const {
    found = _schema.find_label(name, out);
    return GdiStatus::ok;
}

GdiStatus Database::get_all_labels(NamedLabelList &out) const {
    out.clear();
    for (auto id : _schema.all_labels()) {
        NamedLabel item;
        item.id = id;
        item.name = _schema.label_name(id);
        out.push_back(std::move(item));
    }
    return GdiStatus::ok;
}

GdiStatus Database::update_label(LabelId id, kimix::string_view new_name) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (_tx != nullptr) {
        kimix::string const previous = _schema.label_name(id);
        if (previous.empty()) {
            return GdiStatus::error_label;
        }
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::label_renamed;
        entry.label = id;
        entry.name = previous;
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    return _schema.rename_label(id, new_name);
}

GdiStatus Database::free_label(LabelId id) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (!_schema.has_label(id)) {
        return GdiStatus::error_label;
    }
    if (_label_is_a_key(id)) {
        // Freeing the key label of a live vertex would orphan its key index
        // entry, so it is refused rather than silently breaking the primary key.
        return GdiStatus::error_state;
    }
    if (_tx != nullptr) {
        // The undo entry is the REGISTRATION (so the name comes back with the same
        // id) plus one entry per entity the cascade touches, pushed by _strip_label.
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::label_freed;
        entry.label = id;
        entry.name = _schema.label_name(id);
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    GdiStatus const status = _schema.free_label(id);
    if (is_error(status)) {
        return status;
    }
    _strip_label(id); // GDI FreeLabel cascades to every entity
    return GdiStatus::ok;
}

GdiStatus Database::create_property_type(PropertyTypeInfo const &info, PropNameId &out) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    PropNameId id = kNoPropName;
    GdiStatus const status = _schema.create_property_type(info, id);
    if (is_error(status)) {
        return status;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::property_type_created;
        entry.property = id;
        if (!_tx->push(std::move(entry))) {
            (void)_schema.free_property_type(id);
            return GdiStatus::error_size_limit;
        }
    }
    out = id;
    return GdiStatus::ok;
}

GdiStatus Database::get_property_type_by_name(kimix::string_view name, PropNameId &out,
                                             bool &found) const {
    found = _schema.find_property_type(name, out);
    return GdiStatus::ok;
}

GdiStatus Database::get_all_property_types(PropertyTypeList &out) const {
    out = _schema.all_property_types();
    return GdiStatus::ok;
}

GdiStatus Database::update_property_type(PropNameId id, PropertyTypeInfo const &info) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    // Name validation comes FIRST, exactly like the reference backend's
    // RequireName-then-lookup order.
    if (is_blank_name(info.name)) {
        return GdiStatus::error_empty_name;
    }
    if (!_schema.has_property_type(id)) {
        return GdiStatus::error_property_type;
    }
    if (info.count < 1) {
        return GdiStatus::error_count;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::property_type_updated;
        entry.property = id;
        entry.info = _schema.property_type(id);
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    // A live value pins the declaration's shape (see Schema::update_property_type).
    bool const values_exist = _property_values_exist(id);
    return _schema.update_property_type(id, info, values_exist);
}

GdiStatus Database::free_property_type(PropNameId id) {
    if (_tx != nullptr && _tx->read_only()) {
        return GdiStatus::error_read_only_transaction;
    }
    if (!_schema.has_property_type(id)) {
        return GdiStatus::error_property_type;
    }
    if (_tx != nullptr) {
        JournalEntry entry;
        entry.kind = JournalEntry::Kind::property_type_freed;
        entry.property = id;
        entry.info = _schema.property_type(id);
        if (!_tx->push(std::move(entry))) {
            return GdiStatus::error_size_limit;
        }
    }
    GdiStatus const status = _schema.free_property_type(id);
    if (is_error(status)) {
        return status;
    }
    _strip_property_type(id); // GDI FreePropertyType removes every value of it
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------

bool Database::transaction_is_read_only() const noexcept {
    return _tx != nullptr && _tx->read_only();
}

GdiStatus Database::begin_transaction(bool read_only) {
    if (_tx != nullptr) {
        // One write transaction at a time: the container's single-writer contract
        // surfaced as a status (GDI ErrorTransaction).
        return GdiStatus::error_transaction;
    }
    _tx = new Transaction(*this, read_only, _options.max_journal_bytes);
    return GdiStatus::ok;
}

GdiStatus Database::commit() {
    if (_tx == nullptr) {
        return GdiStatus::error_transaction;
    }
    Transaction *tx = _tx;
    _tx = nullptr;
    tx->_closed = true;
    tx->clear_journal();
    // A write transaction dirties what it touched, so the incremental save below
    // (when requested) writes exactly that. Everything a command marked dirty
    // already is left as it is.
    if (_options.persist_on_commit && !tx->read_only() && is_open()) {
        delete tx;
        return persist(/*full=*/false);
    }
    delete tx;
    return GdiStatus::ok;
}

GdiStatus Database::abort() {
    if (_tx == nullptr) {
        return GdiStatus::error_transaction;
    }
    Transaction *tx = _tx;
    _tx = nullptr;
    tx->_closed = true;
    GdiStatus const status = _rollback(*tx);
    delete tx;
    return status;
}

GdiStatus Database::begin_collective() {
    GdiStatus const status = begin_transaction(/*read_only=*/true);
    if (is_error(status)) {
        return status;
    }
    _tx->_collective = true;
    return GdiStatus::ok;
}

GdiStatus Database::close_collective(bool commit_it) {
    if (_tx == nullptr || !_tx->collective()) {
        // A collective scope is begun through begin_collective(); closing a
        // writable transaction this way is a caller error.
        return GdiStatus::error_transaction;
    }
    return commit_it ? commit() : abort();
}

// ---------------------------------------------------------------------------
// Rollback
// ---------------------------------------------------------------------------

GdiStatus Database::_rollback(Transaction &tx) {
    kimix::vector<JournalEntry> journal = tx.take_journal();
    GdiStatus status = GdiStatus::ok;
    // REVERSE order: the last mutation is undone first, so an entity is always
    // restored before the mutation that depended on it is undone.
    for (size_t i = journal.size(); i > 0; --i) {
        JournalEntry &entry = journal[i - 1];
        switch (entry.kind) {
        case JournalEntry::Kind::vertex_created: {
            kimix::unique_ptr<Vertex> removed = _graph.remove_vertex(entry.uid);
            (void)removed;
            break;
        }
        case JournalEntry::Kind::vertex_removed: {
            // Re-adopt the vertex with its payload, then every edge its cascade
            // removed. The key index entry comes back with adopt_vertex.
            if (entry.vertex) {
                entry.vertex->restore_payload(
                    kimix::span<const LabelId>{entry.labels.data(), entry.labels.size()},
                    entry.properties);
                (void)_graph.adopt_vertex(std::move(entry.vertex));
            }
            for (auto &edge : entry.removed_edges) {
                if (edge) {
                    (void)_graph.adopt_edge(std::move(edge));
                }
            }
            break;
        }
        case JournalEntry::Kind::vertex_payload: {
            Vertex *vertex = _graph.find(entry.uid);
            if (vertex != nullptr) {
                vertex->restore_payload(
                    kimix::span<const LabelId>{entry.labels.data(), entry.labels.size()},
                    entry.properties);
            }
            break;
        }
        case JournalEntry::Kind::edge_created:
            (void)_graph.edge_free(entry.uid);
            break;
        case JournalEntry::Kind::edge_removed: {
            kimix::unique_ptr<Edge> restored{new Edge(entry.uid, entry.origin, entry.target,
                                                     entry.direction)};
            restored->assign_payload(kimix::span<const LabelId>{entry.labels.data(),
                                                               entry.labels.size()},
                                     entry.properties);
            restored->clear_dirty();
            (void)_graph.adopt_edge(std::move(restored));
            break;
        }
        case JournalEntry::Kind::edge_state: {
            // Endpoints first (they move the adjacency), then the payload.
            kimix::Guid origin;
            kimix::Guid target;
            if (!_graph.edge_endpoints(entry.uid, origin, target)) {
                break;
            }
            if (origin != entry.origin) {
                (void)_graph.edge_set_origin(entry.uid, entry.origin);
            }
            if (target != entry.target) {
                (void)_graph.edge_set_target(entry.uid, entry.target);
            }
            (void)_graph.edge_set_direction(entry.uid, entry.direction);
            _graph.edge_restore_payload(
                entry.uid,
                kimix::span<const LabelId>{entry.labels.data(), entry.labels.size()},
                entry.properties);
            break;
        }
        case JournalEntry::Kind::label_created:
            (void)_schema.free_label(entry.label);
            break;
        case JournalEntry::Kind::label_renamed:
            (void)_schema.rename_label(entry.label, entry.name);
            break;
        case JournalEntry::Kind::label_freed: {
            LabelTableEntry restored;
            restored.id = entry.label;
            restored.name = entry.name;
            (void)_schema.restore_label(restored);
            break;
        }
        case JournalEntry::Kind::property_type_created:
            (void)_schema.free_property_type(entry.property);
            break;
        case JournalEntry::Kind::property_type_updated: {
            PropertyTypeInfo const current = _schema.property_type(entry.property);
            (void)current;
            (void)_schema.update_property_type(entry.property, entry.info, false);
            break;
        }
        case JournalEntry::Kind::property_type_freed:
            (void)_schema.restore_property_type(entry.info);
            break;
        }
    }
    return status;
}

} // namespace kimix::graph_db
