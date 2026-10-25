/*
 * bench_binary_file_stream.cpp — throughput benchmark for the two core binary
 * readers: kimix::BinaryFileStream (CRT fread baseline) and
 * kimix::XnvmeBinaryFileStream (xNVMe file backend).
 *
 * This is a PLAIN main() benchmark executable, NOT a Boost.UT suite: it is
 * registered in tests/xmake.lua as a binary target without add_tests("default"),
 * so it builds with the test tree but never runs in `xmake test` (a full 1 GiB
 * pass over four workloads takes seconds, not milliseconds).
 *
 * Workloads (all over one generated temp data file, default 1024 MiB):
 *   sequential-128k   whole-file sequential read in 128 KiB chunks
 *   sequential-1m     whole-file sequential read in 1 MiB chunks
 *   random-64k        2000 x 64 KiB reads at pseudo-random 4 KiB-aligned
 *                     offsets, the identical offset list for both streams
 *                     (generated once with a 64-bit LCG)
 *   batched-64k       512 x 64 KiB reads at the SAME 4 KiB-aligned offsets (the
 *                     first 512 of the random list), recorded and committed as
 *                     ONE batch through kimix::DeferredBinaryFileStream, versus
 *                     the same 512 reads issued one by one through
 *                     kimix::XnvmeBinaryFileStream as the control
 *   read-all          the whole file in a single read_all() call
 *
 * The batched workload measures the BOOKKEEPING a batch removes (one open, one
 * commit, 512 recorded commands) and nothing else: neither platform transport of
 * a commit completes two reads concurrently — see the header comment of
 * deferred_binary_file_stream.h — so no speedup from parallelism is expected or
 * implied.
 *
 * Each workload is measured for both streams in an INTERLEAVED fashion: one
 * untimed warm-up pass per stream, then N timed repetitions per stream with
 * fread and xnvme alternating (fread first, N = 3 by default). Best and median
 * wall time are reported and MiB/s is derived from the bytes the workload moved
 * (B / (ms/1000) / 1 MiB). The position is rewound with set_pos(0) between
 * repetitions — for BinaryFileStream that is an fseek(SEEK_SET), for
 * XnvmeBinaryFileStream it is a pure position reset (xnvme reads are positioned
 * reads and never move the file offset).
 *
 * Why interleaved: the process writes the 1 GiB data file itself and the OS
 * starts writing those dirty pages back immediately, so the first timed passes
 * compete with writeback. Measuring one stream's whole battery and then the
 * other's had that drift land on the first stream only and produced a
 * systematic 10 % bias in favour of the second one; alternating per repetition
 * spreads host-side drift (writeback, frequency/thermal changes, other
 * activity) evenly over both streams.
 *
 * The data file content is deterministic, so the streams can be verified:
 * kPatternBlock-sized blocks (64 KiB) are written with an 8-byte tag derived
 * from the block index (little-endian) followed by 0xA5 fill. The sanity check
 * compares the first 4 KiB of both streams byte-for-byte and spot-checks a few
 * offsets (block-aligned, unaligned, near the end) of both streams against the
 * pattern generator. The process exits non-zero when that check fails.
 *
 * The OS page cache is deliberately NOT dropped (no admin rights here, and
 * dropping it would change what is measured): the file is created and read back
 * by the same process, so every read is served from the page cache. This
 * benchmark measures the read-path overhead on a warm cache — the regular-file
 * path both classes use — not storage throughput.
 *
 * CLI: bench_binary_file_stream [file_mib] [chunk_kib] [reps]
 *   file_mib    data file size in MiB           (default 1024)
 *   chunk_kib   benchmark only this chunk size  (default: 128 and 1024)
 *   reps        timed repetitions per case      (default 3)
 * Env: KIMIX_BENCH_DIR places the temp data file in another directory (it lands
 *   in this program's current working directory by default); it is always
 *   deleted on exit.
 */

