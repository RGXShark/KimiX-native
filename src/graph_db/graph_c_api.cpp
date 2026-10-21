/*
 * graph_c_api.cpp — the C ABI over kimix::graph_db::Database (see graph_c_api.h
 * for the contract).
 *
 * STRUCTURE
 *   `struct kimix_gdb_session` wraps ONE Database plus the two pieces of state the
 *   C side cannot express: the constraint handles the caller refers to by id (a
 *   Constraint is a C++ value it cannot hold) and the session's open flag.
 *   Everything else is a direct forwarding call, with the three conversions the
 *   ABI needs:
 *
 *     guid        kimix_gdb_guid {lo, hi}   <->  kimix::Guid {data0, data1}
 *     bytes       (uint8_t*, len)           <->  kimix::span<const std::byte>
 *     property    kimix_gdb_property        <->  kimix::graph_db::PropertyValue
 *
 *   The two-phase read protocol is implemented by ONE helper (append into a
 *   caller scratch buffer) so "buffer too small" behaves identically everywhere:
 *   the call returns KIMIX_GDB_ERR_BUFFER and still reports the sizes needed.
 *
 * NO EXCEPTIONS, NO RTTI, NO C++ TYPES AT THE BOUNDARY. A failure is a status
 * code and an out-parameter left untouched.
 */
#include "graph_c_api.h"

#include "database.h"
#include "graph_db.h"
#include "status.h"
#include "transaction.h"

#include <core/guid.h>
#include <core/md5.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <cstring>
#include <utility>

/*
 * A session-local constraint: the ABI hands out a dense id and keeps the
 * definition beside it, because a Constraint is a C++ value the caller cannot hold.
 */
struct ConstraintSlot {
    kimix::graph_db::ConstraintHandle handle;
    kimix::graph_db::Constraint definition;
};

/* The opaque session (the C side only ever holds a pointer to it). */
struct kimix_gdb_session {
    kimix::graph_db::Database database;
    kimix::vector<ConstraintSlot> constraints;
    uint32_t next_constraint{1};
    bool open{false};
};

namespace {

using kimix::graph_db::Constraint;
using kimix::graph_db::ConstraintHandle;
using kimix::graph_db::Database;
using kimix::graph_db::EdgeDirectionType;
using kimix::graph_db::EdgeOrientation;
using kimix::graph_db::EdgeRecord;
using kimix::graph_db::GdiOp;
using kimix::graph_db::GdiStatus;
using kimix::graph_db::LabelId;
using kimix::graph_db::PropNameId;
using kimix::graph_db::PropertyKind;
using kimix::graph_db::PropertyValue;
using kimix::graph_db::VertexKeyView;


inline int32_t code_of(GdiStatus status) noexcept {
    return static_cast<int32_t>(status);
}

inline kimix::Guid to_guid(kimix_gdb_guid value) noexcept {
    return kimix::Guid{kimix::Guid::GuidData{value.lo, value.hi}};
}

inline kimix_gdb_guid from_guid(kimix::Guid const &value) noexcept {
    kimix_gdb_guid out;
    out.lo = value.to_binary().data0;
    out.hi = value.to_binary().data1;
    return out;
}

inline kimix::span<const std::byte> to_bytes(kimix_gdb_bytes value) noexcept {
    if (value.data == nullptr || value.len == 0) {
        return {};
    }
    return kimix::span<const std::byte>{reinterpret_cast<std::byte const *>(value.data),
                                        value.len};
}

// One property value decoded from the ABI struct. The array kinds read their
// elements from `payload` (little-endian, 8 bytes each).
PropertyValue to_value(kimix_gdb_property const &input) {
    switch (input.kind) {
    case KIMIX_GDB_KIND_INT64:
        return PropertyValue::make_int64(input.i64);
    case KIMIX_GDB_KIND_DOUBLE:
        return PropertyValue::make_double(input.f64);
    case KIMIX_GDB_KIND_STRING:
        return PropertyValue::make_string(kimix::string_view{
            reinterpret_cast<char const *>(input.payload.data), input.payload.len});
    case KIMIX_GDB_KIND_BLOB:
        return PropertyValue::make_blob(to_bytes(input.payload));
    case KIMIX_GDB_KIND_INT64_ARRAY: {
        kimix::vector<int64_t> values;
        size_t const count = input.payload.len / 8;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int64_t element = 0;
            std::memcpy(&element, input.payload.data + i * 8, sizeof(element));
            values.push_back(element);
        }
        return PropertyValue::make_int64_array(
            kimix::span<const int64_t>{values.data(), values.size()});
    }
    case KIMIX_GDB_KIND_DOUBLE_ARRAY: {
        kimix::vector<double> values;
        size_t const count = input.payload.len / 8;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            double element = 0.0;
            std::memcpy(&element, input.payload.data + i * 8, sizeof(element));
            values.push_back(element);
        }
        return PropertyValue::make_double_array(
            kimix::span<const double>{values.data(), values.size()});
    }
    default:
        return PropertyValue{};
    }
}

// A scratch writer: one caller-owned buffer, append-only, overflow flag instead of
// an exception. Every variable-length read uses it. `needed()` tallies EVERY
// append attempt, so a call that overflowed still reports the buffer size the
// caller must provide (the contract: ERR_BUFFER reports the required sizes).
class ScratchWriter {
public:
    ScratchWriter(uint8_t *buffer, size_t capacity) noexcept
        : _buffer(buffer), _capacity(capacity) {
    }

    bool append(kimix::span<const std::byte> bytes, uint32_t &offset, uint32_t &length) noexcept {
        _needed += bytes.size();
        if (_used + bytes.size() > _capacity) {
            _overflow = true;
            offset = 0;
            length = 0;
            return false;
        }
        offset = static_cast<uint32_t>(_used);
        length = static_cast<uint32_t>(bytes.size());
        if (!bytes.empty() && _buffer != nullptr) {
            std::memcpy(_buffer + _used, bytes.data(), bytes.size());
        }
        _used += bytes.size();
        return true;
    }

    bool append_text(kimix::string_view text, uint32_t &offset, uint32_t &length) noexcept {
        return append(kimix::span<const std::byte>{
                          reinterpret_cast<std::byte const *>(text.data()), text.size()},
                      offset, length);
    }

