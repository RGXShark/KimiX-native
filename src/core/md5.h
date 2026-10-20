/*
 * md5.h — kimix::MD5 — 128-bit MD5 message digest.
 *
 * MD5 stores the digest as two uint64_t words (MD5::MD5Data) and builds it in
 * one shot from a string, a string_view or a byte span. The digest is written
 * to little-endian bytes in RFC 1321 order, so data0 holds the first 8 bytes of
 * the digest and data1 the last 8.
 *
 *   kimix::MD5 md5("hello");                  // or kimix::string_view / span
 *   kimix::string hex = md5.to_string();      // 32 hex chars (upper by default)
 *   auto bin = md5.to_binary();               // MD5::MD5Data {data0, data1}
 *   auto arr = kimix::get_md5_from_string("hello");  // std::array<uint8_t, 16>
 *   auto arr2 = kimix::get_md5_from_array(kimix::span<const uint8_t>{});
 *
 * kimix::hash<MD5> and kimix::hash<MD5::MD5Data> hash the 16 binary bytes.
 */
#pragma once

#include "dll_export.h"
#include "stl/string.h"
#include "stl/memory.h"
#include "stl/hash_fwd.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kimix {

inline constexpr size_t md5_size = 16;

KIMIX_CORE_API std::array<uint8_t, md5_size> get_md5_from_string(string const &str) noexcept;
KIMIX_CORE_API std::array<uint8_t, md5_size> get_md5_from_array(span<uint8_t const> data) noexcept;

class KIMIX_CORE_API MD5 {
public:
    struct MD5Data {
        uint64_t data0;
        uint64_t data1;
    };

private:
    MD5Data data;

public:
    MD5Data const &to_binary() const noexcept { return data; }
    MD5() noexcept : data{0ull, 0ull} {}
    MD5(string const &str) noexcept;
    MD5(string_view str) noexcept;
    MD5(span<uint8_t const> bin) noexcept;
    MD5(MD5 const &) = default;
    MD5(MD5 &&) = default;
    MD5(MD5Data const &data) noexcept;
    string to_string(bool upper = true) const;
    // Rebuild the digest from anything the constructors accept. A temporary is
    // constructed and assigned instead of destroy + placement new, so the object
    // stays a valid MD5 at all times.
    template <typename T>
    MD5 &operator=(T &&t) noexcept {
        *this = MD5(std::forward<T>(t));
        return *this;
    }
    ~MD5() = default;
    MD5 &operator=(MD5 const &) = default;
    MD5 &operator=(MD5 &&) = default;
    bool operator==(MD5 const &m) const noexcept;
    bool operator!=(MD5 const &m) const noexcept;
};

template <>
struct hash<MD5::MD5Data> {
    uint64_t operator()(MD5::MD5Data const &m, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(&m.data0, sizeof(MD5::MD5Data), seed);
    }
};

template <>
struct hash<MD5> {
    uint64_t operator()(MD5 const &m, uint64_t seed = hash64_default_seed) const noexcept {
        return hash<MD5::MD5Data>{}(m.to_binary(), seed);
    }
};

} // namespace kimix
