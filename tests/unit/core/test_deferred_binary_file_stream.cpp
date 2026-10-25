// Test for deferred_binary_file_stream.h (kimix::DeferredBinaryFileStream).
// This test covers:
// - Recording positioned reads across two files (unaligned offsets and sizes)
//   and verifying every destination buffer byte-for-byte after commit()
// - commit() returns the number of fully completed reads and clears the list,
//   so a second add + commit round works on the same object
// - A missing file fails (contributes 0) without stopping the other commands
// - clear() drops recorded commands without touching the buffers, and commit()
//   on an empty list returns 0
// - Move construction transfers the pending command list
// - A large batch (600 reads of 4 KiB across two files) exercises the chunked
//   dispatch of the shim's batched read channel (queue depth 256)

#include "ut/ut.hpp"
#include <core/kimix_core.h>
#include <core/deferred_binary_file_stream.h>

#include <cstdio>
#include <cstring>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Deterministic (non-text) content derived from the byte offset and a per-file
// seed: byte i = ((i * 31 + 7 + seed * 131) ^ (i >> 8) ^ (i >> 16)) & 0xff.
// The high-order index bits and the seed are mixed in on purpose: a purely
// periodic pattern (plain (i * 31 + 7) & 0xff repeats every 256 bytes) cannot
// tell a correct reader apart from one that ignores the offset, and a seed-free
// pattern cannot tell two files apart.
void fill_pattern(kimix::vector<kimix::byte> &buffer, uint32_t seed) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<kimix::byte>(
            ((i * 31 + 7 + static_cast<size_t>(seed) * 131) ^ (i >> 8) ^ (i >> 16)) & 0xff);
    }
}

struct TempFile {
    kimix::string path;
    kimix::vector<kimix::byte> content;
};

TempFile make_temp_file(const char *name, size_t size, uint32_t seed) {
    TempFile file;
    file.path = name;
    file.content.resize(size);
    fill_pattern(file.content, seed);

    FILE *stream = fopen(file.path.c_str(), "wb");
    if (stream) {
        fwrite(file.content.data(), 1, file.content.size(), stream);
        fclose(stream);
    }
    return file;
}

void remove_temp_file(const kimix::string &path) { std::remove(path.c_str()); }