    [[nodiscard]] bool overflowed() const noexcept { return _overflow; }
    [[nodiscard]] size_t used() const noexcept { return _used; }
    // The buffer size that fits every append attempt, successful or not: what a
    // caller must allocate after an ERR_BUFFER, and what the probe phase reports.
    [[nodiscard]] size_t needed() const noexcept { return _needed; }

private:
    uint8_t *_buffer{nullptr};
    size_t _capacity{0};
    size_t _used{0};
    size_t _needed{0};
    bool _overflow{false};
};

// The payload bytes a read copies into the scratch buffer for one value.
kimix::span<const std::byte> payload_of(PropertyValue const &value) noexcept {
    switch (value.kind()) {
    case PropertyKind::string: {
        auto const text = value.as_string();
        return kimix::span<const std::byte>{
            reinterpret_cast<std::byte const *>(text.data()), text.size()};
    }
    case PropertyKind::blob:
        return value.as_blob();
    case PropertyKind::int64_array: {
        auto const ints = value.as_int64_array();
        return kimix::span<const std::byte>{
            reinterpret_cast<std::byte const *>(ints.data()), ints.size() * 8};
    }
    case PropertyKind::double_array: {
        auto const reals = value.as_double_array();
        return kimix::span<const std::byte>{
            reinterpret_cast<std::byte const *>(reals.data()), reals.size() * 8};
    }
    default:
        return {};
    }
}

inline void fill_property_record(kimix_gdb_property_record &record, PropNameId name,
                                 PropertyValue const &value, uint32_t offset,
                                 uint32_t length) noexcept {
    record.name_id = name;
    record.kind = static_cast<uint8_t>(value.kind());
    record.dtype = static_cast<uint8_t>(value.data_type());
    size_t const elements = value.element_count();
    record.count = static_cast<uint16_t>(elements == 0 ? 1 : elements);
    record.i64 = value.as_int64();
    record.f64 = value.as_double();
    record.payload_offset = offset;
    record.payload_len = length;
}

// Write a (name, value) list into the caller's arrays. Reports the FULL count and
// the scratch bytes needed even when the buffers are too small.
GdiStatus write_properties(kimix::vector<std::pair<PropNameId, PropertyValue>> const &properties,
                           kimix_gdb_property_record *items, size_t capacity,
                           uint8_t *scratch_buffer, size_t scratch_capacity,
                           size_t *out_count, size_t *out_scratch) {
    // PHASE 1 (both buffers NULL): report the sizes and succeed, so a caller can
    // learn what it needs without a second guess.
    bool const probe = items == nullptr && scratch_buffer == nullptr;
    if (probe) {
        size_t needed = 0;
        for (auto const &property : properties) {
            needed += payload_of(property.second).size();
        }
        if (out_count != nullptr) {
            *out_count = properties.size();
        }
        if (out_scratch != nullptr) {
            *out_scratch = needed;
        }
        return GdiStatus::ok;
    }
    ScratchWriter scratch{scratch_buffer, scratch_capacity};
    for (size_t i = 0; i < properties.size(); ++i) {
        uint32_t offset = 0;
        uint32_t length = 0;
        bool const fits = scratch.append(payload_of(properties[i].second), offset, length);
        if (items != nullptr && i < capacity) {
            fill_property_record(items[i], properties[i].first, properties[i].second, offset,
                                length);
        }
        (void)fits;
    }
    if (out_count != nullptr) {
        *out_count = properties.size();
    }
      if (out_scratch != nullptr) {
          *out_scratch = scratch.needed();
      }
      if (scratch.overflowed()) {
          return GdiStatus::error_buffer;
      }
      if (properties.size() > capacity) {
          return GdiStatus::error_buffer;
      }
      return GdiStatus::ok;
  }

ConstraintSlot *find_slot(kimix_gdb_session *session, uint32_t id) {
    for (auto &slot : session->constraints) {
        if (slot.handle.id == id) {
            return &slot;
        }
    }
    return nullptr;
}

GdiStatus resolve_constraint(kimix_gdb_session *session, uint32_t id, Constraint &out) {
    out = Constraint{};
    if (id == 0) {
        return GdiStatus::ok; // an id of 0 means "no constraint": match everything
    }
    ConstraintSlot *slot = find_slot(session, id);
    if (slot == nullptr) {
        return GdiStatus::error_constraint;
    }
    out = slot->definition;
    return GdiStatus::ok;
}

// The property list of one vertex in the ABI's (name, value) shape.
void collect_vertex_properties(Database &db, kimix::Guid const &uid,
                               kimix::vector<std::pair<PropNameId, PropertyValue>> &out) {
    out.clear();
    kimix::graph_db::Vertex *vertex = db.graph().find(uid);
    if (vertex == nullptr) {
        return;
    }
    kimix::graph_db::PropertyList properties;
    vertex->properties(properties);
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        out.push_back({entry.name, entry.value});
    }
}

void collect_edge_properties(Database &db, kimix::Guid const &edge,
                             kimix::vector<std::pair<PropNameId, PropertyValue>> &out) {
    out.clear();
    kimix::graph_db::PropertyList properties;
    if (!db.graph().edge_properties(edge, properties)) {
        return;
    }
    out.reserve(properties.size());
    for (auto const &entry : properties) {
        out.push_back({entry.name, entry.value});
    }
}

// The (name, value) list of a vertex spec: `properties[i]` is named by
// `property_names[i]`. A missing names array is an argument error the callers
// report through the Database (an unnamed property is refused as
// error_property_type, which is the same status a caller would get for a bogus id).
void named_properties(kimix_gdb_vertex_spec const &spec,
                      kimix::graph_db::PropertyList &out) {
    out.clear();
    if (spec.properties == nullptr || spec.property_count == 0) {
        return;
    }
    out.reserve(spec.property_count);
    for (size_t i = 0; i < spec.property_count; ++i) {
        kimix::graph_db::PropertyEntry entry;
        entry.name = spec.property_names != nullptr ? spec.property_names[i] : 0;
        entry.value = to_value(spec.properties[i]);
        out.push_back(std::move(entry));
    }
}

void named_properties(kimix_gdb_edge_spec const &spec,
                      kimix::graph_db::PropertyList &out) {
    out.clear();
    if (spec.properties == nullptr || spec.property_count == 0) {
        return;
    }
    out.reserve(spec.property_count);
    for (size_t i = 0; i < spec.property_count; ++i) {
        kimix::graph_db::PropertyEntry entry;
        entry.name = spec.property_names != nullptr ? spec.property_names[i] : 0;
        entry.value = to_value(spec.properties[i]);
        out.push_back(std::move(entry));
    }
}

} // namespace

// ===========================================================================
// Library / ABI queries
// ===========================================================================

