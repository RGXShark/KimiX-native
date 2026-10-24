/*
 * property.h — the typed property model of kimix-graph-db (GDI §2.1).
 *
 * A Vertex or an Edge carries a PropertyBag: an interned-name -> PropertyValue
 * map. It sits BESIDE the legacy opaque content blob (Vertex::content()), which
 * stays exactly as it was, so the lower-level consumers and their tests keep
 * working and a GDI adapter still has a place for a raw payload that has no
 * schema.
 *
 * NAMES ARE INTERNED. A PropertyBag is keyed by PropNameId (a 4-byte id the
 * Schema allocates), never by a string: that keeps a record small, makes
 * Schema::rename_property_type O(1) (entities store the id, only the registry's
 * name table changes) and makes a property lookup one integer probe. Both
 * PropertyTypeInfo and the read-back API carry the name text as well, so a
 * caller that has an id can always recover the name without a second round trip.
 *
 * KINDS. PropertyKind mirrors Semevia.Gdi.PropertyValue.ValueKind exactly
 * (None/Int64/Double/String/Blob/Int64Array/DoubleArray). The enumerator
 * `double_value` spells ValueKind.Double and `int64_value` ValueKind.Int64 —
 * `double`/`int64` are unavailable as identifiers (a keyword and an MSVC
 * predefined type), and the C ABI (graph_c_api.h) uses the same spellings.
 *
 * STORAGE / TRADEOFF. Scalars are inline; string/blob payloads and the two
 * array kinds live in kimix::vector members, i.e. a PropertyValue is ~96 bytes
 * and a copy of a blob value copies the blob. That is the deliberate choice: it
 * is value-semantics correct, allocation-free for scalars, and needs no arena
 * lifetime rules. A 24-byte arena-backed variant is a later optimization epoch,
 * not a correctness prerequisite — the record codec (utils.h) writes the
 * payload inline either way.
 *
 * Example:
 *   kimix::graph_db::PropertyValue v = kimix::graph_db::PropertyValue::make_int64(42);
 *   kimix::graph_db::PropertyValue s = kimix::graph_db::PropertyValue::make_string("hello");
 *   if (kimix::graph_db::evaluate(kimix::graph_db::GdiOp::greater, v,
 *                                 kimix::graph_db::PropertyValue::make_int64(7))) { ... }
 */
#pragma once

#include "graph_db_api.h"

#include <core/stl/memory.h>
#include <core/stl/optional.h>
#include <core/stl/string.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

// Interned name ids: one dense id space each, allocated by the Schema and never
// recycled (a freed label/property-type leaves its id dead forever, so a stale
// id can never silently address a new name). 0 is "no id".
using LabelId = uint32_t;
using PropNameId = uint32_t;
inline constexpr LabelId kNoLabel = 0;
inline constexpr PropNameId kNoPropName = 0;

// ---------------------------------------------------------------------------
// GDI type vocabulary (values pinned to Semevia.Gdi)
// ---------------------------------------------------------------------------

// GDI_EDGE_DIRECTED-class datatypes (gdi_datatype.h). Only the ones the property
// model can actually carry are meaningful here; the rest exist so a declaration
// round-trips verbatim.
enum class GdiDataType : uint8_t {
    char_ = 100,
    int8_ = 101,
    uint8_ = 102,
    boolean = 103,
    byte_ = 104,
    int16_ = 105,
    uint16_ = 106,
    int32_ = 107,
    uint32_ = 108,
    float_ = 109,
    date = 110,
    time = 111,
    int64_ = 112,
    uint64_ = 113,
    double_ = 114,
    datetime = 115,
    decimal = 116,
};

// Property container type (GDI "etype"): one value, or a fixed-length array.
enum class GdiContainerType : uint8_t {
    single = 0,
    array = 1,
};

// Comparison operations of a constraint condition (gdi_operation.h:
// GDI_EQUAL=0 ... GDI_EQSMALLER=5).
enum class GdiOp : uint8_t {
    equal = 0,
    not_equal = 1,
    greater = 2,
    greater_or_equal = 3,
    less = 4,
    less_or_equal = 5,
};

