// Test for core/platform.h (kimix::platform path <-> narrow-string
// conversions and the ANSI code page primitives behind them).
//
// kimix::platform::path_from_narrow() / path_to_narrow() are the inline,
// module-local counterparts of kimix::path_from_narrow() /
// kimix::to_string(const filesystem::path&): same CP_ACP semantics, same
// never-throwing contract, but the owning path/string is allocated AND freed
// inside whichever module calls them, so no block crosses a module's
// allocator boundary. These tests pin that behavior down; the heavy
// exception-avoidance cases (unrepresentable names, lone surrogates) are
// covered for the exported helpers in test_stl_filesystem.cpp and must hold
// for these inline twins as well.
#include "ut/ut.hpp"
#include <core/platform.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#endif

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "platform_path_from_narrow_roundtrips_ascii"_test = [] {
        auto p = kimix::platform::path_from_narrow("some/dir/file.txt");
        expect(!p.empty());
        expect(kimix::platform::path_to_narrow(p) == "some/dir/file.txt");
        auto back = kimix::platform::path_from_narrow(
            kimix::platform::path_to_narrow(p));
        expect(back == p);
    };

    "platform_path_from_narrow_empty_is_empty_path"_test = [] {
        auto p = kimix::platform::path_from_narrow("");
        expect(p.empty());
    };

    "platform_path_to_narrow_empty_path_is_empty_string"_test = [] {
        expect(kimix::platform::path_to_narrow(kimix::filesystem::path()).empty());
    };

#if defined(_WIN32) || defined(_WIN64)
    // Query contract: whether the count includes the null terminator varies
    // between Windows builds (documented as included; observed as excluded),
    // so accept either - the owning helper sizes with the query result and
    // strips trailing nulls instead of trusting the count.
    "platform_narrow_to_wide_acp_query_and_fill"_test = [] {
        const kimix::string_view text{"abc"};
        const size_t needed = kimix::platform::narrow_to_wide_acp(text, nullptr, 0u);
        expect(needed == 3u || needed == 4u); // 3 chars, null terminator maybe included
        kimix::wstring wide(needed, L'\0');
        expect(kimix::platform::narrow_to_wide_acp(text, &wide[0], needed) != 0u);
        while (!wide.empty() && wide.back() == L'\0') { wide.pop_back(); }
        expect(wide == L"abc");
    };

    // Bytes the ANSI code page cannot represent must fail the conversion
    // (empty result, i.e. "does not exist") instead of throwing. 0xFF is
    // invalid in GBK (936) and in UTF-8, but a legal single byte in CP1252
    // and CP932, so only assert on the ACPs where it cannot decode.
    "platform_path_from_narrow_rejects_undecodable_bytes"_test = [] {
        if (::GetACP() == 936 || ::GetACP() == CP_UTF8) {
            auto p = kimix::platform::path_from_narrow("bad\xff");
            expect(p.empty());
        }
    };

    // 'D<U+F03A><U+F05C>proj' is a real directory name an agent run created;
    // both code points are unmapped in the GBK code page. path_to_narrow()
    // must degrade lossily to the default replacement character (unless the
    // ACP is UTF-8, which can represent every valid scalar).
    "platform_path_to_narrow_lossy_on_unconvertible_names"_test = [] {
        const kimix::filesystem::path p(std::wstring(L"D\xF03A\xF05Cproj"));
        const kimix::string s = kimix::platform::path_to_narrow(p);
        expect(!s.empty());
        if (::GetACP() != CP_UTF8) {
            expect(s == "D??proj");
        }
    };

    // Strict conversion fails on unrepresentable characters; the lossy pass
    // replaces them (WC_NO_BEST_FIT_CHARS only affects the strict flags).
    // A UTF-8 ACP is the exception: every valid scalar is representable there,
    // so U+F03A passes the strict flags too and strict and lossy agree.
    "platform_wide_to_narrow_acp_strict_vs_lossy"_test = [] {
        const std::wstring_view wide{L"a\xF03A" L"b"};
        if (::GetACP() != CP_UTF8) {
            expect(kimix::platform::wide_to_narrow_acp(wide, nullptr, 0u, true) == 0u);
        } else {
            // Strict query succeeds on a UTF-8 ACP: every valid scalar is
            // representable there, so the count is the 5 encoded UTF-8 bytes
            // (plus the null terminator on builds that include it, hence 5
            // or 6) and the fill emits 61 EF 80 BA 62 — the exact encoding
            // of "a" U+F03A "b".
            const size_t q = kimix::platform::wide_to_narrow_acp(wide, nullptr, 0u, true);
            expect(q == 5u || q == 6u);
            kimix::string out(q + 1u, '\0');
            const size_t filled = kimix::platform::wide_to_narrow_acp(wide, &out[0], out.size(), true);
            expect(filled != 0u);
            while (!out.empty() && out.back() == '\0') { out.pop_back(); }
            expect(out == "a\xEF\x80\xBA" "b") << "strict CP_UTF8 must emit the valid UTF-8 encoding of U+F03A";
            // The bytes decode back through the ACP to the original name.
            auto back = kimix::platform::path_from_narrow(out);
            expect(back.native() == std::wstring(L"a\xF03A" L"b")) << "strict UTF-8 result must round-trip";
        }
        const size_t needed = kimix::platform::wide_to_narrow_acp(wide, nullptr, 0u, false);
        expect(needed > 1u);
        kimix::string out(needed, '\0');
        expect(kimix::platform::wide_to_narrow_acp(wide, &out[0], needed, false) != 0u);
        while (!out.empty() && out.back() == '\0') { out.pop_back(); }
        expect(!out.empty());
        if (::GetACP() != CP_UTF8) {
            expect(out == "a?b");
        } else {
            // On CP_UTF8 the lossy pass has nothing to replace: same bytes as
            // the strict pass above.
            expect(out == "a\xEF\x80\xBA" "b");
        }
    };
#endif

    return 0;
}
