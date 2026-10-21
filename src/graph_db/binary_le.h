/*
 * binary_le.h — INTERNAL little-endian codec helpers shared by the record
 * encoders (utils.cpp, schema.cpp, edge.cpp, serialize.cpp).
 *
 * Not part of the public API and not pulled in by graph_db.h: it exists only so
 * the six record codecs do not each carry their own copy of "write a u32
 * little-endian". Everything here is `inline`, header-only, allocation-free and
 * noexcept; nothing is a file-scope static, so the unity build cannot produce a
 * duplicate definition.
 *
 * The format rules it enforces (see utils.h for the byte-by-byte record layout):
 *   - every multi-byte field is little-endian, on every host;
 *   - no struct is ever memcpy'd to or from disk;
 *   - every READ is bounds-checked by its caller before it happens, so a
 *     truncated or hostile buffer is a `false`, never an out-of-bounds read.
 */
#pragma once

#include <core/guid.h>
#include <core/md5.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kimix::graph_db::detail {

inline void write_u8(std::byte *dst, uint8_t value) noexcept {
    dst[0] = static_cast<std::byte>(value);
}

inline uint8_t read_u8(std::byte const *src) noexcept {
    return std::to_integer<uint8_t>(src[0]);
}

inline void write_u16(std::byte *dst, uint16_t value) noexcept {
    dst[0] = static_cast<std::byte>(value & 0xffu);
    dst[1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

inline uint16_t read_u16(std::byte const *src) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(std::to_integer<uint8_t>(src[0])) |
                                 (static_cast<uint16_t>(std::to_integer<uint8_t>(src[1])) << 8));
}

inline void write_u32(std::byte *dst, uint32_t value) noexcept {
    dst[0] = static_cast<std::byte>(value & 0xffu);
    dst[1] = static_cast<std::byte>((value >> 8) & 0xffu);
    dst[2] = static_cast<std::byte>((value >> 16) & 0xffu);
    dst[3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

inline uint32_t read_u32(std::byte const *src) noexcept {
    return static_cast<uint32_t>(std::to_integer<uint32_t>(src[0])) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[1])) << 8) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[2])) << 16) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[3])) << 24);
}

inline void write_u64(std::byte *dst, uint64_t value) noexcept {
    write_u32(dst + 0, static_cast<uint32_t>(value & 0xffffffffull));
    write_u32(dst + 4, static_cast<uint32_t>((value >> 32) & 0xffffffffull));
}

inline uint64_t read_u64(std::byte const *src) noexcept {
    return static_cast<uint64_t>(read_u32(src + 0)) |
           (static_cast<uint64_t>(read_u32(src + 4)) << 32);
}

// 128-bit values in the layout kimix::Guid and kimix::MD5 share: data0 then
// data1, each little-endian.
inline void write_guid(std::byte *dst, kimix::Guid const &guid) noexcept {
    auto const &words = guid.to_binary();
    write_u64(dst + 0, words.data0);
    write_u64(dst + 8, words.data1);
}

inline kimix::Guid read_guid(std::byte const *src) noexcept {
    return kimix::Guid{kimix::Guid::GuidData{read_u64(src + 0), read_u64(src + 8)}};
}

inline void write_md5(std::byte *dst, kimix::MD5 const &hash) noexcept {
    auto const &words = hash.to_binary();
    write_u64(dst + 0, words.data0);
    write_u64(dst + 8, words.data1);
}

inline kimix::MD5 read_md5(std::byte const *src) noexcept {
    return kimix::MD5{kimix::MD5::MD5Data{read_u64(src + 0), read_u64(src + 8)}};
}

inline void write_bytes(std::byte *dst, kimix::span<const std::byte> bytes) noexcept {
    if (!bytes.empty()) {
        std::memcpy(dst, bytes.data(), bytes.size());
    }
}

inline void write_text(std::byte *dst, kimix::string_view text) noexcept {
    if (!text.empty()) {
        std::memcpy(dst, text.data(), text.size());
    }
}

// A 32-bit length-prefixed byte string: [4B len][bytes]. Returns the advanced
// cursor; `dst` must already be sized by the caller's size computation.
inline std::byte *write_blob(std::byte *dst, kimix::span<const std::byte> bytes) noexcept {
    write_u32(dst, static_cast<uint32_t>(bytes.size()));
    write_bytes(dst + 4, bytes);
    return dst + 4 + bytes.size();
}

// Reads a length-prefixed byte string into `out` from `[cursor, end)`. Returns
// false (leaving `out` empty) when the declared length does not fit.
inline bool read_blob(std::byte const *&cursor, std::byte const *end,
                      kimix::vector<std::byte> &out) noexcept {
    if (end - cursor < 4) {
        return false;
    }
    uint32_t const length = read_u32(cursor);
    cursor += 4;
    if (static_cast<uint64_t>(length) > static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    out.assign(cursor, cursor + length);
    cursor += length;
    return true;
}

// The kimix::string spelling of read_blob (used by the schema tables).
inline bool read_text(std::byte const *&cursor, std::byte const *end,
                      kimix::string &out) noexcept {
    if (end - cursor < 4) {
        return false;
    }
    uint32_t const length = read_u32(cursor);
    cursor += 4;
    if (static_cast<uint64_t>(length) > static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    out.assign(reinterpret_cast<char const *>(cursor), length);
    cursor += length;
    return true;
}

} // namespace kimix::graph_db::detail
