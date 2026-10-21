/*
 * property.cpp — PropertyValue construction, access, equality and the GDI
 * comparison semantics (see property.h for the contract).
 *
 * Nothing here allocates on a failure path, nothing throws, and every accessor
 * is total: a kind mismatch yields a zeroed/empty value.
 */
#include "binary_le.h"
#include "property.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace kimix::graph_db {

namespace {

// Byte view of a string value (the payload vector stores UTF-8 bytes verbatim).
inline kimix::span<const std::byte> bytes_of(kimix::string_view text) noexcept {
    return kimix::span<const std::byte>{
        reinterpret_cast<std::byte const *>(text.data()), text.size()};
}

inline kimix::string_view text_of(kimix::vector<std::byte> const &bytes) noexcept {
    if (bytes.empty()) {
        return {};
    }
    return kimix::string_view{reinterpret_cast<char const *>(bytes.data()), bytes.size()};
}

inline bool same_bytes(kimix::vector<std::byte> const &lhs,
                       kimix::vector<std::byte> const &rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.empty()) {
        return true;
    }
    return std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

template<typename T>
inline bool same_sequence(kimix::vector<T> const &lhs, kimix::vector<T> const &rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

} // namespace

// ---------------------------------------------------------------------------
// Type vocabulary names
// ---------------------------------------------------------------------------

char const *op_name(GdiOp op) noexcept {
    switch (op) {
    case GdiOp::equal: return "equal";
    case GdiOp::not_equal: return "not_equal";
    case GdiOp::greater: return "greater";
    case GdiOp::greater_or_equal: return "greater_or_equal";
    case GdiOp::less: return "less";
    case GdiOp::less_or_equal: return "less_or_equal";
    }
    return "op_unknown";
}

char const *data_type_name(GdiDataType type) noexcept {
    switch (type) {
    case GdiDataType::char_: return "char";
    case GdiDataType::int8_: return "int8";
    case GdiDataType::uint8_: return "uint8";
    case GdiDataType::boolean: return "bool";
    case GdiDataType::byte_: return "byte";
    case GdiDataType::int16_: return "int16";
    case GdiDataType::uint16_: return "uint16";
    case GdiDataType::int32_: return "int32";
    case GdiDataType::uint32_: return "uint32";
    case GdiDataType::float_: return "float";
    case GdiDataType::date: return "date";
    case GdiDataType::time: return "time";
    case GdiDataType::int64_: return "int64";
    case GdiDataType::uint64_: return "uint64";
    case GdiDataType::double_: return "double";
    case GdiDataType::datetime: return "datetime";
    case GdiDataType::decimal: return "decimal";
    }
    return "datatype_unknown";
}

char const *container_type_name(GdiContainerType type) noexcept {
    switch (type) {
    case GdiContainerType::single: return "single";
    case GdiContainerType::array: return "array";
    }
    return "container_unknown";
}

char const *property_kind_name(PropertyKind kind) noexcept {
    switch (kind) {
    case PropertyKind::none: return "none";
    case PropertyKind::int64_value: return "int64";
    case PropertyKind::double_value: return "double";
    case PropertyKind::string: return "string";
    case PropertyKind::blob: return "blob";
    case PropertyKind::int64_array: return "int64_array";
    case PropertyKind::double_array: return "double_array";
    }
    return "kind_unknown";
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PropertyValue PropertyValue::make_int64(int64_t value) noexcept {
    PropertyValue out;
    out._kind = PropertyKind::int64_value;
    out._integer = value;
    return out;
}

PropertyValue PropertyValue::make_double(double value) noexcept {
    PropertyValue out;
    out._kind = PropertyKind::double_value;
    out._double = value;
    return out;
}

PropertyValue PropertyValue::make_string(kimix::string_view value) {
    PropertyValue out;
    out._kind = PropertyKind::string;
    auto const bytes = bytes_of(value);
    out._bytes.assign(bytes.begin(), bytes.end());
    return out;
}

PropertyValue PropertyValue::make_blob(kimix::span<const std::byte> value) {
    PropertyValue out;
    out._kind = PropertyKind::blob;
    out._bytes.assign(value.begin(), value.end());
    return out;
}

PropertyValue PropertyValue::make_int64_array(kimix::span<const int64_t> value) {
    PropertyValue out;
    out._kind = PropertyKind::int64_array;
    out._int64_array.assign(value.begin(), value.end());
    return out;
}

PropertyValue PropertyValue::make_double_array(kimix::span<const double> value) {
    PropertyValue out;
    out._kind = PropertyKind::double_array;
    out._double_array.assign(value.begin(), value.end());
    return out;
}

void PropertyValue::clear() noexcept {
    _kind = PropertyKind::none;
    _integer = 0;
    _double = 0.0;
    _bytes.clear();
    _int64_array.clear();
    _double_array.clear();
}

// ---------------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------------

kimix::string_view PropertyValue::as_string() const noexcept {
    return _kind == PropertyKind::string ? text_of(_bytes) : kimix::string_view{};
}

kimix::span<const std::byte> PropertyValue::as_blob() const noexcept {
    if (_kind != PropertyKind::blob) {
        return {};
    }
    if (_bytes.empty()) {
        return {};
    }
    return kimix::span<const std::byte>{_bytes.data(), _bytes.size()};
}

kimix::span<const int64_t> PropertyValue::as_int64_array() const noexcept {
    if (_kind != PropertyKind::int64_array || _int64_array.empty()) {
        return {};
    }
    return kimix::span<const int64_t>{_int64_array.data(), _int64_array.size()};
}

kimix::span<const double> PropertyValue::as_double_array() const noexcept {
    if (_kind != PropertyKind::double_array || _double_array.empty()) {
        return {};
    }
    return kimix::span<const double>{_double_array.data(), _double_array.size()};
}

size_t PropertyValue::element_count() const noexcept {
    switch (_kind) {
    case PropertyKind::int64_array: return _int64_array.size();
    case PropertyKind::double_array: return _double_array.size();
    case PropertyKind::none: return 0;
    default: return 0;
    }
}

bool PropertyValue::equals(PropertyValue const &other) const noexcept {
    if (_kind != other._kind) {
        return false;
    }
    switch (_kind) {
    case PropertyKind::none: return true;
    case PropertyKind::int64_value: return _integer == other._integer;
    case PropertyKind::double_value: return _double == other._double;
    case PropertyKind::string:
    case PropertyKind::blob: return same_bytes(_bytes, other._bytes);
    case PropertyKind::int64_array: return same_sequence(_int64_array, other._int64_array);
    case PropertyKind::double_array: return same_sequence(_double_array, other._double_array);
    }
    return false;
}

GdiDataType PropertyValue::data_type() const noexcept {
    switch (_kind) {
    case PropertyKind::int64_value:
    case PropertyKind::int64_array:
        return GdiDataType::int64_;
    case PropertyKind::double_value:
    case PropertyKind::double_array:
        return GdiDataType::double_;
    case PropertyKind::string:
        return GdiDataType::char_;
    case PropertyKind::blob:
        return GdiDataType::byte_;
    case PropertyKind::none:
        return GdiDataType::int64_; // unreachable: `none` never validates
    }
    return GdiDataType::int64_;
}

bool PropertyValue::matches_declaration(GdiContainerType container, GdiDataType data,
                                        uint32_t count) const noexcept {
    if (_kind == PropertyKind::none) {
        return false;
    }
    bool const is_array = _kind == PropertyKind::int64_array || _kind == PropertyKind::double_array;
    if (is_array != (container == GdiContainerType::array)) {
        return false;
    }
    if (data_type() != data) {
        return false;
    }
    if (is_array) {
        return element_count() == static_cast<size_t>(count);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Comparison (Semevia.Gdi's InMemoryStore.Compare)
// ---------------------------------------------------------------------------

kimix::optional<int32_t> compare_values(PropertyValue const &lhs,
                                        PropertyValue const &rhs) noexcept {
    PropertyKind const left = lhs.kind();
    PropertyKind const right = rhs.kind();
    // Numeric cross-kind comparison: an int64 is widened to double, exactly like
    // the reference's `(double)actual.AsInt64`.
    if (left == PropertyKind::int64_value && right == PropertyKind::int64_value) {
        int64_t const a = lhs.as_int64();
        int64_t const b = rhs.as_int64();
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    if (left == PropertyKind::double_value && right == PropertyKind::double_value) {
        double const a = lhs.as_double();
        double const b = rhs.as_double();
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    if (left == PropertyKind::int64_value && right == PropertyKind::double_value) {
        double const a = static_cast<double>(lhs.as_int64());
        double const b = rhs.as_double();
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    if (left == PropertyKind::double_value && right == PropertyKind::int64_value) {
        double const a = lhs.as_double();
        double const b = static_cast<double>(rhs.as_int64());
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    if (left == PropertyKind::string && right == PropertyKind::string) {
        auto const a = lhs.as_string();
        auto const b = rhs.as_string();
        int const order = a.compare(b);
        if (order < 0) {
            return -1;
        }
        if (order > 0) {
            return 1;
        }
        return 0;
    }
    // Same-but-unorderable (blob, arrays) is a defined equality; mixed kinds are
    // unequal. Neither supports an ordering.
    if (lhs.equals(rhs)) {
        return 0;
    }
    return std::nullopt;
}

bool evaluate(GdiOp op, PropertyValue const &actual, PropertyValue const &expected) noexcept {
    auto const order = compare_values(actual, expected);
    if (!order) {
        // Unorderable pair: only (in)equality is meaningful.
        bool const same = actual.equals(expected);
        if (op == GdiOp::equal) {
            return same;
        }
        if (op == GdiOp::not_equal) {
            return !same;
        }
        return false;
    }
    int32_t const value = *order;
    switch (op) {
    case GdiOp::equal: return value == 0;
    case GdiOp::not_equal: return value != 0;
    case GdiOp::greater: return value > 0;
    case GdiOp::greater_or_equal: return value >= 0;
    case GdiOp::less: return value < 0;
    case GdiOp::less_or_equal: return value <= 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Record-level property codec
// ---------------------------------------------------------------------------

namespace {

// Bytes of the value payload (the scalars are 8 fixed bytes, the arrays 8 bytes
// per element, the byte kinds their own length).
inline size_t payload_size(PropertyValue const &value) noexcept {
    switch (value.kind()) {
    case PropertyKind::none: return 0;
    case PropertyKind::int64_value:
    case PropertyKind::double_value: return 8;
    case PropertyKind::string: return value.as_string().size();
    case PropertyKind::blob: return value.as_blob().size();
    case PropertyKind::int64_array: return value.as_int64_array().size() * 8;
    case PropertyKind::double_array: return value.as_double_array().size() * 8;
    }
    return 0;
}

inline uint32_t payload_count(PropertyValue const &value) noexcept {
    switch (value.kind()) {
    case PropertyKind::int64_array:
    case PropertyKind::double_array:
        return static_cast<uint32_t>(value.element_count());
    default:
        return 1;
    }
}

inline uint64_t double_bits(double value) noexcept {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline double bits_double(uint64_t bits) noexcept {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

size_t encoded_property_entry_size(PropertyEntry const &entry) noexcept {
    // name + kind + dtype + reserved + count + payload length, then the payload.
    return 20 + payload_size(entry.value);
}

void encode_property_entry(std::byte *dst, PropertyEntry const &entry) noexcept {
    using namespace detail;
    write_u32(dst + 0, entry.name);
    write_u8(dst + 4, static_cast<uint8_t>(entry.value.kind()));
    write_u8(dst + 5, static_cast<uint8_t>(entry.value.data_type()));
    write_u16(dst + 6, 0); // reserved
    write_u32(dst + 8, payload_count(entry.value));
    write_u64(dst + 12, static_cast<uint64_t>(payload_size(entry.value)));
    std::byte *payload = dst + 20;
    switch (entry.value.kind()) {
    case PropertyKind::none:
        break;
    case PropertyKind::int64_value:
        write_u64(payload, static_cast<uint64_t>(entry.value.as_int64()));
        break;
    case PropertyKind::double_value:
        write_u64(payload, double_bits(entry.value.as_double()));
        break;
    case PropertyKind::string:
        write_bytes(payload, bytes_of(entry.value.as_string()));
        break;
    case PropertyKind::blob:
        write_bytes(payload, entry.value.as_blob());
        break;
    case PropertyKind::int64_array: {
        auto const values = entry.value.as_int64_array();
        for (size_t i = 0; i < values.size(); ++i) {
            write_u64(payload + i * 8, static_cast<uint64_t>(values[i]));
        }
        break;
    }
    case PropertyKind::double_array: {
        auto const values = entry.value.as_double_array();
        for (size_t i = 0; i < values.size(); ++i) {
            write_u64(payload + i * 8, double_bits(values[i]));
        }
        break;
    }
    }
}

bool decode_property_entry(std::byte const *&cursor, std::byte const *end,
                           PropertyEntry &out) noexcept {
    using namespace detail;
    out = PropertyEntry{};
    if (end - cursor < 20) {
        return false;
    }
    out.name = read_u32(cursor + 0);
    auto const kind = static_cast<PropertyKind>(read_u8(cursor + 4));
    uint32_t const count = read_u32(cursor + 8);
    uint64_t const payload_len = read_u64(cursor + 12);
    cursor += 20;
    if (payload_len > static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    std::byte const *payload = cursor;
    switch (kind) {
    case PropertyKind::none:
        out.value = PropertyValue{};
        break;
    case PropertyKind::int64_value:
        if (payload_len != 8) {
            return false;
        }
        out.value = PropertyValue::make_int64(static_cast<int64_t>(read_u64(payload)));
        break;
    case PropertyKind::double_value:
        if (payload_len != 8) {
            return false;
        }
        out.value = PropertyValue::make_double(bits_double(read_u64(payload)));
        break;
    case PropertyKind::string:
        out.value = PropertyValue::make_string(kimix::string_view{
            reinterpret_cast<char const *>(payload), static_cast<size_t>(payload_len)});
        break;
    case PropertyKind::blob:
        out.value = PropertyValue::make_blob(
            kimix::span<const std::byte>{payload, static_cast<size_t>(payload_len)});
        break;
    case PropertyKind::int64_array: {
        if (payload_len != static_cast<uint64_t>(count) * 8ull) {
            return false;
        }
        kimix::vector<int64_t> values;
        values.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            values.push_back(static_cast<int64_t>(read_u64(payload + i * 8)));
        }
        out.value = PropertyValue::make_int64_array(
            kimix::span<const int64_t>{values.data(), values.size()});
        break;
    }
    case PropertyKind::double_array: {
        if (payload_len != static_cast<uint64_t>(count) * 8ull) {
            return false;
        }
        kimix::vector<double> values;
        values.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            values.push_back(bits_double(read_u64(payload + i * 8)));
        }
        out.value = PropertyValue::make_double_array(
            kimix::span<const double>{values.data(), values.size()});
        break;
    }
    default:
        return false;
    }
    cursor += static_cast<size_t>(payload_len);
    return true;
}

} // namespace kimix::graph_db
