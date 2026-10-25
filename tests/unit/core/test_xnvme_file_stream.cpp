// Test for binary_file_stream_xnvme.h (kimix::XnvmeBinaryFileStream).
// This test covers:
// - Construct from path (opens read-only through xNVMe)
// - Missing file leaves the stream closed
// - length() matches the written size, initial position() == 0
// - sequential read() of a deterministic 1 MiB binary pattern
// - read(span) after set_pos() to an unaligned offset
// - read_all() matches the full pattern
// - short read at the tail: a 64-byte request at N-10 returns exactly 10 bytes
//   with the right content (pread(2) semantics; regression for the Linux shim
//   path that used to turn a short read into a failure — see
//   kimix_xnvme_stream_pread)
// - chunked while-read loop over the whole file: reads in 128-byte chunks until
//   read() returns 0 and reconstructs the file byte-for-byte
// - move construction / move assignment transfer ownership
// - close() resets the stream (is_open false, length 0, reads return 0)

#include "ut/ut.hpp"
#include <core/kimix_core.h>
#include <core/binary_file_stream_xnvme.h>

#include <cstdio>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// 1 MiB of deterministic (non-text) data, derived from the byte offset:
// byte i = ((i * 31 + 7) ^ (i >> 8) ^ (i >> 16)) & 0xff.
// The high-order index bits are mixed in on purpose: a purely periodic pattern
// (plain (i * 31 + 7) & 0xff repeats every 256 bytes) cannot tell a correct
// reader apart from one that ignores the offset and always returns the first
// bytes of the file — exactly the failure mode the Windows xnvme read path has
// — so the period here is 16 MiB, far beyond the file.
constexpr size_t kPatternSize = 1024 * 1024;

void fill_pattern(kimix::vector<kimix::byte> &buffer) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<kimix::byte>(((i * 31 + 7) ^ (i >> 8) ^ (i >> 16)) & 0xff);
    }
}

kimix::vector<kimix::byte> make_pattern() {
    kimix::vector<kimix::byte> buffer(kPatternSize);
    fill_pattern(buffer);
    return buffer;
}

kimix::string write_temp_file(const kimix::vector<kimix::byte> &content) {
    kimix::string path = "test_xnvme_stream_temp.bin";
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(content.data(), 1, content.size(), f);
        fclose(f);
    }
    return path;
}

