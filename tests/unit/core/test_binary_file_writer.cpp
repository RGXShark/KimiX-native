// Test for binary_file_writer.h (kimix::BinaryFileWriter +
// kimix::DeferredBinaryWriter).
// This test covers:
// - BinaryFileWriter: create + write a pattern, close, read back with
//   fopen/fread and compare byte-for-byte; position() bookkeeping; sync()
// - BinaryFileWriter: overwrite through set_pos(), the std::span overload, the
//   truncate = true (empties) and truncate = false (keeps content) semantics
// - BinaryFileWriter: default construction is closed and writes 0 bytes;
//   move construction / move assignment transfer the handle
// - DeferredBinaryWriter: add_write() moves the payload (source vector empty);
//   commit() writes two files and two offsets of the same file (existing content
//   preserved), pending_count()/clear(), repeated add + commit, empty commit
// - Both classes are move-only (compile-time checks) and a 200-command batch
//   commits fully

#include "ut/ut.hpp"
#include <core/kimix_core.h>
#include <core/binary_file_writer.h>

#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Deterministic (non-text) content derived from the byte offset and a seed:
// byte i = ((i * 31 + 7 + seed * 131) ^ (i >> 8) ^ (i >> 16)) & 0xff.
// The seed keeps different regions different; the high-order index bits are
// mixed in so a pattern cannot repeat within the sizes used here.
kimix::byte pattern_byte(size_t index, uint32_t seed) {
    return static_cast<kimix::byte>(
        ((index * 31 + 7 + static_cast<size_t>(seed) * 131) ^ (index >> 8) ^ (index >> 16)) & 0xff);
}

kimix::vector<kimix::byte> make_pattern(size_t size, uint32_t seed) {
    kimix::vector<kimix::byte> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = pattern_byte(i, seed);
    }
    return data;
}

kimix::vector<std::byte> make_payload(size_t size, uint32_t seed) {
    kimix::vector<std::byte> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>(pattern_byte(i, seed));
    }
    return data;
}

bool write_file(const kimix::string &path, const kimix::vector<kimix::byte> &content) {
    FILE *stream = fopen(path.c_str(), "wb");
    if (!stream) {
        return false;
    }
    size_t const written = fwrite(content.data(), 1, content.size(), stream);
    fclose(stream);
    return written == content.size();
}

kimix::vector<kimix::byte> read_file(const kimix::string &path) {
    kimix::vector<kimix::byte> data;

    FILE *stream = fopen(path.c_str(), "rb");
    if (!stream) {
        return data;
    }
    fseek(stream, 0, SEEK_END);
    long const size = ftell(stream);
    fseek(stream, 0, SEEK_SET);
    if (size > 0) {
        data.resize(static_cast<size_t>(size));
        size_t const got = fread(data.data(), 1, data.size(), stream);
        data.resize(got);
    }
    fclose(stream);
    return data;
}

// Compares a file's bytes with an expected region of 'expected' starting at
// 'offset' (a missing file has size 0 and never matches a non-empty region).
bool file_region_matches(const kimix::string &path, const kimix::vector<kimix::byte> &expected,
                         size_t offset) {
    auto const content = read_file(path);
    if (content.size() < offset) {
        return false;
    }
    size_t const count = content.size() - offset;
    if (offset + count > expected.size()) {
        return false;
    }
    return count == 0 || std::memcmp(content.data() + offset, expected.data(), count) == 0;
}

bool file_matches_pattern(const kimix::string &path, const kimix::vector<std::byte> &payload,
                          size_t offset) {
    auto const content = read_file(path);
    if (content.size() < offset + payload.size()) {
        return false;
    }
    return std::memcmp(content.data() + offset, payload.data(), payload.size()) == 0;
}