// Byte-exact comparison of a destination buffer with the range of 'file' it was
// supposed to receive.
bool buffer_matches(const kimix::vector<kimix::byte> &buffer, const TempFile &file, uint64_t offset) {
    if (offset + buffer.size() > file.content.size()) {
        return false;
    }
    return std::memcmp(buffer.data(), file.content.data() + offset, buffer.size()) == 0;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "deferred_default_construction_is_empty"_test = [] {
        kimix::DeferredBinaryFileStream stream;
        expect(eq(stream.pending_count(), 0_u)) << "a fresh stream records nothing";
        expect(eq(stream.commit(), 0_u)) << "committing nothing dispatches nothing";
        stream.clear();
        expect(eq(stream.pending_count(), 0_u));
    };

    "deferred_commit_reads_across_two_files"_test = [] {
        auto file_a = make_temp_file("test_deferred_a.bin", 1024 * 1024, 1);
        auto file_b = make_temp_file("test_deferred_b.bin", 256 * 1024, 2);

        kimix::vector<kimix::byte> b0(4096);
        kimix::vector<kimix::byte> b1(1);
        kimix::vector<kimix::byte> b2(1024);
        kimix::vector<kimix::byte> b3(3);
        kimix::vector<kimix::byte> b4(100000);
        kimix::vector<kimix::byte> b5(49);

        constexpr uint64_t kA0 = 12345;   // unaligned
        constexpr uint64_t kA1 = 0;
        constexpr uint64_t kB2 = 70001;   // unaligned
        constexpr uint64_t kA3 = 65535;   // unaligned, odd size
        constexpr uint64_t kB4 = 0;
        constexpr uint64_t kA5 = 1048576 - 49; // last bytes of the file

        kimix::DeferredBinaryFileStream stream;
        stream.add_read(file_a.path, kA0, b0.size(), b0.data());
        stream.add_read(file_a.path, kA1, b1.size(), b1.data());
        stream.add_read(file_b.path, kB2, b2.size(), b2.data());
        stream.add_read(file_a.path, kA3, b3.size(), b3.data());
        stream.add_read(file_b.path, kB4, b4.size(), b4.data());
        stream.add_read(file_a.path, kA5, b5.size(), b5.data());

        expect(eq(stream.pending_count(), 6_u)) << "six reads recorded";

        size_t const completed = stream.commit();
        expect(eq(completed, 6_u)) << "every read must complete fully";
        expect(eq(stream.pending_count(), 0_u)) << "commit() consumes the recorded reads";

        expect(buffer_matches(b0, file_a, kA0)) << "unaligned read from file A";
        expect(buffer_matches(b1, file_a, kA1)) << "first byte of file A";
        expect(buffer_matches(b2, file_b, kB2)) << "unaligned read from file B";
        expect(buffer_matches(b3, file_a, kA3)) << "3-byte read from file A";
        expect(buffer_matches(b4, file_b, kB4)) << "large read from file B";
        expect(buffer_matches(b5, file_a, kA5)) << "tail read from file A";

        remove_temp_file(file_a.path);
        remove_temp_file(file_b.path);
    };

    "deferred_commit_twice_in_a_row"_test = [] {
        auto file = make_temp_file("test_deferred_twice.bin", 64 * 1024, 3);

        kimix::DeferredBinaryFileStream stream;
        kimix::vector<kimix::byte> first(512);
        kimix::vector<kimix::byte> second(512);

        stream.add_read(file.path, 4096, first.size(), first.data());
        stream.add_read(file.path, 8, second.size(), second.data());
        expect(eq(stream.commit(), 2_u));
        expect(eq(stream.pending_count(), 0_u));

        kimix::vector<kimix::byte> third(1024);
        kimix::vector<kimix::byte> fourth(7);
        stream.add_read(file.path, 60000, third.size(), third.data());
        stream.add_read(file.path, 3, fourth.size(), fourth.data());
        expect(eq(stream.pending_count(), 2_u));
        expect(eq(stream.commit(), 2_u)) << "the stream is reusable after commit()";
        expect(eq(stream.pending_count(), 0_u));

        expect(buffer_matches(first, file, 4096));
        expect(buffer_matches(second, file, 8));
        expect(buffer_matches(third, file, 60000));
        expect(buffer_matches(fourth, file, 3));

        remove_temp_file(file.path);
    };

    "deferred_missing_file_does_not_stop_others"_test = [] {
        auto file = make_temp_file("test_deferred_present.bin", 32 * 1024, 4);

        kimix::string const missing = "test_deferred_missing_file__.bin";
        remove_temp_file(missing);

        kimix::vector<kimix::byte> lost(256);
        kimix::vector<kimix::byte> good(2048);

        kimix::DeferredBinaryFileStream stream;
        stream.add_read(missing, 0, lost.size(), lost.data());
        stream.add_read(file.path, 777, good.size(), good.data());
        expect(eq(stream.pending_count(), 2_u));

        expect(eq(stream.commit(), 1_u)) << "only the readable command completes";
        expect(eq(stream.pending_count(), 0_u));
        expect(buffer_matches(good, file, 777)) << "the good buffer must still be filled";

        remove_temp_file(file.path);
    };

    "deferred_missing_file_only"_test = [] {
        kimix::string const missing = "test_deferred_missing_only__.bin";
        remove_temp_file(missing);

        kimix::vector<kimix::byte> lost(64);
        kimix::DeferredBinaryFileStream stream;
        stream.add_read(missing, 0, lost.size(), lost.data());

        expect(eq(stream.commit(), 0_u)) << "an unopenable path completes nothing";
        expect(eq(stream.pending_count(), 0_u));
    };

    "deferred_clear_drops_commands"_test = [] {
        auto file = make_temp_file("test_deferred_clear.bin", 8192, 5);

        kimix::vector<kimix::byte> buffer(128);
        std::memset(buffer.data(), 0x5a, buffer.size());

        kimix::DeferredBinaryFileStream stream;
        stream.add_read(file.path, 0, buffer.size(), buffer.data());
        stream.add_read(file.path, 4096, buffer.size(), buffer.data());
        stream.add_read(file.path, 1024, buffer.size(), buffer.data());
        expect(eq(stream.pending_count(), 3_u));

        stream.clear();
        expect(eq(stream.pending_count(), 0_u)) << "clear() drops every command";
        expect(eq(stream.commit(), 0_u)) << "nothing left to dispatch";

        bool untouched = true;
        for (size_t i = 0; i < buffer.size(); ++i) {
            if (static_cast<unsigned char>(buffer[i]) != 0x5a) {
                untouched = false;
                break;
            }
        }
        expect(untouched) << "clear() must not touch the destination buffers";

        remove_temp_file(file.path);
    };

    "deferred_move_transfers_pending_commands"_test = [] {
        auto file = make_temp_file("test_deferred_move.bin", 16 * 1024, 6);

        kimix::vector<kimix::byte> first(300);
        kimix::vector<kimix::byte> second(700);

        kimix::DeferredBinaryFileStream source;
        source.add_read(file.path, 10, first.size(), first.data());
        source.add_read(file.path, 15000, second.size(), second.data());
        expect(eq(source.pending_count(), 2_u));

        kimix::DeferredBinaryFileStream moved(std::move(source));
        expect(eq(moved.pending_count(), 2_u)) << "the moved-to stream owns the commands";
        expect(eq(source.pending_count(), 0_u)) << "the moved-from stream is empty";
        expect(eq(moved.commit(), 2_u));

        expect(buffer_matches(first, file, 10));
        expect(buffer_matches(second, file, 15000));

        remove_temp_file(file.path);
    };

    "deferred_large_batch_across_files"_test = [] {
        // 600 reads of 4 KiB: 400 aligned reads of one file (more than the shim's
        // 256-deep queue, so the chunked dispatch runs) and 200 unaligned reads of
        // a second file.
        constexpr size_t kChunk = 4096;
        constexpr size_t kAlignedCount = 400;
        constexpr size_t kUnalignedCount = 200;
        constexpr size_t kTotal = kAlignedCount + kUnalignedCount;
        constexpr uint64_t kUnalignedStart = 3;

        auto aligned = make_temp_file("test_deferred_large_a.bin", kAlignedCount * kChunk, 7);
        auto unaligned = make_temp_file("test_deferred_large_b.bin",
                                        kUnalignedCount * kChunk + kUnalignedStart + kChunk, 8);

        kimix::vector<kimix::vector<kimix::byte>> buffers(kTotal);
        for (auto &buffer : buffers) {
            buffer.resize(kChunk);
        }

        kimix::DeferredBinaryFileStream stream;
        for (size_t i = 0; i < kAlignedCount; ++i) {
            stream.add_read(aligned.path, i * kChunk, kChunk, buffers[i].data());
        }
        for (size_t i = 0; i < kUnalignedCount; ++i) {
            size_t const index = kAlignedCount + i;
            stream.add_read(unaligned.path, kUnalignedStart + i * kChunk, kChunk,
                            buffers[index].data());
        }

        expect(eq(stream.pending_count(), kTotal));
        expect(eq(stream.commit(), kTotal)) << "every one of the 600 reads must complete";

        bool matches = true;
        for (size_t i = 0; i < kAlignedCount && matches; ++i) {
            matches = buffer_matches(buffers[i], aligned, i * kChunk);
        }
        for (size_t i = 0; i < kUnalignedCount && matches; ++i) {
            matches = buffer_matches(buffers[kAlignedCount + i], unaligned,
                                     kUnalignedStart + i * kChunk);
        }
        expect(matches) << "all 600 destination buffers must match their source range";

        remove_temp_file(aligned.path);
        remove_temp_file(unaligned.path);
    };
}