extern "C" {

uint32_t kimix_gdb_abi_version(void) {
    return KIMIX_GDB_ABI_VERSION;
}

const char *kimix_gdb_version_string(void) {
    return KIMIX_GRAPH_DB_VERSION;
}

int32_t kimix_gdb_layout_info(kimix_gdb_layout *out) {
    if (out == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    out->abi_version = KIMIX_GDB_ABI_VERSION;
    out->sizeof_property = static_cast<uint32_t>(sizeof(kimix_gdb_property));
    out->sizeof_property_record = static_cast<uint32_t>(sizeof(kimix_gdb_property_record));
    out->sizeof_vertex_key = static_cast<uint32_t>(sizeof(kimix_gdb_vertex_key));
    out->sizeof_edge_record = static_cast<uint32_t>(sizeof(kimix_gdb_edge_record));
    out->sizeof_property_type = static_cast<uint32_t>(sizeof(kimix_gdb_property_type));
    out->sizeof_capability = static_cast<uint32_t>(sizeof(kimix_gdb_capability));
    out->sizeof_vertex_spec = static_cast<uint32_t>(sizeof(kimix_gdb_vertex_spec));
    out->sizeof_size_t = static_cast<uint32_t>(sizeof(size_t));
    out->alignof_guid = static_cast<uint32_t>(alignof(kimix_gdb_guid));
    return KIMIX_GDB_OK;
}

const char *kimix_gdb_status_name(int32_t status) {
    return kimix::graph_db::status_name(static_cast<GdiStatus>(status));
}

// ===========================================================================
// Session lifecycle
// ===========================================================================

int32_t kimix_gdb_open(const kimix_gdb_open_options *options, kimix_gdb_session **out) {
    if (out == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix_gdb_open_options defaults{};
    defaults.auto_create_schema = 1;
    defaults.key_mode = 0;
    defaults.allow_parallel_edges = 1;
    defaults.allow_self_edges = 1;
    defaults.max_journal_bytes = 64ull * 1024ull * 1024ull;
    kimix_gdb_open_options const &request = options != nullptr ? *options : defaults;

    kimix::graph_db::GraphOptions graph_options;
    graph_options.auto_create_schema = request.auto_create_schema != 0;
    graph_options.key_mode = request.key_mode == 1 ? kimix::graph_db::KeyMode::report_warning
                                                   : kimix::graph_db::KeyMode::enforce_unique;
    graph_options.allow_parallel_edges = request.allow_parallel_edges != 0;
    graph_options.allow_self_edges = request.allow_self_edges != 0;
    graph_options.persist_on_commit = request.persist_on_commit != 0;
    graph_options.max_journal_bytes =
        request.max_journal_bytes != 0 ? request.max_journal_bytes : 64ull * 1024ull * 1024ull;
    graph_options.cold_map_size = request.cold_map_size;

    kimix_gdb_session *session = new kimix_gdb_session();
    kimix::filesystem::path directory;
    if (request.directory != nullptr && request.directory[0] != '\0') {
        if (!kimix::path_from_narrow(request.directory, directory)) {
            delete session;
            return KIMIX_GDB_ERR_ARGUMENT;
        }
        graph_options.graph_name = request.directory;
    }
    GdiStatus const status = session->database.open(directory, graph_options);
    if (is_error(status)) {
        delete session;
        return code_of(status);
    }
    session->open = true;
    *out = session;
    return KIMIX_GDB_OK;
}

void kimix_gdb_close(kimix_gdb_session *session) {
    if (session == nullptr) {
        return;
    }
    session->database.close();
    session->open = false;
    delete session;
}

int32_t kimix_gdb_capabilities(kimix_gdb_session *session, kimix_gdb_capability *out) {
    if (session == nullptr || out == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::Capability const capability = session->database.capabilities();
    out->supports_global_scan = capability.supports_global_scan ? 1 : 0;
    out->supports_edge_by_stable_uid = capability.supports_edge_by_stable_uid ? 1 : 0;
    out->supports_edge_property_read = capability.supports_edge_property_read ? 1 : 0;
    out->supports_native_multi_hop = capability.supports_native_multi_hop ? 1 : 0;
    out->max_native_hop_depth = capability.max_native_hop_depth;
    out->supports_global_aggregates = capability.supports_global_aggregates ? 1 : 0;
    out->supports_collective_transactions = capability.supports_collective_transactions ? 1 : 0;
    out->enforces_unique_external_ids = capability.enforces_unique_external_ids ? 1 : 0;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_persist(kimix_gdb_session *session, uint8_t full) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.persist(full != 0));
}

int32_t kimix_gdb_load(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.load_store());
}

// ===========================================================================
// Vertices
// ===========================================================================

int32_t kimix_gdb_vertex_create(kimix_gdb_session *session, const kimix_gdb_vertex_spec *spec,
                                kimix_gdb_guid *out_uid, uint8_t *out_warning) {
    if (session == nullptr || spec == nullptr ||
        (spec->labels == nullptr && spec->label_count != 0) ||
        (spec->properties == nullptr && spec->property_count != 0) ||
        (spec->property_names == nullptr && spec->property_count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::PropertyList properties;
    named_properties(*spec, properties);
    kimix::graph_db::VertexCreateResult created;
    GdiStatus const status = session->database.create_vertex(
        VertexKeyView{spec->key.label, to_bytes(spec->key.external_id)},
        kimix::span<const LabelId>{spec->labels, spec->label_count}, &properties, created);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_uid != nullptr) {
        *out_uid = from_guid(created.uid);
    }
    if (out_warning != nullptr) {
        *out_warning = created.non_unique_id_warning ? 1 : 0;
    }
    return code_of(status);
}

int32_t kimix_gdb_vertex_get_or_create(kimix_gdb_session *session,
                                       const kimix_gdb_vertex_spec *spec,
                                       kimix_gdb_guid *out_uid, uint8_t *out_created) {
    if (session == nullptr || spec == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::PropertyList properties;
    named_properties(*spec, properties);
    kimix::graph_db::VertexMergeResult merged;
    GdiStatus const status = session->database.get_or_create_vertex(
        VertexKeyView{spec->key.label, to_bytes(spec->key.external_id)},
        kimix::span<const LabelId>{spec->labels, spec->label_count}, &properties, merged);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_uid != nullptr) {
        *out_uid = from_guid(merged.uid);
    }
    if (out_created != nullptr) {
        *out_created = merged.created ? 1 : 0;
    }
    return code_of(status);
}

int32_t kimix_gdb_vertex_translate(kimix_gdb_session *session, const kimix_gdb_vertex_key *key,
                                   kimix_gdb_guid *out_uid, uint8_t *out_found) {
    if (session == nullptr || key == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid resolved;
    bool found = false;
    GdiStatus const status = session->database.translate_vertex_id(
        VertexKeyView{key->label, to_bytes(key->external_id)}, resolved, found);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_found != nullptr) {
        *out_found = found ? 1 : 0;
    }
    if (found && out_uid != nullptr) {
        *out_uid = from_guid(resolved);
    }
    return code_of(status);
}

int32_t kimix_gdb_vertex_free(kimix_gdb_session *session, kimix_gdb_guid vertex) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.free_vertex(to_guid(vertex)));
}

int32_t kimix_gdb_vertex_free_guarded(kimix_gdb_session *session, kimix_gdb_guid vertex) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.free_vertex_guarded(to_guid(vertex)));
}