#include <core/kimix_core.h>
#include <core/binary_file_stream.h>
#include <core/binary_file_stream_xnvme.h>
#include <core/deferred_binary_file_stream.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// kimix's size literals (64_k, 16_M, ...) live in the inline namespace
// kimix::size_literals and user-defined literals are only found by unqualified
// lookup, so the namespace has to be brought in explicitly here.
using namespace kimix::size_literals;

namespace {

// ---------------------------------------------------------------------------
// File content
// ---------------------------------------------------------------------------

constexpr size_t kPatternBlock = 64_k;                  // one data block: 64 KiB
constexpr uint64_t kPatternTag = 0x4B494D495842454Eull; // "KIMIXBEN"
constexpr unsigned char kPatternFill = 0xA5;

// Byte at 'offset' of the generated file: the little-endian bytes of
// (kPatternTag ^ block_index) in the first 8 bytes of every 64 KiB block, 0xA5
// everywhere else. Cheap to generate (one memset + one 8-byte tag per block)
// and, unlike a plain periodic fill, offset-sensitive: 128 KiB and 1 MiB blocks
// start with different bytes, so a reader that ignores the offset cannot pass
// the spot checks.
inline unsigned char pattern_byte(uint64_t offset) {
    const uint64_t block = offset / kPatternBlock;
    const size_t in_block = static_cast<size_t>(offset % kPatternBlock);
    if (in_block < sizeof(uint64_t)) {
        const uint64_t tag = kPatternTag ^ block;
        return static_cast<unsigned char>((tag >> (8u * in_block)) & 0xFFull);
    }
    return kPatternFill;
}

// The xnvme class takes uint64_t offsets, the fread baseline takes a long
// (32-bit on Windows, so the baseline cannot seek past 2 GiB there).
inline void seek_to(kimix::BinaryFileStream &stream, uint64_t pos) {
    stream.set_pos(static_cast<long>(pos));
}

inline void seek_to(kimix::XnvmeBinaryFileStream &stream, uint64_t pos) {
    stream.set_pos(pos);
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

struct Stats {
    double best_ms = 0.0;
    double median_ms = 0.0;
};

struct Row {
    kimix::string workload;
    kimix::string stream;
    kimix::string chunk;
    uint64_t bytes = 0;
    Stats stats;
};

Stats summarize(kimix::vector<double> &samples) {
    Stats stats;
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    stats.best_ms = samples.front();
    stats.median_ms = samples[samples.size() / 2];
    return stats;
}

double mib_per_second(uint64_t bytes, double ms) {
    if (ms <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(bytes) * 1000.0 / (ms * 1024.0 * 1024.0);
}

// ---------------------------------------------------------------------------
// Workloads — one pass each, returning the elapsed milliseconds. Both stream
// types go through the same template for a workload, so the loop shape, the
// chunk size, the buffer and the byte count are identical for the two sides.
// ---------------------------------------------------------------------------

// Whole-file sequential read in 'chunk'-sized read(void*, size) calls.
template <typename Stream>
double pass_sequential(Stream &stream, size_t chunk, kimix::vector<kimix::byte> &buffer, uint64_t &bytes) {
    seek_to(stream, 0);
    kimix::Clock clock;
    bytes = 0;
    size_t read_bytes = 0;
    while ((read_bytes = stream.read(buffer.data(), chunk)) > 0) {
        bytes += read_bytes;
    }
    return clock.toc();
}

// 'offsets.size()' positioned 64 KiB reads; for the fread baseline every
// iteration pays its fseek, for xnvme set_pos() is a position reset and the
// read is issued as a positioned read.
template <typename Stream>
double pass_random(Stream &stream, const kimix::vector<uint64_t> &offsets, size_t chunk, kimix::vector<kimix::byte> &buffer, uint64_t &bytes) {
    kimix::Clock clock;
    bytes = 0;
    for (uint64_t offset : offsets) {
        seek_to(stream, offset);
        bytes += stream.read(buffer.data(), chunk);
    }
    return clock.toc();
}

// Whole file in one read_all() call (the result vector is allocated and freed
// inside every pass, for both streams alike).
template <typename Stream>
double pass_read_all(Stream &stream, uint64_t &bytes) {
    kimix::Clock clock;
    auto data = stream.read_all();
    bytes = data.size();
    return clock.toc();
}

// One batched pass: record every (offset -> its own slice of 'buffers') read and
// commit them as a single batch. 'buffers' must hold offsets.size() * chunk
// bytes, one slice per recorded read, because every destination buffer stays
// caller-owned and must not move until commit() returns.
double pass_deferred(kimix::DeferredBinaryFileStream &stream, const kimix::string &path,
                     const kimix::vector<uint64_t> &offsets, size_t chunk,
                     kimix::vector<kimix::byte> &buffers, uint64_t &bytes) {
    stream.clear();
    for (size_t i = 0; i < offsets.size(); ++i) {
        stream.add_read(path, offsets[i], chunk, buffers.data() + i * chunk);
    }
    kimix::Clock clock;
    const size_t completed = stream.commit(); // one open, one batch, one close
    bytes = static_cast<uint64_t>(completed) * chunk;
    return clock.toc();
}

// ---------------------------------------------------------------------------
// Interleaved measurement driver
// ---------------------------------------------------------------------------

struct PairResult {
    Stats fread;
    Stats xnvme;
    uint64_t fread_bytes = 0;
    uint64_t xnvme_bytes = 0;
};

// One warm-up pass per stream, then 'reps' timed repetitions per stream with
// fread and xnvme alternating (fread first). See the file header for why.
template <typename FreadPass, typename XnvmePass>
PairResult measure_pair(FreadPass fread_pass, XnvmePass xnvme_pass, int reps) {
    PairResult result;
    fread_pass(result.fread_bytes); // warm-up
    xnvme_pass(result.xnvme_bytes); // warm-up

    kimix::vector<double> fread_samples;
    kimix::vector<double> xnvme_samples;
    fread_samples.reserve(static_cast<size_t>(reps));
    xnvme_samples.reserve(static_cast<size_t>(reps));
    for (int rep = 0; rep < reps; ++rep) {
        fread_samples.push_back(fread_pass(result.fread_bytes));
        xnvme_samples.push_back(xnvme_pass(result.xnvme_bytes));
    }
    result.fread = summarize(fread_samples);
    result.xnvme = summarize(xnvme_samples);
    return result;
}

// ---------------------------------------------------------------------------
// Sanity check
// ---------------------------------------------------------------------------

// Reads 'span' bytes at 'offset' and verifies them against pattern_byte().
template <typename Stream>
bool spot_check(Stream &stream, const char *name, uint64_t offset, size_t span, unsigned char *scratch, bool &ok) {
    seek_to(stream, offset);
    const size_t got = stream.read(scratch, span);
    if (got != span) {
        std::printf("  FAIL %-6s read %zu of %zu bytes at offset %llu\n", name, got, span,
                    static_cast<unsigned long long>(offset));
        ok = false;
        return false;
    }
    for (size_t i = 0; i < span; ++i) {
        if (scratch[i] != pattern_byte(offset + i)) {
            std::printf("  FAIL %-6s byte 0x%02X at offset %llu, expected 0x%02X\n", name, scratch[i],
                        static_cast<unsigned long long>(offset + i), pattern_byte(offset + i));
            ok = false;
            return false;
        }
    }
    return true;
}

bool sanity_check(const kimix::string &path, uint64_t file_size) {
    bool ok = true;

    kimix::BinaryFileStream fread_stream(path);
    kimix::XnvmeBinaryFileStream xnvme_stream(path);
    if (!fread_stream.is_open() || !xnvme_stream.is_open()) {
        std::printf("  FAIL could not open '%s' (fread open: %d, xnvme open: %d)\n", path.c_str(),
                    static_cast<int>(fread_stream.is_open()), static_cast<int>(xnvme_stream.is_open()));
        return false;
    }

    // First 4 KiB: both streams must agree byte-for-byte.
    const size_t head = static_cast<size_t>(std::min<uint64_t>(4_k, file_size));
    kimix::vector<kimix::byte> head_fread(head);
    kimix::vector<kimix::byte> head_xnvme(head);
    seek_to(fread_stream, 0);
    seek_to(xnvme_stream, 0);
    const size_t got_fread = fread_stream.read(head_fread.data(), head);
    const size_t got_xnvme = xnvme_stream.read(head_xnvme.data(), head);
    if (got_fread != head || got_xnvme != head) {
        std::printf("  FAIL first %zu bytes: fread read %zu, xnvme read %zu\n", head, got_fread, got_xnvme);
        ok = false;
    } else if (std::memcmp(head_fread.data(), head_xnvme.data(), head) != 0) {
        std::printf("  FAIL first %zu bytes differ between fread and xnvme\n", head);
        ok = false;
    } else {
        std::printf("  OK   first %zu bytes identical (fread == xnvme)\n", head);
    }

    // Spot checks of both streams against the pattern generator: block start,
    // unaligned offset inside a block, start of the last block, last byte. A
    // reader that ignores the requested offset (the failure mode of the
    // upstream Windows positioned-read bug this repo works around) cannot pass
    // these.
    kimix::vector<uint64_t> offsets = {0, kPatternBlock + 3, 3 * kPatternBlock + 100, file_size > kPatternBlock ? file_size - kPatternBlock : 0,
                                       file_size > 0 ? file_size - 1 : 0};
    kimix::vector<kimix::byte> scratch(16);
    for (uint64_t offset : offsets) {
        const uint64_t available = file_size > offset ? file_size - offset : 0;
        if (available == 0) {
            continue;
        }
        const size_t span = static_cast<size_t>(std::min<uint64_t>(scratch.size(), available));
        spot_check(fread_stream, "fread", offset, span, reinterpret_cast<unsigned char *>(scratch.data()), ok);
        spot_check(xnvme_stream, "xnvme", offset, span, reinterpret_cast<unsigned char *>(scratch.data()), ok);
    }
    if (ok) {
        std::printf("  OK   pattern spot checks passed for both streams\n");
    }
    return ok;
}

// ---------------------------------------------------------------------------
// File creation
// ---------------------------------------------------------------------------

// Writes (block_count) x 64 KiB blocks; the tag of block b is kPatternTag ^ b,
// so the file is deterministic and offset-sensitive. Plain fopen/fwrite.
bool write_data_file(const kimix::string &path, uint64_t size) {
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[bench] cannot create '%s'\n", path.c_str());
        return false;
    }
    kimix::vector<kimix::byte> block(kPatternBlock);
    std::memset(block.data(), kPatternFill, block.size());
    const uint64_t block_count = (size + kPatternBlock - 1) / kPatternBlock;
    for (uint64_t b = 0; b < block_count; ++b) {
        const uint64_t tag = kPatternTag ^ b;
        std::memcpy(block.data(), &tag, sizeof(tag));
        if (std::fwrite(block.data(), 1, block.size(), file) != block.size()) {
            std::fprintf(stderr, "[bench] short write in block %llu\n", static_cast<unsigned long long>(b));
            std::fclose(file);
            return false;
        }
    }
    if (std::fclose(file) != 0) {
        std::fprintf(stderr, "[bench] cannot flush '%s'\n", path.c_str());
        return false;
    }
    return true;
}

struct TempFileGuard {
    kimix::string path;
    ~TempFileGuard() {
        if (!path.empty()) {
            std::remove(path.c_str());
        }
    }
};

// ---------------------------------------------------------------------------
// Host / build identification
// ---------------------------------------------------------------------------

kimix::string compiler_name() {
#if defined(_MSC_VER)
    return kimix::format("MSVC {}.{}", _MSC_VER / 100, _MSC_VER % 100);
#elif defined(__clang__)
    return kimix::format("clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    return kimix::format("gcc {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    return kimix::string("unknown");
#endif
}

kimix::string platform_name() {
#if defined(KIMIX_PLATFORM_WINDOWS)
    return kimix::string("windows");
#elif defined(KIMIX_PLATFORM_UNIX)
    return kimix::string("unix");
#else
    return kimix::string("unknown");
#endif
}

kimix::string working_directory() {
    std::error_code ec;
    auto path = kimix::filesystem::current_path(ec);
    return ec ? kimix::string("?") : kimix::to_string(path);
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct BenchConfig {
    uint64_t file_mib = 1024;
    size_t chunk_kib = 0; // 0 = both default chunk sizes
    int reps = 3;
};

bool parse_u64(const char *text, uint64_t &out) {
    if (!text || !text[0]) {
        return false;
    }
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || (end && *end != '\0')) {
        return false;
    }
    out = value;
    return true;
}

void print_usage(const char *argv0) {
    std::printf("usage: %s [file_mib] [chunk_kib] [reps]\n"
                "  file_mib   data file size in MiB (default 1024)\n"
                "  chunk_kib  only benchmark this chunk size (default: 128 and 1024)\n"
                "  reps       timed repetitions per case (default 3)\n"
                "env: KIMIX_BENCH_DIR  directory for the temp data file (default: CWD)\n",
                argv0);
}

} // namespace

int main(int argc, char *argv[]) {
    BenchConfig config;
    if (argc > 4) {
        print_usage(argv[0]);
        return 2;
    }
    if (argc > 1 && (!parse_u64(argv[1], config.file_mib) || config.file_mib == 0)) {
        print_usage(argv[0]);
        return 2;
    }
    if (argc > 2) {
        uint64_t chunk_kib = 0;
        if (!parse_u64(argv[2], chunk_kib) || chunk_kib == 0) {
            print_usage(argv[0]);
            return 2;
        }
        config.chunk_kib = static_cast<size_t>(chunk_kib);
    }
    if (argc > 3) {
        uint64_t reps = 0;
        if (!parse_u64(argv[3], reps) || reps == 0) {
            print_usage(argv[0]);
            return 2;
        }
        config.reps = static_cast<int>(reps);
    }

    // Round the file size up to whole pattern blocks (64 KiB).
    const uint64_t file_size = ((config.file_mib * 1_M) + kPatternBlock - 1) / kPatternBlock * kPatternBlock;

    const char *env_dir = std::getenv("KIMIX_BENCH_DIR");
    kimix::string data_path = (env_dir && env_dir[0]) ? kimix::string(env_dir) : kimix::string(".");
    data_path += "/bench_binary_file_stream_data.bin";

    kimix::vector<size_t> chunks;
    if (config.chunk_kib != 0) {
        chunks.push_back(config.chunk_kib * 1_k);
    } else {
        chunks.push_back(128_k);
        chunks.push_back(1024_k);
    }
    size_t max_chunk = 0;
    for (size_t chunk : chunks) {
        max_chunk = std::max(max_chunk, chunk);
    }
    max_chunk = std::max(max_chunk, static_cast<size_t>(64_k));

    std::printf("=== bench_binary_file_stream ===\n");
    std::printf("host        : %s\n", kimix::cpu_name().c_str());
    std::printf("platform    : %s\n", platform_name().c_str());
    std::printf("compiler    : %s\n", compiler_name().c_str());
    std::printf("mode        : %s\n",
#if defined(NDEBUG)
                "release"
#else
                "debug"
#endif
    );
    std::printf("cwd         : %s\n", working_directory().c_str());
    std::printf("data file   : %s (%.0f MiB)\n", data_path.c_str(), static_cast<double>(file_size) / (1024.0 * 1024.0));
    std::printf("reps        : %d timed (+1 untimed warm-up pass per stream per case)\n", config.reps);
    std::printf("chunks      :");
    for (size_t chunk : chunks) {
        std::printf(" %zu KiB", chunk / 1_k);
    }
    std::printf("   random: 2000 x 64 KiB @ 4 KiB-aligned LCG offsets\n");
    std::printf("   batched: 512 x 64 KiB @ the first 512 random offsets, one commit\n");
    std::printf("order       : interleaved per repetition (fread, xnvme, fread, xnvme, ...)\n");
#if defined(_WIN32)
    std::printf("note        : Windows positioned reads go through the shim workaround (ReadFile + OVERLAPPED)\n");
#else
    std::printf("note        : Linux positioned reads go through xnvme_file_pread()\n");
#endif
    std::printf("note        : OS page cache is warm (never dropped); this is the regular-file path\n");
    for (size_t chunk : chunks) {
        if (chunk > file_size) {
            std::printf("note        : skipping the %zu KiB chunk, the file is smaller\n", chunk / 1_k);
        }
    }

    // Data file (deleted again on every exit path).
    TempFileGuard guard{data_path};
    {
        kimix::Clock clock;
        if (!write_data_file(data_path, file_size)) {
            return 1;
        }
        std::printf("data file written in %.1f ms\n", clock.toc());
    }

    // Settle: the file was just written, so its pages are dirty in the page
    // cache and the kernel writes them back asynchronously. Two seconds is not
    // a guarantee that they are all clean, but it keeps the bulk of that
    // writeback out of the timed passes (and the interleaving above makes sure
    // the remainder is shared by both streams).
    constexpr int kSettleSeconds = 2;
    std::printf("settling %d s before the timed reads (page-cache writeback)\n", kSettleSeconds);
    std::this_thread::sleep_for(std::chrono::seconds(kSettleSeconds));

    // Correctness first: a broken read path must fail the run, not the timings.
    std::printf("\n--- sanity check ---\n");
    const bool correct = sanity_check(data_path, file_size);
    std::printf("sanity check: %s\n", correct ? "OK" : "FAIL");

    // Random-access offsets: one 64-bit LCG, generated once, shared by both
    // streams, 4 KiB-aligned, inside [0, file_size - 64 KiB).
    const size_t random_chunk = 64_k;
    const size_t random_reads = 2000;
    kimix::vector<uint64_t> random_offsets;
    random_offsets.reserve(random_reads);
    {
        const uint64_t span = file_size > random_chunk ? file_size - random_chunk : 0;
        uint64_t state = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i < random_reads; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const uint64_t raw = span == 0 ? 0 : (state >> 17) % (span / 4096 + 1);
            random_offsets.push_back(raw * 4096ull);
        }
    }

    kimix::vector<Row> rows;
    kimix::vector<kimix::byte> buffer(max_chunk);

    kimix::BinaryFileStream fread_stream(data_path);
    kimix::XnvmeBinaryFileStream xnvme_stream(data_path);
    if (!fread_stream.is_open() || !xnvme_stream.is_open()) {
        std::printf("[bench] cannot open '%s' (fread open: %d, xnvme open: %d)\n", data_path.c_str(),
                    static_cast<int>(fread_stream.is_open()), static_cast<int>(xnvme_stream.is_open()));
        return 1;
    }

    for (size_t chunk : chunks) {
        if (chunk > file_size) {
            continue;
        }
        PairResult result = measure_pair(
            [&](uint64_t &bytes) { return pass_sequential(fread_stream, chunk, buffer, bytes); },
            [&](uint64_t &bytes) { return pass_sequential(xnvme_stream, chunk, buffer, bytes); }, config.reps);
        const kimix::string chunk_label = kimix::format("{} KiB", chunk / 1_k);
        rows.push_back({"sequential", "fread", chunk_label, result.fread_bytes, result.fread});
        rows.push_back({"sequential", "xnvme", chunk_label, result.xnvme_bytes, result.xnvme});
    }

    {
        PairResult result = measure_pair(
            [&](uint64_t &bytes) { return pass_random(fread_stream, random_offsets, random_chunk, buffer, bytes); },
            [&](uint64_t &bytes) { return pass_random(xnvme_stream, random_offsets, random_chunk, buffer, bytes); }, config.reps);
        rows.push_back({"random", "fread", "64 KiB", result.fread_bytes, result.fread});
        rows.push_back({"random", "xnvme", "64 KiB", result.xnvme_bytes, result.xnvme});
    }

    {
        PairResult result = measure_pair(
            [&](uint64_t &bytes) { return pass_read_all(fread_stream, bytes); },
            [&](uint64_t &bytes) { return pass_read_all(xnvme_stream, bytes); }, config.reps);
        rows.push_back({"read-all", "fread", "whole", result.fread_bytes, result.fread});
        rows.push_back({"read-all", "xnvme", "whole", result.xnvme_bytes, result.xnvme});
    }

    // Batched (deferred) reads: 512 x 64 KiB at the same offsets the random
    // workload uses (its first 512), issued as one DeferredBinaryFileStream
    // commit per pass. The control is the identical 512 reads through
    // XnvmeBinaryFileStream one by one, so the two rows differ only in HOW the
    // reads are dispatched.
    const size_t batched_reads = std::min<size_t>(512, random_offsets.size());
    const kimix::string batched_label = kimix::format("64 KiB x{}", batched_reads);
    bool batched_ok = true;
    if (batched_reads != 0) {
        kimix::vector<uint64_t> batched_offsets;
        batched_offsets.reserve(batched_reads);
        for (size_t i = 0; i < batched_reads; ++i) {
            batched_offsets.push_back(random_offsets[i]);
        }
        kimix::vector<kimix::byte> batched_buffers(batched_reads * random_chunk);
        kimix::DeferredBinaryFileStream deferred;

        // Correctness before timing: the batch must complete every recorded read
        // and the bytes must be the right ones (spot-check the last slice against
        // the pattern generator).
        {
            uint64_t batched_bytes = 0;
            pass_deferred(deferred, data_path, batched_offsets, random_chunk, batched_buffers, batched_bytes);
            const bool complete = batched_bytes == static_cast<uint64_t>(batched_reads) * random_chunk;
            const size_t last = batched_reads - 1;
            const unsigned char *slice = reinterpret_cast<const unsigned char *>(batched_buffers.data() + last * random_chunk);
            bool content_ok = true;
            for (size_t i = 0; i < random_chunk; ++i) {
                if (slice[i] != pattern_byte(batched_offsets[last] + i)) {
                    content_ok = false;
                    break;
                }
            }
            if (!complete || !content_ok) {
                std::printf("  FAIL deferred batch: %llu of %zu bytes landed, content %s\n",
                            static_cast<unsigned long long>(batched_bytes), batched_reads * random_chunk,
                            content_ok ? "ok" : "mismatch");
                batched_ok = false;
            } else {
                std::printf("  OK   deferred batch completed %zu x %zu KiB reads\n", batched_reads, random_chunk / 1_k);
            }
        }

        // 'result.fread' carries the individual-read control here, 'result.xnvme'
        // the batched commit — the two slots are just "first" and "second" of the
        // interleaved measurement.
        PairResult result = measure_pair(
            [&](uint64_t &bytes) { return pass_random(xnvme_stream, batched_offsets, random_chunk, buffer, bytes); },
            [&](uint64_t &bytes) { return pass_deferred(deferred, data_path, batched_offsets, random_chunk, batched_buffers, bytes); },
            config.reps);
        rows.push_back({"batched", "xnvme", batched_label, result.fread_bytes, result.fread});
        rows.push_back({"batched", "deferred", batched_label, result.xnvme_bytes, result.xnvme});
    }

    std::printf("\n--- results ---\n");
    std::printf("%-12s %-6s %-9s %13s %12s %12s %14s\n", "workload", "stream", "chunk", "bytes", "best ms", "best MiB/s", "median MiB/s");
    for (const Row &row : rows) {
        std::printf("%-12s %-6s %-9s %13llu %12.1f %12.1f %14.1f\n", row.workload.c_str(), row.stream.c_str(), row.chunk.c_str(),
                    static_cast<unsigned long long>(row.bytes), row.stats.best_ms, mib_per_second(row.bytes, row.stats.best_ms),
                    mib_per_second(row.bytes, row.stats.median_ms));
    }

    std::printf("\ntemp data file removed: %s\n", data_path.c_str());
    std::printf("sanity check: %s\n", correct ? "OK" : "FAIL");
    std::printf("batched check: %s\n", batched_ok ? "OK" : "FAIL");
    return (correct && batched_ok) ? 0 : 1;
}
