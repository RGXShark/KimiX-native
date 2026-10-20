/*
 * guid.h — kimix::Guid — 128-bit globally unique identifier.
 *
 * A Guid stores its value as two uint64_t words (Guid::GuidData), the same
 * layout as kimix::MD5, and converts freely between the two. Text forms are
 * 32-char hex (to_string) and 22-char unpadded standard base64 (to_base64).
 *
 *   kimix::Guid g{true};            // generate a new (v4) guid
 *   auto s  = g.to_string();        // 32 hex chars, upper by default
 *   auto b64 = g.to_base64();       // 22 chars, no "==" padding
 *   auto p  = kimix::Guid::try_parse("...");   // optional<Guid> (22 or 32 chars)
 *   kimix::Guid q{"..."};
 *   g.remake();                     // new random value
 *   g.reset();                      // zero
 *
 * NOTE (deviation from LuisaCompute vstl): the original logged and called a
 * hard-exit on a string/span of the wrong length. This project has no
 * exceptions and must not gain a logging dependency, so invalid input instead
 * leaves the Guid zeroed (data{0, 0}).
 */
#pragma once

#include "dll_export.h"
#include "md5.h"
#include "compare.h"
#include "stl/string.h"
#include "stl/memory.h"
#include "stl/optional.h"

#include <array>
#include <cstdint>

namespace kimix {

class KIMIX_CORE_API Guid {
public:
    struct GuidData {
        uint64_t data0;
        uint64_t data1;
    };

private:
    GuidData data;

public:
    Guid() noexcept : data{0, 0} {}
    explicit Guid(bool generate) noexcept;
    // Wrong length (not 22 base64 / 32 hex chars) leaves the guid zeroed.
    Guid(string_view strv) noexcept;
    static optional<Guid> try_parse(string_view strv) noexcept;
    // A span whose size is not 16 bytes leaves the guid zeroed.
    Guid(span<uint8_t const> data) noexcept;
    Guid(MD5 const &md5) noexcept {
        auto const &bin = md5.to_binary();
        data.data0 = bin.data0;
        data.data1 = bin.data1;
    }
    operator MD5() const noexcept {
        return MD5(MD5::MD5Data{data.data0, data.data1});
    }
    Guid(std::array<uint8_t, sizeof(GuidData)> const &data) noexcept;
    Guid(GuidData const &d) noexcept;
    void remake() noexcept;
    Guid(Guid const &) = default;
    Guid(Guid &&) = default;
    Guid &operator=(Guid const &) = default;
    Guid &operator=(Guid &&) = default;
    void reset() noexcept {
        data.data0 = 0;
        data.data1 = 0;
    }
    GuidData const &to_binary() const noexcept { return data; }
    std::array<uint8_t, sizeof(GuidData)> to_array() const noexcept;
    string to_string(bool upper = true) const;
    // Writes exactly 32 hex chars — NO NUL terminator. 'result' must be a
    // buffer of at least 32 chars.
    void to_string(char *result, bool upper = true) const noexcept;
    string to_base64() const;
    // Writes exactly 24 chars (22 payload chars + "=="). 'result' must be a
    // buffer of at least 24 chars; nothing is NUL-terminated.
    void to_base64(char *result) const noexcept;

    bool operator==(Guid const &d) const noexcept {
        return data.data0 == d.data.data0 && data.data1 == d.data.data1;
    }
    bool operator!=(Guid const &d) const noexcept {
        return !operator==(d);
    }
    Guid &operator=(GuidData const &d) noexcept {
        data = d;
        return *this;
    }
    explicit operator bool() const noexcept {
        return data.data0 != 0 && data.data1 != 0;
    }
    bool operator!() const noexcept {
        return !(operator bool());
    }
};

template <>
struct hash<Guid> {
    uint64_t operator()(Guid const &guid, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(&guid.to_binary().data0, sizeof(Guid::GuidData), seed);
    }
};

template <>
struct compare<Guid> {
    int32_t operator()(Guid const &a, Guid const &b) const noexcept {
        if (a.to_binary().data0 > b.to_binary().data0) return 1;
        if (a.to_binary().data0 < b.to_binary().data0) return -1;
        if (a.to_binary().data1 > b.to_binary().data1) return 1;
        if (a.to_binary().data1 < b.to_binary().data1) return -1;
        return 0;
    }
};

} // namespace kimix