// ===========================================================================
// Labels
// ===========================================================================

int32_t kimix_gdb_vertex_label_add(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                   uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.add_label(to_guid(vertex), label));
}

int32_t kimix_gdb_vertex_label_remove(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.remove_label(to_guid(vertex), label));
}

int32_t kimix_gdb_vertex_labels(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                uint32_t *labels, size_t capacity, size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const uid = to_guid(vertex);
    kimix::graph_db::Vertex *record = session->database.graph().find(uid);
    if (record == nullptr) {
        return KIMIX_GDB_ERR_VERTEX;
    }
    kimix::vector<LabelId> ids;
    record->labels(ids);
    // The fetch below writes the key label first and skips it inside `ids`, so
    // the probe must count exactly that.  Normally _labels does NOT carry the
    // key label (it lives in _key_label), but a create whose label list names
    // the key label stores it there too — count it once, not twice.
    bool const key_in_labels =
        record->key_label() != 0 &&
        std::find(ids.begin(), ids.end(), record->key_label()) != ids.end();
    size_t const total = ids.size() + (record->key_label() != 0 && !key_in_labels ? 1 : 0);
    *out_count = total;
    if (labels == nullptr) {
        return KIMIX_GDB_OK; // size query
    }
    if (capacity < total) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    size_t cursor = 0;
    if (record->key_label() != 0) {
        labels[cursor++] = record->key_label(); // the key label comes FIRST
    }
    for (auto id : ids) {
        if (id == record->key_label()) {
            continue;
        }
        labels[cursor++] = id;
    }
    *out_count = cursor;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_edge_label_add(kimix_gdb_session *session, kimix_gdb_guid edge,
                                 uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.add_edge_label(to_guid(edge), label));
}

int32_t kimix_gdb_edge_label_remove(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.remove_edge_label(to_guid(edge), label));
}

int32_t kimix_gdb_edge_labels(kimix_gdb_session *session, kimix_gdb_guid edge,
                              uint32_t *labels, size_t capacity, size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::vector<LabelId> ids;
    if (!session->database.graph().edge_labels(to_guid(edge), ids)) {
        return KIMIX_GDB_ERR_UID;
    }
    *out_count = ids.size();
    if (labels == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (capacity < ids.size()) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        labels[i] = ids[i];
    }
    return KIMIX_GDB_OK;
}

// ===========================================================================
// Properties
// ===========================================================================

} // extern "C"

namespace {

int32_t vertex_property_write(kimix_gdb_session *session, kimix_gdb_guid vertex,
                              uint32_t name, kimix_gdb_property const *value, int mode) {
    if (session == nullptr || value == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const uid = to_guid(vertex);
    PropertyValue const decoded = to_value(*value);
    GdiStatus status = GdiStatus::ok;
    switch (mode) {
    case 0: status = session->database.add_property(uid, name, decoded); break;
    case 1: status = session->database.set_property(uid, name, decoded); break;
    default: status = session->database.update_property(uid, name, decoded); break;
    }
    return code_of(status);
}

int32_t vertex_properties_read(kimix_gdb_session *session, kimix_gdb_guid vertex,
                               void *raw_items, size_t capacity, uint8_t *scratch,
                               size_t scratch_capacity, size_t *out_count, size_t *out_scratch,
                               int what) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const uid = to_guid(vertex);
    if (!session->database.graph().contains(uid)) {
        return KIMIX_GDB_ERR_VERTEX;
    }
    if (what == 0) {
        kimix::vector<std::pair<PropNameId, PropertyValue>> properties;
        collect_vertex_properties(session->database, uid, properties);
        return code_of(write_properties(
            properties, static_cast<kimix_gdb_property_record *>(raw_items), capacity, scratch,
            scratch_capacity, out_count, out_scratch));
    }
      auto *items = static_cast<kimix_gdb_property_type *>(raw_items);
      // Property TYPES: the declaration of every property the vertex carries.
      kimix::graph_db::PropertyTypeList types;
      (void)session->database.get_property_types(uid, types);
      // PHASE 1 (both buffers NULL): sizes only, success — the documented probe.
      if (items == nullptr && scratch == nullptr) {
          size_t needed = 0;
          for (auto const &type : types) {
              needed += type.name.size();
          }
          if (out_count != nullptr) {
              *out_count = types.size();
          }
          if (out_scratch != nullptr) {
              *out_scratch = needed;
          }
          return KIMIX_GDB_OK;
      }
      ScratchWriter writer{scratch, scratch_capacity};
      if (out_count != nullptr) {
          *out_count = types.size();
    }
    GdiStatus status = GdiStatus::ok;
    for (size_t i = 0; i < types.size(); ++i) {
        uint32_t offset = 0;
        uint32_t length = 0;
        if (!writer.append_text(types[i].name, offset, length)) {
            status = GdiStatus::error_buffer;
        }
        if (items != nullptr && i < capacity) {
            items[i].id = types[i].name_id;
            items[i].container = static_cast<uint8_t>(types[i].container);
            items[i].dtype = static_cast<uint8_t>(types[i].data);
            items[i].reserved = 0;
            items[i].count = types[i].count;
            items[i].name_offset = offset;
            items[i].name_len = length;
        }
    }
    if (out_scratch != nullptr) {
        *out_scratch = writer.needed();
    }
    if (status == GdiStatus::ok && raw_items != nullptr && types.size() > capacity) {
        status = GdiStatus::error_buffer;
    }
    return code_of(status);
}

int32_t edge_property_write(kimix_gdb_session *session, kimix_gdb_guid edge, uint32_t name,
                            kimix_gdb_property const *value, int mode) {
    if (session == nullptr || value == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const id = to_guid(edge);
    PropertyValue const decoded = to_value(*value);
    GdiStatus status = GdiStatus::ok;
    switch (mode) {
    case 0: status = session->database.add_edge_property(id, name, decoded); break;
    case 1: status = session->database.set_edge_property(id, name, decoded); break;
    default: status = session->database.update_edge_property(id, name, decoded); break;
    }
    return code_of(status);
}

int32_t edge_properties_read(kimix_gdb_session *session, kimix_gdb_guid edge,
                             void *raw_items, size_t capacity, uint8_t *scratch,
                             size_t scratch_capacity, size_t *out_count, size_t *out_scratch,
                             int what) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid const id = to_guid(edge);
    if (!session->database.graph().edges().contains(id)) {
        return KIMIX_GDB_ERR_UID;
    }
    if (what == 0) {
        kimix::vector<std::pair<PropNameId, PropertyValue>> properties;
        collect_edge_properties(session->database, id, properties);
        return code_of(write_properties(
            properties, static_cast<kimix_gdb_property_record *>(raw_items), capacity, scratch,
            scratch_capacity, out_count, out_scratch));
    }
    auto *items = static_cast<kimix_gdb_property_type *>(raw_items);
    kimix::graph_db::PropertyTypeList types;
    (void)session->database.get_edge_property_types(id, types);
    // PHASE 1 (both buffers NULL): sizes only, success — the documented probe.
    if (items == nullptr && scratch == nullptr) {
        size_t needed = 0;
        for (auto const &type : types) {
            needed += type.name.size();
        }
        if (out_count != nullptr) {
            *out_count = types.size();
        }
        if (out_scratch != nullptr) {
            *out_scratch = needed;
        }
        return KIMIX_GDB_OK;
    }
    ScratchWriter writer{scratch, scratch_capacity};
    if (out_count != nullptr) {
        *out_count = types.size();
    }
    GdiStatus status = GdiStatus::ok;
    for (size_t i = 0; i < types.size(); ++i) {
        uint32_t offset = 0;
        uint32_t length = 0;
        if (!writer.append_text(types[i].name, offset, length)) {
            status = GdiStatus::error_buffer;
        }
        if (items != nullptr && i < capacity) {
            items[i].id = types[i].name_id;
            items[i].container = static_cast<uint8_t>(types[i].container);
            items[i].dtype = static_cast<uint8_t>(types[i].data);
            items[i].reserved = 0;
            items[i].count = types[i].count;
            items[i].name_offset = offset;
            items[i].name_len = length;
        }
    }
    if (out_scratch != nullptr) {
        *out_scratch = writer.needed();
    }
    if (status == GdiStatus::ok && raw_items != nullptr && types.size() > capacity) {
        status = GdiStatus::error_buffer;
    }
    return code_of(status);
}

} // namespace