void remove_temp_file(const kimix::string &path) { std::remove(path.c_str()); }

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "xnvme_default_construction_closed"_test = [] {
        kimix::XnvmeBinaryFileStream stream;
        expect(!stream.is_open()) << "default-constructed stream is not open";
        expect(!static_cast<bool>(stream));
        expect(eq(stream.length(), 0_u));
        expect(eq(stream.position(), 0_u));
        expect(eq(stream.read(nullptr, 0), 0_u)) << "closed stream reads 0";
    };

    "xnvme_construct_from_path"_test = [] {
        auto pattern = make_pattern();
        auto path = write_temp_file(pattern);
        kimix::XnvmeBinaryFileStream stream(path);
        expect(stream.is_open()) << "should be open after construction from path";
        expect(static_cast<bool>(stream));
        stream.close();
        remove_temp_file(path);
    };

    "xnvme_missing_file_stays_closed"_test = [] {
        kimix::XnvmeBinaryFileStream stream(kimix::string("__nonexistent_file_xnvme__.bin"));
        expect(!stream.is_open()) << "non-existent file should not open";
        expect(eq(stream.length(), 0_u));
    };

    "xnvme_length_and_initial_position"_test = [] {
        auto pattern = make_pattern();
        auto path = write_temp_file(pattern);
        kimix::XnvmeBinaryFileStream stream(path);

        expect(eq(stream.length(), kPatternSize)) << "length() must match the written size";
        expect(eq(stream.position(), 0_u)) << "initial position should be 0";

        stream.close();
        remove_temp_file(path);
    };

    "xnvme_sequential_read_matches_pattern"_test = [] {
        auto pattern = make_pattern();
        auto path = write_temp_file(pattern);
        kimix::XnvmeBinaryFileStream stream(path);

        // 64 KiB chunks: exercises the byte-granular pread and the position book-keeping.
        constexpr size_t kChunk = 64 * 1024;
        kimix::vector<kimix::byte> chunk(kChunk);
        size_t total = 0;
        bool matches = true;
        while (total < pattern.size()) {
            size_t const want = (pattern.size() - total) < kChunk ? (pattern.size() - total) : kChunk;
            size_t const got = stream.read(chunk.data(), want);
            if (got != want) {
                matches = false;
                break;
            }
            if (std::memcmp(chunk.data(), pattern.data() + total, got) != 0) {
                matches = false;
                break;
            }
            total += got;
        }
        expect(matches) << "sequential reads must reproduce the pattern";
        expect(eq(total, kPatternSize));
        expect(eq(stream.position(), static_cast<uint64_t>(kPatternSize)));

        // Reading past EOF yields 0 bytes, like fread() does.
        expect(eq(stream.read(chunk.data(), kChunk), 0_u));

        stream.close();
        remove_temp_file(path);
    };

    "xnvme_read_span_after_unaligned_seek"_test = [] {
        auto pattern = make_pattern();
        auto path = write_temp_file(pattern);
        kimix::XnvmeBinaryFileStream stream(path);

        constexpr uint64_t kOffset = 12345;
        stream.set_pos(kOffset);
        expect(eq(stream.position(), kOffset)) << "position should follow set_pos()";

        kimix::vector<kimix::byte> buffer(4096);
        size_t const got = stream.read(std::span<kimix::byte>(buffer));
        expect(eq(got, buffer.size()));
        expect(std::memcmp(buffer.data(), pattern.data() + kOffset, got) == 0)
            << "unaligned seek must read the right bytes";
        expect(eq(stream.position(), kOffset + got));

        // Seeking back to 0 and reading through the span overload again.
        stream.set_pos(0);
        size_t const head = stream.read(std::span<kimix::byte>(buffer.data(), 16));
        expect(eq(head, 16_u));
        expect(std::memcmp(buffer.data(), pattern.data(), head) == 0);

        stream.close();
        remove_temp_file(path);
    };

    "xnvme_short_read_at_tail_returns_short_count"_test = [] {
        // 1000 bytes, read 64 of them at offset N-10: only 10 are left, so the
        // read reports 10 — the pread(2) contract — and the bytes must be the
        // file's last ten. This is the regression test for the Linux shim path
        // that used to report such a read as a failure (and therefore as 0 bytes
        // through read()): xnvme's synchronous backend decides a transfer failed
        // by comparing the count and then reading errno, which turns a short read
        // into "error" whenever errno happens to be non-zero.
        constexpr size_t kFileSize = 1000;
        constexpr size_t kTail = 10;
        kimix::vector<kimix::byte> content(kFileSize);
        fill_pattern(content);
        auto path = write_temp_file(content);

        kimix::XnvmeBinaryFileStream stream(path);
        expect(stream.is_open()) << "the 1000-byte file must open";
        expect(eq(stream.length(), kFileSize));

        stream.set_pos(kFileSize - kTail);
        kimix::vector<kimix::byte> buffer(64);
        std::memset(buffer.data(), 0, buffer.size());
        size_t const got = stream.read(buffer.data(), buffer.size());
        expect(eq(got, static_cast<size_t>(kTail)))
            << "a short read at the tail must return the short count, not 0";
        expect(std::memcmp(buffer.data(), content.data() + (kFileSize - kTail), kTail) == 0)
            << "the short read must return the file's last bytes";
        expect(eq(stream.position(), static_cast<uint64_t>(kFileSize)))
            << "the position advances by the bytes actually read";

        // At EOF every further read reports 0, for any request size.
        expect(eq(stream.read(buffer.data(), buffer.size()), 0_u));

        stream.close();
        remove_temp_file(path);
    };

    "xnvme_chunked_read_loop_reconstructs_file"_test = [] {
        // The while-read loop contract: read() returns the transferred bytes and
        // 0 at end of file, so one buffer is enough to walk the whole file (the
        // last chunk is short: 1000 = 7 x 128 + 104).
        constexpr size_t kFileSize = 1000;
        constexpr size_t kChunk = 128;
        kimix::vector<kimix::byte> content(kFileSize);
        fill_pattern(content);
        auto path = write_temp_file(content);

        kimix::XnvmeBinaryFileStream stream(path);
        expect(stream.is_open());

        kimix::vector<kimix::byte> chunk(kChunk);
        size_t total = 0;
        size_t reads = 0;
        bool matches = true;
        for (;;) {
            size_t const got = stream.read(chunk.data(), kChunk);
            if (got == 0) {
                break;
            }
            if (got > kChunk || total + got > kFileSize) {
                matches = false;
                break;
            }
            if (std::memcmp(chunk.data(), content.data() + total, got) != 0) {
                matches = false;
                break;
            }
            total += got;
            ++reads;
        }
        expect(matches) << "the chunked loop must reconstruct the file";
        expect(eq(total, kFileSize)) << "the loop must read every byte";
        expect(eq(reads, 8_u)) << "7 full chunks + 1 short chunk";
        expect(eq(stream.position(), static_cast<uint64_t>(kFileSize)));
        expect(eq(stream.read(chunk.data(), kChunk), 0_u)) << "EOF stays EOF";

        stream.close();
        remove_temp_file(path);
    };

    "xnvme_read_all"_test = [] {
        auto pattern = make_pattern();
        auto path = write_temp_file(pattern);
        kimix::XnvmeBinaryFileStream stream(path);

        auto data = stream.read_all();
        expect(eq(data.size(), kPatternSize));
        expect(std::memcmp(data.data(), pattern.data(), data.size()) == 0)
            << "read_all() must reproduce the pattern";
        expect(eq(stream.position(), static_cast<uint64_t>(kPatternSize)))
            << "read_all() leaves the position at the end of the data";

        stream.close();
        remove_temp_file(path);
    };

    "xnvme_move_constructor_transfers_ownership"_test = [] {
        auto pattern = make_pattern();
        auto path = write_temp_file(pattern);
        kimix::XnvmeBinaryFileStream source(path);
        size_t const length = source.length();

        kimix::XnvmeBinaryFileStream moved(std::move(source));
        expect(moved.is_open());
        expect(eq(moved.length(), length));
        expect(!source.is_open()) << "moved-from stream should not be open";
        expect(eq(source.length(), 0_u));
        expect(eq(source.read(nullptr, 0), 0_u));

        auto data = moved.read_all();
        expect(eq(data.size(), kPatternSize));
        expect(std::memcmp(data.data(), pattern.data(), data.size()) == 0)
            << "moved-to stream must still read correctly";

        moved.close();
        remove_temp_file(path);
    };

    "xnvme_move_assignment_transfers_ownership"_test = [] {
        auto pattern = make_pattern();
        auto path = write_temp_file(pattern);
        kimix::XnvmeBinaryFileStream source(path);

        kimix::XnvmeBinaryFileStream target;
        target = std::move(source);
        expect(target.is_open());
        expect(eq(target.length(), kPatternSize));
        expect(!source.is_open()) << "moved-from stream should not be open";

        kimix::vector<kimix::byte> buffer(256);
        size_t const got = target.read(buffer.data(), buffer.size());
        expect(eq(got, buffer.size()));
        expect(std::memcmp(buffer.data(), pattern.data(), got) == 0);

        target.close();
        remove_temp_file(path);
    };

    "xnvme_close_resets"_test = [] {
        auto pattern = make_pattern();
        auto path = write_temp_file(pattern);
        kimix::XnvmeBinaryFileStream stream(path);
        expect(stream.is_open());

        stream.close();
        expect(!stream.is_open()) << "should not be open after close";
        expect(eq(stream.length(), 0_u)) << "length should be 0 after close";
        expect(eq(stream.position(), 0_u)) << "position should be 0 after close";
        expect(eq(stream.read(nullptr, 0), 0_u));

        kimix::vector<kimix::byte> buffer(64);
        expect(eq(stream.read(std::span<kimix::byte>(buffer)), 0_u))
            << "reads on a closed stream must return 0";

        remove_temp_file(path);
    };
}