[[nodiscard]] KIMIX_GRAPH_DB_API char const *op_name(GdiOp op) noexcept;
[[nodiscard]] KIMIX_GRAPH_DB_API char const *data_type_name(GdiDataType type) noexcept;
[[nodiscard]] KIMIX_GRAPH_DB_API char const *container_type_name(GdiContainerType type) noexcept;

// ---------------------------------------------------------------------------
// PropertyValue
// ---------------------------------------------------------------------------

// Which payload a PropertyValue carries — Semevia.Gdi.PropertyValue.ValueKind.
enum class PropertyKind : uint8_t {
    none = 0,
    int64_value = 1,
    double_value = 2,
    string = 3,
    blob = 4,
    int64_array = 5,
    double_array = 6,
};

[[nodiscard]] KIMIX_GRAPH_DB_API char const *property_kind_name(PropertyKind kind) noexcept;

/*
 * One typed value. Value semantics: a copy owns its payloads, so an entity may
 * hand out copies freely. Every accessor is total — asking for the wrong kind
 * returns a zeroed/empty value instead of asserting, because the C ABI must
 * never abort on a caller mistake (the Database validates kinds and returns
 * error_wrong_type long before an accessor is reached; a short-circuit `as_*`
 * read of a mismatched value is a bug in this library, not a trap for the
 * adapter).
 */
class KIMIX_GRAPH_DB_API PropertyValue {
public:
    PropertyValue() noexcept = default;

    static PropertyValue make_int64(int64_t value) noexcept;
    static PropertyValue make_double(double value) noexcept;
    static PropertyValue make_string(kimix::string_view value);
    static PropertyValue make_blob(kimix::span<const std::byte> value);
    static PropertyValue make_int64_array(kimix::span<const int64_t> value);
    static PropertyValue make_double_array(kimix::span<const double> value);

    [[nodiscard]] PropertyKind kind() const noexcept { return _kind; }
    [[nodiscard]] bool is_none() const noexcept { return _kind == PropertyKind::none; }

  [[nodiscard]] int64_t as_int64() const noexcept {
      return _kind == PropertyKind::int64_value ? _payload.integer : 0;
  }
  [[nodiscard]] double as_double() const noexcept {
      return _kind == PropertyKind::double_value ? _payload.real : 0.0;
  }
    // UTF-8 text of a string value (empty for any other kind). Valid until this
    // value is modified or destroyed.
    [[nodiscard]] kimix::string_view as_string() const noexcept;
    [[nodiscard]] kimix::span<const std::byte> as_blob() const noexcept;
    [[nodiscard]] kimix::span<const int64_t> as_int64_array() const noexcept;
    [[nodiscard]] kimix::span<const double> as_double_array() const noexcept;

    // Number of elements of an array value, 0 for a scalar.
    [[nodiscard]] size_t element_count() const noexcept;

    // Value equality — the C# reference's Equals, including the array kinds.
    [[nodiscard]] bool equals(PropertyValue const &other) const noexcept;    // Rule of five (plan v3 §5.4): the value owns its variable-size payload
    // through the tagged union below, so copying and destruction manage the
    // heap block explicitly.
    PropertyValue(PropertyValue const &other);
    PropertyValue &operator=(PropertyValue const &other);
    PropertyValue(PropertyValue &&other) noexcept;
    PropertyValue &operator=(PropertyValue &&other) noexcept;
    ~PropertyValue();

    // The GDI datatype this value's payload corresponds to (int64 -> Int64,
    // double_value -> Double, string -> Char, blob -> Byte,
    // int64_array -> Int64, double_array -> Double). Used by the schema's
    // declaration check (error_wrong_type).
    [[nodiscard]] GdiDataType data_type() const noexcept;