extern "C" {

int32_t kimix_gdb_vertex_property_add(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t name, const kimix_gdb_property *value) {
    return vertex_property_write(session, vertex, name, value, 0);
}

int32_t kimix_gdb_vertex_property_set(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t name, const kimix_gdb_property *value) {
    return vertex_property_write(session, vertex, name, value, 1);
}

int32_t kimix_gdb_vertex_property_update(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                         uint32_t name, const kimix_gdb_property *value) {
    return vertex_property_write(session, vertex, name, value, 2);
}

int32_t kimix_gdb_vertex_properties_remove(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                           const uint32_t *names, size_t name_count) {
    if (session == nullptr || (names == nullptr && name_count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.remove_properties(
        to_guid(vertex), kimix::span<const PropNameId>{names, name_count}));
}

int32_t kimix_gdb_vertex_properties(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                    kimix_gdb_property_record *items, size_t capacity,
                                    uint8_t *scratch, size_t scratch_capacity,
                                    size_t *out_count, size_t *out_scratch_bytes) {
    return vertex_properties_read(session, vertex, items, capacity, scratch, scratch_capacity,
                                  out_count, out_scratch_bytes, 0);
}

int32_t kimix_gdb_vertex_property_types(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                        kimix_gdb_property_type *items, size_t capacity,
                                        uint8_t *scratch, size_t scratch_capacity,
                                        size_t *out_count, size_t *out_scratch_bytes) {
    return vertex_properties_read(session, vertex, items, capacity, scratch, scratch_capacity,
                                  out_count, out_scratch_bytes, 1);
}

int32_t kimix_gdb_edge_property_add(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t name, const kimix_gdb_property *value) {
    return edge_property_write(session, edge, name, value, 0);
}

int32_t kimix_gdb_edge_property_set(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t name, const kimix_gdb_property *value) {
    return edge_property_write(session, edge, name, value, 1);
}

int32_t kimix_gdb_edge_property_update(kimix_gdb_session *session, kimix_gdb_guid edge,
                                       uint32_t name, const kimix_gdb_property *value) {
    return edge_property_write(session, edge, name, value, 2);
}

int32_t kimix_gdb_edge_properties_remove(kimix_gdb_session *session, kimix_gdb_guid edge,
                                         const uint32_t *names, size_t name_count) {
    if (session == nullptr || (names == nullptr && name_count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.remove_edge_properties(
        to_guid(edge), kimix::span<const PropNameId>{names, name_count}));
}

int32_t kimix_gdb_edge_properties(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_property_record *items, size_t capacity,
                                  uint8_t *scratch, size_t scratch_capacity,
                                  size_t *out_count, size_t *out_scratch_bytes) {
    return edge_properties_read(session, edge, items, capacity, scratch, scratch_capacity,
                                out_count, out_scratch_bytes, 0);
}

int32_t kimix_gdb_edge_property_types(kimix_gdb_session *session, kimix_gdb_guid edge,
                                      kimix_gdb_property_type *items, size_t capacity,
                                      uint8_t *scratch, size_t scratch_capacity,
                                      size_t *out_count, size_t *out_scratch_bytes) {
    return edge_properties_read(session, edge, items, capacity, scratch, scratch_capacity,
                                out_count, out_scratch_bytes, 1);
}

// ===========================================================================
// Edges
// ===========================================================================

int32_t kimix_gdb_edge_create(kimix_gdb_session *session, kimix_gdb_guid origin,
kimix_gdb_guid target, uint8_t direction, const uint32_t *labels,
size_t label_count, const kimix_gdb_property *properties,
size_t property_count, kimix_gdb_guid *out_edge) {
    if (session == nullptr || (labels == nullptr && label_count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix_gdb_edge_spec spec{};
    spec.origin = origin;
    spec.target = target;
    spec.direction = direction;
    spec.labels = labels;
    spec.label_count = label_count;
    spec.properties = properties;
    spec.property_names = nullptr;
    spec.property_count = 0; // unnamed properties are not accepted (see the header)
    (void)property_count;
    kimix::graph_db::PropertyList list;
    named_properties(spec, list);
    kimix::Guid edge;
    GdiStatus const status = session->database.edge_create(
        to_guid(origin), to_guid(target),
        direction == KIMIX_GDB_EDGE_UNDIRECTED ? EdgeDirectionType::undirected
                                               : EdgeDirectionType::directed,
        kimix::span<const LabelId>{labels, label_count}, &list, edge);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_edge != nullptr) {
        *out_edge = from_guid(edge);
    }
    return code_of(status);
}

int32_t kimix_gdb_edge_free(kimix_gdb_session *session, kimix_gdb_guid edge) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.edge_free(to_guid(edge)));
}

int32_t kimix_gdb_edge_set_origin(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_guid origin) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.set_edge_origin(to_guid(edge), to_guid(origin)));
}