void remove_file(const kimix::string &path) { std::remove(path.c_str()); }

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    // Both classes hand out handles and file state: copying them would double-close.
    static_assert(!std::is_copy_constructible_v<kimix::BinaryFileWriter>,
                  "BinaryFileWriter must not be copyable");
    static_assert(std::is_move_constructible_v<kimix::BinaryFileWriter>,
                  "BinaryFileWriter must be movable");
    static_assert(!std::is_copy_constructible_v<kimix::DeferredBinaryWriter>,
                  "DeferredBinaryWriter must not be copyable");
    static_assert(std::is_move_constructible_v<kimix::DeferredBinaryWriter>,
                  "DeferredBinaryWriter must be movable");

    "writer_default_construction_is_closed"_test = [] {
        kimix::BinaryFileWriter writer;
        expect(!writer.is_open()) << "default-constructed writer is not open";
        expect(!static_cast<bool>(writer));

        kimix::vector<kimix::byte> buffer = make_pattern(64, 1);
        expect(eq(writer.write(buffer.data(), buffer.size()), 0_u))
            << "writing on a closed writer writes nothing";
        expect(eq(writer.write(std::span<const kimix::byte>(buffer)), 0_u));
        expect(!writer.sync()) << "a closed writer cannot sync";
        expect(eq(writer.position(), 0_u));
        writer.close(); // idempotent
    };

    "writer_empty_path_stays_closed"_test = [] {
        kimix::string const empty_path;
        kimix::BinaryFileWriter writer(empty_path);
        expect(!writer.is_open());
    };

    "writer_create_write_read_back"_test = [] {
        kimix::string const path = "test_writer_basic.bin";
        auto payload = make_pattern(100000, 11);

        {
            kimix::BinaryFileWriter writer(path);
            expect(writer.is_open()) << "a fresh path must open";
            expect(static_cast<bool>(writer));
            expect(eq(writer.position(), 0_u));

            expect(eq(writer.write(payload.data(), payload.size()), payload.size()))
                << "write() reports the full byte count";
            expect(eq(writer.position(), static_cast<uint64_t>(payload.size())))
                << "position() follows the writes";
            expect(writer.sync()) << "sync() flushes the file";
            writer.close();
            expect(!writer.is_open());
            expect(eq(writer.position(), 0_u)) << "close() resets the position";
        }

        auto const content = read_file(path);
        expect(eq(content.size(), payload.size()));
        expect(std::memcmp(content.data(), payload.data(), payload.size()) == 0)
            << "the written bytes must be the pattern";

        remove_file(path);
    };

    "writer_span_write_and_positioned_overwrite"_test = [] {
        kimix::string const path = "test_writer_overwrite.bin";
        auto base = make_pattern(8192, 12);
        auto patch = make_pattern(512, 13);
        expect(write_file(path, base));

        {
            kimix::BinaryFileWriter writer(path, false); // keep the existing content
            expect(writer.is_open());

            writer.set_pos(1024);
            expect(eq(writer.write(std::span<const kimix::byte>(patch)), patch.size()))
                << "span overload writes at the current position";
            expect(eq(writer.position(), 1024 + patch.size()));

            writer.set_pos(4096);
            expect(eq(writer.write(patch.data(), 16), 16_u));
            expect(writer.sync());
        }

        auto const content = read_file(path);
        expect(eq(content.size(), base.size())) << "positioned writes must not resize the file";

        bool preserved = std::memcmp(content.data(), base.data(), 1024) == 0;
        preserved = preserved && std::memcmp(content.data() + 1024, patch.data(), patch.size()) == 0;
        preserved = preserved &&
                    std::memcmp(content.data() + 1024 + patch.size(), base.data() + 1024 + patch.size(),
                                4096 - 1024 - patch.size()) == 0;
        preserved = preserved && std::memcmp(content.data() + 4096, patch.data(), 16) == 0;
        preserved = preserved &&
                    std::memcmp(content.data() + 4096 + 16, base.data() + 4096 + 16,
                                base.size() - 4096 - 16) == 0;
        expect(preserved) << "untouched regions must survive a truncate = false writer";

        remove_file(path);
    };

    "writer_truncate_semantics"_test = [] {
        kimix::string const path = "test_writer_truncate.bin";
        auto big = make_pattern(65536, 14);
        expect(write_file(path, big));

        // truncate = false: the size of the existing file is kept.
        {
            kimix::BinaryFileWriter writer(path, false);
            expect(writer.is_open());
            expect(eq(writer.write(big.data(), 64), 64_u)) << "writes at position 0 of the old file";
        }
        expect(eq(read_file(path).size(), big.size())) << "no truncation without the flag";

        // truncate = true: the second open empties the file.
        // ('small' is a macro, `#define small char`, in the Windows SDK headers.)
        auto const tiny = make_pattern(100, 15);
        {
            kimix::BinaryFileWriter writer(path, true);
            expect(writer.is_open());
            expect(eq(writer.write(tiny.data(), tiny.size()), tiny.size()));
        }
        auto const content = read_file(path);
        expect(eq(content.size(), tiny.size())) << "truncate = true empties the existing file";
        expect(std::memcmp(content.data(), tiny.data(), tiny.size()) == 0);
        expect(!file_region_matches(path, big, 0)) << "the old content is gone";

        remove_file(path);
    };

    "writer_creates_missing_file"_test = [] {
        kimix::string const path = "test_writer_created.bin";
        remove_file(path);

        auto const payload = make_pattern(4096, 16);
        {
            kimix::BinaryFileWriter writer(path, false); // create, but do not truncate
            expect(writer.is_open()) << "a missing file is created";
            expect(eq(writer.write(payload.data(), payload.size()), payload.size()));
            expect(writer.sync());
        }

        auto const content = read_file(path);
        expect(eq(content.size(), payload.size()));
        expect(std::memcmp(content.data(), payload.data(), payload.size()) == 0);

        remove_file(path);
    };

    "writer_move_construction_and_assignment"_test = [] {
        kimix::string const path = "test_writer_move.bin";
        auto const payload = make_pattern(2048, 17);

        kimix::BinaryFileWriter source(path);
        expect(source.is_open());
        kimix::BinaryFileWriter moved(std::move(source));
        expect(moved.is_open()) << "move construction transfers the handle";
        expect(!source.is_open());
        expect(eq(source.write(payload.data(), payload.size()), 0_u))
            << "the moved-from writer is closed";

        kimix::BinaryFileWriter target;
        target = std::move(moved);
        expect(target.is_open()) << "move assignment transfers the handle";
        expect(!moved.is_open());

        expect(eq(target.write(payload.data(), payload.size()), payload.size()));
        target.set_pos(64);
        // Re-writing the bytes that are already there keeps the file identical to
        // 'payload', so a full-file comparison below is meaningful.
        expect(eq(target.write(payload.data() + 64, 32), 32_u));
        expect(target.sync());
        expect(eq(target.position(), 96_u));
        target.close(); // close before removing: Windows cannot delete an open file

        auto const content = read_file(path);
        expect(eq(content.size(), payload.size()));
        expect(std::memcmp(content.data(), payload.data(), payload.size()) == 0);

        remove_file(path);
    };

    "deferred_writer_default_and_clear"_test = [] {
        kimix::DeferredBinaryWriter writer;
        expect(eq(writer.pending_count(), 0_u));
        expect(eq(writer.commit(), 0_u)) << "committing nothing writes nothing";

        kimix::string const path = "test_writer_cleared.bin";
        remove_file(path);

        writer.add_write(path, 0, make_payload(1024, 18));
        writer.add_write(path, 4096, make_payload(128, 19));
        expect(eq(writer.pending_count(), 2_u));

        writer.clear();
        expect(eq(writer.pending_count(), 0_u)) << "clear() drops every command";
        expect(eq(writer.commit(), 0_u)) << "nothing left to write";
        expect(eq(read_file(path).size(), 0_u)) << "clear() must not create or write the file";
    };

    "deferred_writer_add_write_moves_payload"_test = [] {
        kimix::string const path = "test_writer_moved_payload.bin";
        remove_file(path);

        kimix::DeferredBinaryWriter writer;
        auto payload = make_payload(4096, 20);
        size_t const size = payload.size();

        writer.add_write(path, 0, std::move(payload));
        expect(payload.empty()) << "add_write() moves the payload out of the caller's vector";
        expect(eq(writer.pending_count(), 1_u));

        expect(eq(writer.commit(), 1_u));
        expect(eq(read_file(path).size(), size)) << "the moved payload reached the file";

        remove_file(path);
    };

    "deferred_writer_commit_two_files_and_two_offsets"_test = [] {
        kimix::string const path_a = "test_writer_deferred_a.bin";
        kimix::string const path_b = "test_writer_deferred_b.bin";

        // path_a already holds content that the offset writes must preserve.
        auto const base = make_pattern(8192, 21);
        expect(write_file(path_a, base));
        remove_file(path_b);

        auto const first = make_payload(1024, 22);
        auto const second = make_payload(256, 23);
        auto const third = make_payload(4096, 24);
        auto const unaligned = make_payload(256, 25);

        kimix::DeferredBinaryWriter writer;
        writer.add_write(path_a, 512, make_payload(first.size(), 22));
        writer.add_write(path_b, 0, make_payload(third.size(), 24));
        writer.add_write(path_a, 6144, make_payload(second.size(), 23));
        writer.add_write(path_a, 253, make_payload(unaligned.size(), 25));
        expect(eq(writer.pending_count(), 4_u));

        expect(eq(writer.commit(), 4_u)) << "every command must be written";
        expect(eq(writer.pending_count(), 0_u)) << "commit() consumes the recorded writes";

        auto const content_a = read_file(path_a);
        expect(eq(content_a.size(), base.size())) << "offset writes must not truncate";
        expect(std::memcmp(content_a.data(), base.data(), 253) == 0) << "head preserved";
        expect(std::memcmp(content_a.data() + 253, unaligned.data(), unaligned.size()) == 0)
            << "unaligned offset written";
        expect(std::memcmp(content_a.data() + 253 + unaligned.size(),
                           base.data() + 253 + unaligned.size(), 512 - 253 - unaligned.size()) == 0)
            << "the gap between the two writes is preserved";
        expect(std::memcmp(content_a.data() + 512, first.data(), first.size()) == 0)
            << "first offset written";
        expect(std::memcmp(content_a.data() + 6144, second.data(), second.size()) == 0)
            << "second offset in the same file written";
        expect(std::memcmp(content_a.data() + 8192 - 128, base.data() + 8192 - 128, 128) == 0)
            << "tail preserved";

        expect(file_matches_pattern(path_b, third, 0)) << "the second file was created and written";

        remove_file(path_a);
        remove_file(path_b);
    };

    "deferred_writer_add_commit_twice"_test = [] {
        kimix::string const path = "test_writer_twice.bin";
        remove_file(path);

        kimix::DeferredBinaryWriter writer;
        writer.add_write(path, 0, make_payload(1024, 26));
        writer.add_write(path, 2048, make_payload(1024, 27));
        expect(eq(writer.commit(), 2_u));
        expect(eq(writer.pending_count(), 0_u));

        writer.add_write(path, 4096, make_payload(512, 28));
        expect(eq(writer.pending_count(), 1_u));
        expect(eq(writer.commit(), 1_u)) << "the writer is reusable after commit()";
        expect(eq(writer.pending_count(), 0_u));

        expect(file_matches_pattern(path, make_payload(1024, 26), 0));
        expect(file_matches_pattern(path, make_payload(512, 28), 4096));

        remove_file(path);
    };

    "deferred_writer_move_transfers_commands"_test = [] {
        kimix::string const path = "test_writer_move_deferred.bin";
        remove_file(path);

        auto const payload = make_payload(1024, 29);

        kimix::DeferredBinaryWriter source;
        source.add_write(path, 0, make_payload(payload.size(), 29));
        expect(eq(source.pending_count(), 1_u));

        kimix::DeferredBinaryWriter moved(std::move(source));
        expect(eq(moved.pending_count(), 1_u)) << "the moved-to writer owns the commands";
        expect(eq(source.pending_count(), 0_u)) << "the moved-from writer is empty";
        expect(eq(moved.commit(), 1_u));

        expect(file_matches_pattern(path, payload, 0));

        remove_file(path);
    };

    "deferred_writer_large_batch"_test = [] {
        // 200 commands of 1 KiB: 100 contiguous 1 KiB regions in each of two files,
        // so the file content is exactly the concatenation of the payloads.
        constexpr size_t kCount = 200;
        constexpr size_t kSize = 1024;
        constexpr size_t kPerFile = kCount / 2;

        kimix::string const path_a = "test_writer_batch_a.bin";
        kimix::string const path_b = "test_writer_batch_b.bin";
        remove_file(path_a);
        remove_file(path_b);

        kimix::vector<kimix::byte> expected_a(kPerFile * kSize);
        kimix::vector<kimix::byte> expected_b(kPerFile * kSize);

        kimix::DeferredBinaryWriter writer;
        for (size_t i = 0; i < kCount; ++i) {
            auto payload = make_payload(kSize, static_cast<uint32_t>(i));
            bool const to_a = i % 2 == 0;
            size_t const index = i / 2;
            size_t const offset = index * kSize;

            auto &expected = to_a ? expected_a : expected_b;
            std::memcpy(expected.data() + offset, payload.data(), kSize);
            writer.add_write(to_a ? path_a : path_b, offset, std::move(payload));
        }

        expect(eq(writer.pending_count(), kCount));
        expect(eq(writer.commit(), kCount)) << "all 200 writes must complete";

        auto const content_a = read_file(path_a);
        auto const content_b = read_file(path_b);
        expect(eq(content_a.size(), expected_a.size()));
        expect(eq(content_b.size(), expected_b.size()));
        expect(std::memcmp(content_a.data(), expected_a.data(), expected_a.size()) == 0)
            << "file A must hold the first payload of every pair";
        expect(std::memcmp(content_b.data(), expected_b.data(), expected_b.size()) == 0)
            << "file B must hold the second payload of every pair";

        remove_file(path_a);
        remove_file(path_b);
    };
}