    // True when this value satisfies a PropertyTypeInfo's container/dtype/count
    // declaration.
    [[nodiscard]] bool matches_declaration(GdiContainerType container, GdiDataType data,
                                           uint32_t count) const noexcept;    void clear() noexcept;
private:
    // Frees any owned payload block and resets to `none`.
    void _release() noexcept;
    // Deep-copies `other`'s payload into this (already released) value.
    void _clone_from(PropertyValue const &other);    // PLAN V3 §5.4: the payload is ONE tagged union (24 B total) instead of
    // three kimix::vectors (~96 B, nearly all of it dead weight for the scalar
    // kinds that dominate real bags). The variable-size kinds own a single
    // heap block; `count` is in ELEMENTS, not bytes.
    PropertyKind _kind{PropertyKind::none};
    union Payload {
        int64_t integer;
        double real;
        struct {
            std::byte *data;
            size_t count;
        } bytes; // string (UTF-8, no NUL) or blob; `_kind` disambiguates
        struct {
            int64_t *data;
            size_t count;
        } int64s;
        struct {
            double *data;
            size_t count;
        } doubles;
        Payload() noexcept : integer(0) {}
        ~Payload() {}
    } _payload;
};

// Result of compare_values(): negative / zero / positive ordering.
[[nodiscard]] KIMIX_GRAPH_DB_API kimix::optional<int32_t>
compare_values(PropertyValue const &lhs, PropertyValue const &rhs) noexcept;

// Evaluates one property condition: `actual <op> expected`, reproducing
// Semevia.Gdi's InMemoryStore.Compare to the letter:
//   - numeric kinds compare across int64/double (an int64 is widened);
//   - two strings compare ordinally (byte-wise here, which is the same order for
//     every UTF-8 sequence, and therefore for every ASCII name);
//   - everything else (blob, arrays, mixed kinds) supports equality ONLY, and
//     any ordering op on them is false rather than an error.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
evaluate(GdiOp op, PropertyValue const &actual, PropertyValue const &expected) noexcept;

// ---------------------------------------------------------------------------
// Bags and lists
// ---------------------------------------------------------------------------

// Interned-name -> value. kimix::unordered_map = ankerl::unordered_dense, so a
// small bag is one flat array, and iteration order is insertion order (the tests
// sort before comparing, but an unfiltered read-back therefore stays stable).
using PropertyBag = kimix::unordered_map<PropNameId, PropertyValue>;

// One (id, value) pair of a read-back. The Database turns these into
// name/value pairs through the Schema.
struct KIMIX_GRAPH_DB_API PropertyEntry {
    PropNameId name{kNoPropName};
    PropertyValue value;
};
using PropertyList = kimix::vector<PropertyEntry>;

// A property type's declaration: GDI_CreatePropertyType's name/etype/dtype/count
// triple. `count` is the array length for an array declaration and 1 for a
// single value (GDI requires >= 1).
struct KIMIX_GRAPH_DB_API PropertyTypeInfo {
    PropNameId name_id{kNoPropName};
    kimix::string name;
    GdiContainerType container{GdiContainerType::single};
    GdiDataType data{GdiDataType::int64_};
    uint32_t count{1};
};
using PropertyTypeList = kimix::vector<PropertyTypeInfo>;

// ---------------------------------------------------------------------------
// Record-level property codec (shared by the vertex and edge records)
// ---------------------------------------------------------------------------

/*
 * One encoded property entry, little-endian with no padding:
 *
 *   [4B name id][1B kind][1B dtype][2B reserved][4B count][8B payload bytes][payload]
 *
 * `count` is the element count of an array value and 1 for every scalar (a
 * string's BYTE length is the payload length, not the count), so a decoder can
 * size the payload before reading it. `dtype` is the value's GDI datatype, kept
 * so a record read back without a schema still describes itself.
 *
 * encoded_property_entry_size() is what a record encoder sizes its buffer with,
 * so the layout has exactly one implementation.
 */
[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_property_entry_size(PropertyEntry const &entry) noexcept;
KIMIX_GRAPH_DB_API void encode_property_entry(std::byte *dst,
                                              PropertyEntry const &entry) noexcept;
// Partial decode: returns false (leaving `out` default-constructed) when the
// entry does not fit, and never reads outside [cursor, end).
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_property_entry(std::byte const *&cursor, std::byte const *end,
                      PropertyEntry &out) noexcept;

} // namespace kimix::graph_db