int32_t kimix_gdb_edge_set_target(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_guid target) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.set_edge_target(to_guid(edge), to_guid(target)));
}

int32_t kimix_gdb_edge_set_direction(kimix_gdb_session *session, kimix_gdb_guid edge,
                                     uint8_t direction) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.set_edge_direction(
        to_guid(edge), direction == KIMIX_GDB_EDGE_UNDIRECTED ? EdgeDirectionType::undirected
                                                             : EdgeDirectionType::directed));
}

int32_t kimix_gdb_edge_endpoints(kimix_gdb_session *session, kimix_gdb_guid edge,
                                 kimix_gdb_guid *out_origin, kimix_gdb_guid *out_target) {
    if (session == nullptr || out_origin == nullptr || out_target == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::Guid origin;
    kimix::Guid target;
    GdiStatus const status = session->database.get_edge_endpoints(to_guid(edge), origin, target);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_origin = from_guid(origin);
    *out_target = from_guid(target);
    return KIMIX_GDB_OK;
}

// ===========================================================================
// Filtered, oriented, paged reads
// ===========================================================================

int32_t kimix_gdb_edges_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, int64_t cap,
                                  kimix_gdb_edge_record *items, size_t capacity,
                                  size_t *out_count, size_t *out_total) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint resolved;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    kimix::graph_db::EdgePage page;
    // Phase 1 (items == NULL) always asks for the count, so the caller learns the
    // size even when it passed no cap.
    int64_t const effective_cap = items == nullptr ? 0 : cap;
    status = session->database.get_edges_of_vertex(to_guid(vertex), resolved, direction,
                                                  effective_cap, page);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_total != nullptr) {
        *out_total = page.total_count;
    }
    if (out_count != nullptr) {
        *out_count = page.items.size();
    }
    if (items == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (page.items.size() > capacity) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < page.items.size(); ++i) {
        items[i].uid = from_guid(page.items[i].id);
        items[i].origin = from_guid(page.items[i].origin);
        items[i].target = from_guid(page.items[i].target);
        items[i].direction = page.items[i].direction == EdgeDirectionType::undirected
                                 ? KIMIX_GDB_EDGE_UNDIRECTED
                                 : KIMIX_GDB_EDGE_DIRECTED;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_neighbors_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t constraint, uint8_t orientation, int64_t cap,
                                      kimix_gdb_guid *items, size_t capacity, size_t *out_count,
                                      size_t *out_total) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint resolved;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    kimix::graph_db::VertexPage page;
    int64_t const effective_cap = items == nullptr ? 0 : cap;
    status = session->database.get_neighbor_vertices(to_guid(vertex), resolved, direction,
                                                    effective_cap, page);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_total != nullptr) {
        *out_total = page.total_count;
    }
    if (out_count != nullptr) {
        *out_count = page.items.size();
    }
    if (items == nullptr) {
        return KIMIX_GDB_OK;
    }
    if (page.items.size() > capacity) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    for (size_t i = 0; i < page.items.size(); ++i) {
        items[i] = from_guid(page.items[i]);
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_count_neighbors(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, size_t *out_count) {
    if (session == nullptr || out_count == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint resolved;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    return code_of(
        session->database.count_neighbors(to_guid(vertex), resolved, direction, *out_count));
}

int32_t kimix_gdb_exists_neighbor(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation,
                                  uint8_t *out_exists) {
    if (session == nullptr || out_exists == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    Constraint resolved;
    GdiStatus status = resolve_constraint(session, constraint, resolved);
    if (is_error(status)) {
        return code_of(status);
    }
    EdgeOrientation const direction = orientation == KIMIX_GDB_ORIENTATION_OUTGOING
                                          ? EdgeOrientation::outgoing
                                      : orientation == KIMIX_GDB_ORIENTATION_INCOMING
                                          ? EdgeOrientation::incoming
                                          : EdgeOrientation::both;
    bool exists = false;
    status = session->database.exists_neighbor(to_guid(vertex), resolved, direction, exists);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_exists = exists ? 1 : 0;
    return KIMIX_GDB_OK;
}

// ===========================================================================
// Transactions
// ===========================================================================

int32_t kimix_gdb_tx_begin(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.begin_transaction());
}

int32_t kimix_gdb_tx_commit(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.commit());
}

int32_t kimix_gdb_tx_abort(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.abort());
}

int32_t kimix_gdb_tx_begin_collective(kimix_gdb_session *session) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.begin_collective());
}

int32_t kimix_gdb_tx_close_collective(kimix_gdb_session *session, uint8_t commit_it) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.close_collective(commit_it != 0));
}

// ===========================================================================
// Schema
// ===========================================================================

