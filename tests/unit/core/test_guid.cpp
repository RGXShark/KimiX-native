// Test for core/guid.h (kimix::Guid).
// This test covers:
// - Default / generated construction and the bool conversion
// - 32-char hex round-trip through to_string() and try_parse()
// - 22-char base64 round-trip through to_base64() and try_parse()
// - The raw char* to_string() overload (exactly 32 chars, no NUL)
// - Invalid input handling (zeroed guid, empty optional)
// - kimix::compare<Guid> ordering and kimix::hash<Guid>
// - MD5 <-> Guid conversion

#include "ut/ut.hpp"
#include <core/kimix_core.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "guid_default_is_zero"_test = [] {
        kimix::Guid g;
        expect(!g);
        expect(g.to_string() == kimix::string{"00000000000000000000000000000000"});
        expect(kimix::Guid::try_parse(g.to_string()).has_value());
    };

    "guid_generated_is_nonzero"_test = [] {
        kimix::Guid g{true};
        expect(static_cast<bool>(g));
        kimix::Guid h{true};
        expect(g != h) << "two freshly generated guids must differ";
        g.remake();
        expect(static_cast<bool>(g));
    };

    "guid_hex_roundtrip"_test = [] {
        kimix::Guid g{true};
        kimix::string hex = g.to_string();
        expect(eq(hex.size(), size_t{32}));
        auto parsed = kimix::Guid::try_parse(hex);
        expect(parsed.has_value());
        expect(*parsed == g);
        // Lowercase hex parses too.
        kimix::string lower = g.to_string(false);
        auto parsed_lower = kimix::Guid::try_parse(lower);
        expect(parsed_lower.has_value());
        expect(*parsed_lower == g);
        // Direct construction from the hex string.
        kimix::Guid direct{kimix::string_view{hex}};
        expect(direct == g);
    };

    "guid_base64_roundtrip"_test = [] {
        kimix::Guid g{true};
        kimix::string b64 = g.to_base64();
        expect(eq(b64.size(), size_t{22}));
        auto parsed = kimix::Guid::try_parse(b64);
        expect(parsed.has_value());
        expect(*parsed == g);
        kimix::Guid direct{kimix::string_view{b64}};
        expect(direct == g);
        // Raw 24-char overload keeps the "==" padding.
        char raw[24];
        g.to_base64(raw);
        expect(std::memcmp(raw, b64.data(), 22) == 0);
        expect(raw[22] == '=' && raw[23] == '=');
    };

    "guid_raw_hex_buffer"_test = [] {
        kimix::Guid g{kimix::Guid::GuidData{0x0123456789abcdefull, 0xfedcba9876543210ull}};
        char buf[32];
        g.to_string(buf, false);
        expect(std::memcmp(buf, "efcdab89674523011032547698badcfe", 32) == 0)
            << "raw buffer holds the little-endian memory image, 32 chars, no NUL";
        kimix::string s = g.to_string(false);
        expect(std::memcmp(buf, s.data(), 32) == 0);
    };

    "guid_invalid_input_is_zeroed"_test = [] {
        kimix::Guid g{kimix::string_view{"too-short"}};
        expect(!g);
        expect(!kimix::Guid::try_parse("too-short").has_value());
        expect(!kimix::Guid::try_parse(kimix::string_view{"", 0}).has_value());
        // Wrong-size span leaves the guid zeroed.
        uint8_t bytes[8] = {};
        kimix::Guid h{kimix::span<uint8_t const>{bytes, 8}};
        expect(!h);
        // 16-byte span copies the payload.
        uint8_t full[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        kimix::Guid i{kimix::span<uint8_t const>{full, 16}};
        expect(static_cast<bool>(i));
        expect(i.to_array() == kimix::Guid{i}.to_array());
    };

    "guid_compare_and_hash"_test = [] {
        kimix::Guid a{kimix::Guid::GuidData{1ull, 2ull}};
        kimix::Guid b{kimix::Guid::GuidData{1ull, 3ull}};
        kimix::Guid c{kimix::Guid::GuidData{2ull, 0ull}};
        expect(lt(kimix::compare<kimix::Guid>{}(a, b), 0));
        expect(gt(kimix::compare<kimix::Guid>{}(b, a), 0));
        expect(eq(kimix::compare<kimix::Guid>{}(a, kimix::Guid{kimix::Guid::GuidData{1ull, 2ull}}), 0));
        expect(lt(kimix::compare<kimix::Guid>{}(b, c), 0));
        uint64_t expected = kimix::hash64(&a.to_binary().data0, sizeof(kimix::Guid::GuidData));
        expect(eq(kimix::hash<kimix::Guid>{}(a), expected));
    };

    "guid_md5_interop"_test = [] {
        kimix::MD5 md5{kimix::string{"hello"}};
        kimix::Guid g{md5};
        expect(g.to_string() == md5.to_string())
            << "a guid built from an MD5 keeps the same 32 hex chars";
        kimix::MD5 back = static_cast<kimix::MD5>(g);
        expect(back == md5);
    };
}