int32_t kimix_gdb_label_create(kimix_gdb_session *session, const char *name, size_t name_len,
                               uint32_t *out_label) {
    if (session == nullptr || name == nullptr || out_label == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    LabelId id = 0;
    GdiStatus const status = session->database.create_label(
        kimix::string_view{name, name_len}, id);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_label = id;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_label_find(kimix_gdb_session *session, const char *name, size_t name_len,
                             uint32_t *out_label, uint8_t *out_found) {
    if (session == nullptr || name == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    LabelId id = 0;
    bool found = false;
    GdiStatus const status = session->database.get_label_by_name(
        kimix::string_view{name, name_len}, id, found);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_found != nullptr) {
        *out_found = found ? 1 : 0;
    }
    if (found && out_label != nullptr) {
        *out_label = id;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_label_get_all(kimix_gdb_session *session, kimix_gdb_label *items,
                                size_t capacity, uint8_t *scratch, size_t scratch_capacity,
                                size_t *out_count, size_t *out_scratch_bytes) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
      kimix::graph_db::NamedLabelList labels;
      GdiStatus status = session->database.get_all_labels(labels);
      if (is_error(status)) {
          return code_of(status);
      }
      // PHASE 1 (both buffers NULL): sizes only, success — the documented probe.
      if (items == nullptr && scratch == nullptr) {
          size_t needed = 0;
          for (auto const &label : labels) {
              needed += label.name.size();
          }
          if (out_count != nullptr) {
              *out_count = labels.size();
          }
          if (out_scratch_bytes != nullptr) {
              *out_scratch_bytes = needed;
          }
          return KIMIX_GDB_OK;
      }
      ScratchWriter writer{scratch, scratch_capacity};
      if (out_count != nullptr) {
          *out_count = labels.size();
      }
      for (size_t i = 0; i < labels.size(); ++i) {
          uint32_t offset = 0;
          uint32_t length = 0;
          if (!writer.append_text(labels[i].name, offset, length)) {
              status = GdiStatus::error_buffer;
          }
          if (items != nullptr && i < capacity) {
              items[i].id = labels[i].id;
              items[i].name_offset = offset;
              items[i].name_len = length;
          }
      }
    if (out_scratch_bytes != nullptr) {
        *out_scratch_bytes = writer.needed();
    }
    if (status == GdiStatus::ok && items != nullptr && labels.size() > capacity) {
        status = GdiStatus::error_buffer;
    }
    return code_of(status);
}

int32_t kimix_gdb_label_update(kimix_gdb_session *session, uint32_t label, const char *name,
                               size_t name_len) {
    if (session == nullptr || name == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.update_label(label, kimix::string_view{name, name_len}));
}

int32_t kimix_gdb_label_free(kimix_gdb_session *session, uint32_t label) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.free_label(label));
}

int32_t kimix_gdb_label_name(kimix_gdb_session *session, uint32_t label, char *out,
                             size_t capacity, size_t *out_required) {
    if (session == nullptr || out_required == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::string const name = session->database.schema().label_name(label);
    *out_required = name.size();
    if (out == nullptr || capacity < name.size()) {
        return KIMIX_GDB_ERR_BUFFER;
    }
    if (!name.empty()) {
        std::memcpy(out, name.data(), name.size());
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_property_type_create(kimix_gdb_session *session, const char *name,
                                       size_t name_len, uint8_t container, uint8_t dtype,
                                       uint32_t count, uint32_t *out_type) {
    if (session == nullptr || name == nullptr || out_type == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::PropertyTypeInfo info;
    info.name = kimix::string{kimix::string_view{name, name_len}};
    info.container = container == KIMIX_GDB_CONTAINER_ARRAY
                         ? kimix::graph_db::GdiContainerType::array
                         : kimix::graph_db::GdiContainerType::single;
    info.data = static_cast<kimix::graph_db::GdiDataType>(dtype);
    info.count = count;
    PropNameId id = 0;
    GdiStatus const status = session->database.create_property_type(info, id);
    if (is_error(status)) {
        return code_of(status);
    }
    *out_type = id;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_property_type_find(kimix_gdb_session *session, const char *name,
                                     size_t name_len, uint32_t *out_type, uint8_t *out_found) {
    if (session == nullptr || name == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    PropNameId id = 0;
    bool found = false;
    GdiStatus const status = session->database.get_property_type_by_name(
        kimix::string_view{name, name_len}, id, found);
    if (is_error(status)) {
        return code_of(status);
    }
    if (out_found != nullptr) {
        *out_found = found ? 1 : 0;
    }
    if (found && out_type != nullptr) {
        *out_type = id;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_property_type_get_all(kimix_gdb_session *session,
                                        kimix_gdb_property_type *items, size_t capacity,
                                        uint8_t *scratch, size_t scratch_capacity,
                                        size_t *out_count, size_t *out_scratch_bytes) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
      kimix::graph_db::PropertyTypeList types;
      GdiStatus status = session->database.get_all_property_types(types);
      if (is_error(status)) {
          return code_of(status);
      }
      // PHASE 1 (both buffers NULL): sizes only, success — the documented probe.
      if (items == nullptr && scratch == nullptr) {
          size_t needed = 0;
          for (auto const &type : types) {
              needed += type.name.size();
          }
          if (out_count != nullptr) {
              *out_count = types.size();
          }
          if (out_scratch_bytes != nullptr) {
              *out_scratch_bytes = needed;
          }
          return KIMIX_GDB_OK;
      }
      ScratchWriter writer{scratch, scratch_capacity};
      if (out_count != nullptr) {
          *out_count = types.size();
      }
      for (size_t i = 0; i < types.size(); ++i) {
          uint32_t offset = 0;
          uint32_t length = 0;
          if (!writer.append_text(types[i].name, offset, length)) {
              status = GdiStatus::error_buffer;
          }
          if (items != nullptr && i < capacity) {
              items[i].id = types[i].name_id;
              items[i].container = static_cast<uint8_t>(types[i].container);
              items[i].dtype = static_cast<uint8_t>(types[i].data);
              items[i].reserved = 0;
              items[i].count = types[i].count;
              items[i].name_offset = offset;
              items[i].name_len = length;
          }
      }
      if (out_scratch_bytes != nullptr) {
          *out_scratch_bytes = writer.needed();
      }
      if (status == GdiStatus::ok && items != nullptr && types.size() > capacity) {
          status = GdiStatus::error_buffer;
      }
      return code_of(status);
  }

int32_t kimix_gdb_property_type_update(kimix_gdb_session *session, uint32_t type,
                                       const char *name, size_t name_len, uint8_t container,
                                       uint8_t dtype, uint32_t count) {
    if (session == nullptr || name == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::graph_db::PropertyTypeInfo info;
    info.name = kimix::string{kimix::string_view{name, name_len}};
    info.container = container == KIMIX_GDB_CONTAINER_ARRAY
                         ? kimix::graph_db::GdiContainerType::array
                         : kimix::graph_db::GdiContainerType::single;
    info.data = static_cast<kimix::graph_db::GdiDataType>(dtype);
    info.count = count;
    info.name_id = type;
    return code_of(session->database.update_property_type(type, info));
}

int32_t kimix_gdb_property_type_free(kimix_gdb_session *session, uint32_t type) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    return code_of(session->database.free_property_type(type));
}

// ===========================================================================
// Constraints
// ===========================================================================

int32_t kimix_gdb_constraint_create(kimix_gdb_session *session, uint32_t *out_constraint) {
    if (session == nullptr || out_constraint == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    if (session->constraints.size() >= 256) {
        return KIMIX_GDB_ERR_SIZE_LIMIT; // a bounded registry, by design
    }
    ConstraintHandle handle;
    GdiStatus const status = session->database.create_constraint(Constraint{}, handle);
    if (is_error(status)) {
        return code_of(status);
    }
    ConstraintSlot slot;
    slot.handle = handle;
    slot.definition = Constraint{};
    session->constraints.push_back(std::move(slot));
    *out_constraint = handle.id;
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_constraint_add_label(kimix_gdb_session *session, uint32_t constraint,
                                       uint32_t label, uint8_t op) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    ConstraintSlot *slot = find_slot(session, constraint);
    if (slot == nullptr) {
        return KIMIX_GDB_ERR_CONSTRAINT;
    }
    GdiStatus const status =
        session->database.add_label_condition(slot->handle, label, static_cast<GdiOp>(op));
    if (is_error(status)) {
        return code_of(status);
    }
    // Keep the session's copy in step: the read commands resolve the DEFINITION
    // (the Database owns the registry, the ABI owns the mapping to its ids).
    Constraint current;
    if (session->database.schema().constraint(slot->handle, current)) {
        slot->definition = current;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_constraint_add_property(kimix_gdb_session *session, uint32_t constraint,
                                          uint32_t property, uint8_t op,
                                          const kimix_gdb_property *value, uint32_t count) {
    if (session == nullptr || value == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    ConstraintSlot *slot = find_slot(session, constraint);
    if (slot == nullptr) {
        return KIMIX_GDB_ERR_CONSTRAINT;
    }
    GdiStatus const status = session->database.add_property_condition(
        slot->handle, property, static_cast<GdiOp>(op), to_value(*value), count);
    if (is_error(status)) {
        return code_of(status);
    }
    Constraint current;
    if (session->database.schema().constraint(slot->handle, current)) {
        slot->definition = current;
    }
    return KIMIX_GDB_OK;
}

int32_t kimix_gdb_constraint_free(kimix_gdb_session *session, uint32_t constraint) {
    if (session == nullptr) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    for (size_t i = 0; i < session->constraints.size(); ++i) {
        if (session->constraints[i].handle.id != constraint) {
            continue;
        }
        GdiStatus const status = session->database.free_constraint(session->constraints[i].handle);
        session->constraints.erase(session->constraints.begin() + static_cast<ptrdiff_t>(i));
        return code_of(status);
    }
    return KIMIX_GDB_ERR_CONSTRAINT;
}

// ===========================================================================
// Bulk
// ===========================================================================

int32_t kimix_gdb_vertices_create_bulk(kimix_gdb_session *session,
                                       const kimix_gdb_vertex_spec *specs, size_t count,
                                       uint8_t continue_on_error, kimix_gdb_guid *out_uids,
                                       int32_t *out_status, uint8_t *out_warning) {
    if (session == nullptr || (specs == nullptr && count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::vector<kimix::graph_db::VertexBulkRow> rows;
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        kimix::graph_db::VertexBulkRow row;
        row.key.label = specs[i].key.label;
        row.key.external_id =
            kimix::graph_db::ExternalId::from_bytes(to_bytes(specs[i].key.external_id));
        for (size_t k = 0; k < specs[i].label_count; ++k) {
            row.labels.push_back(specs[i].labels[k]);
        }
        named_properties(specs[i], row.properties);
        row.content = to_bytes(specs[i].content);
        rows.push_back(std::move(row));
    }
    kimix::vector<kimix::graph_db::VertexCreateResult> results;
    kimix::vector<GdiStatus> statuses;
    kimix::graph_db::BulkStats stats;
    GdiStatus const status = session->database.create_vertices_bulk(
        kimix::span<kimix::graph_db::VertexBulkRow const>{rows.data(), rows.size()},
        continue_on_error != 0, results, &statuses, stats);
    for (size_t i = 0; i < results.size() && i < count; ++i) {
        if (out_uids != nullptr) {
            out_uids[i] = from_guid(results[i].uid);
        }
        if (out_status != nullptr && i < statuses.size()) {
            out_status[i] = code_of(statuses[i]);
        }
        if (out_warning != nullptr) {
            out_warning[i] = results[i].non_unique_id_warning ? 1 : 0;
        }
    }
    return is_error(status) ? code_of(status) : KIMIX_GDB_OK;
}

int32_t kimix_gdb_edges_create_bulk(kimix_gdb_session *session,
                                    const kimix_gdb_edge_spec *specs, size_t count,
                                    uint8_t continue_on_error, kimix_gdb_guid *out_uids,
                                    int32_t *out_status) {
    if (session == nullptr || (specs == nullptr && count != 0)) {
        return KIMIX_GDB_ERR_ARGUMENT;
    }
    kimix::vector<kimix::graph_db::EdgeBulkRow> rows;
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        kimix::graph_db::EdgeBulkRow row;
        row.origin = to_guid(specs[i].origin);
        row.target = to_guid(specs[i].target);
        row.direction = specs[i].direction == KIMIX_GDB_EDGE_UNDIRECTED
                            ? EdgeDirectionType::undirected
                            : EdgeDirectionType::directed;
        for (size_t k = 0; k < specs[i].label_count; ++k) {
            row.labels.push_back(specs[i].labels[k]);
        }
        named_properties(specs[i], row.properties);
        rows.push_back(std::move(row));
    }
    kimix::vector<kimix::Guid> edge_ids;
    kimix::vector<GdiStatus> statuses;
    kimix::graph_db::BulkStats stats;
    GdiStatus const status = session->database.create_edges_bulk(
        kimix::span<kimix::graph_db::EdgeBulkRow const>{rows.data(), rows.size()},
        continue_on_error != 0, edge_ids, &statuses, stats);
    for (size_t i = 0; i < edge_ids.size() && i < count; ++i) {
        if (out_uids != nullptr) {
            out_uids[i] = from_guid(edge_ids[i]);
        }
        if (out_status != nullptr && i < statuses.size()) {
            out_status[i] = code_of(statuses[i]);
        }
    }
    return is_error(status) ? code_of(status) : KIMIX_GDB_OK;
}

} // extern "C"
