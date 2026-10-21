/*
 * utils.cpp — key scheme, vertex-record codec and cold storage.
 * See utils.h for the layout documented byte by byte; the encoders below are
 * the single place that layout is materialised, so the two must be kept in
 * sync.
 *
 * Portability notes:
 *  - Every multi-byte field is written/read byte by byte in little-endian
 *    order, so the record is identical on any host and no struct is ever
 *    memcpy'd to or from the disk.
 *  - Decoding is total: each read is bounds-checked against the buffer size
 *    before it happens, so a truncated or hostile record is a `false`, never a
 *    crash and never an abort.
 *  - No exceptions: kimix::LMDB reports fatal LMDB errors itself (documented in
 *    utils.h); the bool results here cover the recoverable conditions only.
 */
#include "utils.h"

#include <core/md5.h>
#include <core/stl/string.h>

#include <cstring>
#include <utility>

namespace kimix::graph_db {

namespace {

// --- little-endian scalar helpers (no alignment assumptions) ----------------

inline void write_u32le(std::byte *dst, uint32_t value) noexcept {
    dst[0] = static_cast<std::byte>(value & 0xffu);
    dst[1] = static_cast<std::byte>((value >> 8) & 0xffu);
    dst[2] = static_cast<std::byte>((value >> 16) & 0xffu);
    dst[3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

inline uint32_t read_u32le(std::byte const *src) noexcept {
    return static_cast<uint32_t>(std::to_integer<uint32_t>(src[0])) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[1])) << 8) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[2])) << 16) |
           (static_cast<uint32_t>(std::to_integer<uint32_t>(src[3])) << 24);
}

// Appends a 128-bit value as two little-endian 64-bit words. kimix::Guid and
// kimix::MD5 share that layout (both are two uint64_t words), so one pair of
// helpers covers the guid fields and the content hash in the record.
inline void write_two_u64le(std::byte *dst, uint64_t data0, uint64_t data1) noexcept {
    write_u32le(dst + 0, static_cast<uint32_t>(data0 & 0xffffffffull));
    write_u32le(dst + 4, static_cast<uint32_t>((data0 >> 32) & 0xffffffffull));
    write_u32le(dst + 8, static_cast<uint32_t>(data1 & 0xffffffffull));
    write_u32le(dst + 12, static_cast<uint32_t>((data1 >> 32) & 0xffffffffull));
}

inline kimix::Guid::GuidData read_two_u64le(std::byte const *src) noexcept {
    kimix::Guid::GuidData words{};
    words.data0 = static_cast<uint64_t>(read_u32le(src + 0)) |
                  (static_cast<uint64_t>(read_u32le(src + 4)) << 32);
    words.data1 = static_cast<uint64_t>(read_u32le(src + 8)) |
                  (static_cast<uint64_t>(read_u32le(src + 12)) << 32);
    return words;
}

inline void write_guid_bytes(std::byte *dst, kimix::Guid const &guid) noexcept {
    auto const &words = guid.to_binary();
    write_two_u64le(dst, words.data0, words.data1);
}

inline kimix::Guid read_guid_bytes(std::byte const *src) noexcept {
    return kimix::Guid{read_two_u64le(src)};
}

// Append `count` guids at `dst`; returns the advanced pointer.
inline std::byte *write_guid_table(std::byte *dst,
                                   kimix::vector<kimix::Guid> const &guids) noexcept {
    for (auto const &guid : guids) {
        write_guid_bytes(dst, guid);
        dst += 16;
    }
    return dst;
}

} // namespace

// ---------------------------------------------------------------------------
// Key helpers
// ---------------------------------------------------------------------------

std::array<std::byte, kVertexKeySize>
guid_key(kimix::Guid const &guid) noexcept {
    std::array<std::byte, kVertexKeySize> key{};
    key[0] = static_cast<std::byte>(kVertexKeyPrefix[0]);
    key[1] = static_cast<std::byte>(kVertexKeyPrefix[1]);
    write_guid_bytes(key.data() + 2, guid);
    return key;
}

void guid_key(kimix::Guid const &guid, kimix::vector<std::byte> &out) {
    auto key = guid_key(guid);
    out.assign(key.begin(), key.end());
}

kimix::optional<kimix::Guid>
guid_from_key(kimix::span<const std::byte> key) noexcept {
    if (key.size() != kVertexKeySize) {
        return std::nullopt;
    }
    if (std::to_integer<char>(key[0]) != kVertexKeyPrefix[0] ||
        std::to_integer<char>(key[1]) != kVertexKeyPrefix[1]) {
        return std::nullopt;
    }
    return read_guid_bytes(key.data() + 2);
}

kimix::MD5 md5_of(kimix::span<const std::byte> bytes) noexcept {
    return kimix::MD5{kimix::span<uint8_t const>{
        reinterpret_cast<uint8_t const *>(bytes.data()), bytes.size()}};
}

// ---------------------------------------------------------------------------
// Vertex record codec
// ---------------------------------------------------------------------------

size_t encoded_record_size(VertexRecord const &record) noexcept {
    return kVertexRecordHeaderSize + (record.depending_guids.size() +
                                      record.depended_guids.size()) * 16 +
           record.content.size();
}

void encode_vertex_record(VertexRecord const &record,
                          kimix::vector<std::byte> &out) {
    out.clear();
    out.resize(encoded_record_size(record));

    std::byte *cursor = out.data();
    std::memcpy(cursor, kRecordMagic.data(), kRecordMagicSize);
    write_guid_bytes(cursor + kRecordGuidOffset, record.guid);
    // The cached MD5 travels as its 16 RFC-1321 bytes, i.e. data0 then data1.
    auto const &hash_words = record.content_hash.to_binary();
    write_two_u64le(cursor + kRecordHashOffset, hash_words.data0, hash_words.data1);
    write_u32le(cursor + kRecordDependingCountOffset,
                static_cast<uint32_t>(record.depending_guids.size()));
    write_u32le(cursor + kRecordDependedCountOffset,
                static_cast<uint32_t>(record.depended_guids.size()));

    cursor += kVertexRecordHeaderSize;
    cursor = write_guid_table(cursor, record.depending_guids);
    cursor = write_guid_table(cursor, record.depended_guids);
    if (!record.content.empty()) {
        std::memcpy(cursor, record.content.data(), record.content.size());
    }
}

bool decode_vertex_record(kimix::span<const std::byte> bytes,
                          VertexRecord &out) noexcept {
    if (bytes.size() < kVertexRecordHeaderSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kRecordMagic.data(), kRecordMagicSize) != 0) {
        return false;
    }

    uint32_t const n_depending = read_u32le(bytes.data() + kRecordDependingCountOffset);
    uint32_t const n_depended = read_u32le(bytes.data() + kRecordDependedCountOffset);
    // Guard against a bogus count before it drives any allocation: each entry
    // costs 16 bytes and must fit in what is left after the header.
    size_t const available = bytes.size() - kVertexRecordHeaderSize;
    uint64_t const deps_bytes =
        (static_cast<uint64_t>(n_depending) + static_cast<uint64_t>(n_depended)) * 16ull;
    if (deps_bytes > available) {
        return false;
    }

    out.guid = read_guid_bytes(bytes.data() + kRecordGuidOffset);
    // The 16 hash bytes are the two uint64 words of the MD5 (identical layout
    // to GuidData, see <core/md5.h>), so the same reader recovers both fields.
    auto const hash_words = read_two_u64le(bytes.data() + kRecordHashOffset);
    out.content_hash =
        kimix::MD5{kimix::MD5::MD5Data{hash_words.data0, hash_words.data1}};

    std::byte const *cursor = bytes.data() + kVertexRecordHeaderSize;
    out.depending_guids.clear();
    out.depending_guids.reserve(n_depending);
    for (uint32_t i = 0; i < n_depending; ++i, cursor += 16) {
        out.depending_guids.push_back(read_guid_bytes(cursor));
    }
    out.depended_guids.clear();
    out.depended_guids.reserve(n_depended);
    for (uint32_t i = 0; i < n_depended; ++i, cursor += 16) {
        out.depended_guids.push_back(read_guid_bytes(cursor));
    }

    size_t const content_size =
        bytes.size() - kVertexRecordHeaderSize - static_cast<size_t>(deps_bytes);
    out.content.assign(cursor, cursor + content_size);
    return true;
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

bool ensure_dir(kimix::filesystem::path const &dir) noexcept {
    std::error_code ec;
    if (kimix::filesystem::exists(dir, ec)) {
        return !ec && kimix::filesystem::is_directory(dir, ec);
    }
    if (ec) {
        return false;
    }
    kimix::filesystem::create_directories(dir, ec);
    return !ec;
}

kimix::filesystem::path default_cold_dir() {
    kimix::filesystem::path dir;
    if (!kimix::path_from_utf8(kColdDirName, dir)) {
        return {};
    }
    return dir;
}

// ---------------------------------------------------------------------------
// ColdStorage
// ---------------------------------------------------------------------------

ColdStorage::ColdStorage(kimix::filesystem::path const &dir,
                         size_t map_size) noexcept
    : _db(dir, 126ull, map_size),
      _path(dir) {
}

bool ColdStorage::open(kimix::filesystem::path const &dir,
                       size_t map_size) noexcept {
    if (dir.empty()) {
        return false;
    }
    close();
    _db = kimix::LMDB(dir, 126ull, map_size);
    _path = dir;
    return valid();
}

void ColdStorage::close() noexcept {
    _db = kimix::LMDB{};
    _path.clear();
}

bool ColdStorage::valid() const noexcept {
    return static_cast<bool>(_db);
}

kimix::filesystem::path ColdStorage::path() const noexcept {
    return _path;
}

bool ColdStorage::read_record(kimix::Guid const &guid,
                              VertexRecord &out) const noexcept {
    if (!valid()) {
        return false;
    }
    auto key = guid_key(guid);
    auto value = _db.read(kimix::span<const std::byte>{key.data(), key.size()});
    if (value.empty()) {
        return false;
    }
    return decode_vertex_record(value, out);
}

bool ColdStorage::load_all_records(kimix::vector<ColdRecord> &out) const noexcept {
    if (!valid()) {
        return false;
    }
    out.clear();
    for (auto it = _db.begin(); it != _db.end(); ++it) {
        auto const &value = *it;
        ColdRecord record;
        record.key.assign(value.key.begin(), value.key.end());
        record.value.assign(value.value.begin(), value.value.end());
        out.push_back(std::move(record));
    }
    return true;
}

bool ColdStorage::load_vertex_records(kimix::vector<VertexRecord> &out,
                                      size_t *skipped) const noexcept {
    if (!valid()) {
        return false;
    }
    out.clear();
    size_t skipped_count = 0;
    for (auto it = _db.begin(); it != _db.end(); ++it) {
        auto const &value = *it;
        // Only "v/" keys are vertex records; "meta/root" and any future key
        // family are simply not vertices.
        if (!guid_from_key(value.key)) {
            continue;
        }
        VertexRecord record;
        if (!decode_vertex_record(value.value, record)) {
            ++skipped_count;
            continue;
        }
        out.push_back(std::move(record));
    }
    if (skipped) {
        *skipped = skipped_count;
    }
    return true;
}

bool ColdStorage::save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept {
    if (!valid()) {
        return false;
    }
    _db.write_all(std::move(commands));
    return true;
}

bool ColdStorage::save_records(kimix::vector<VertexRecord> &&records) const noexcept {
    if (!valid()) {
        return false;
    }
    kimix::vector<kimix::LMDBWriteCommand> commands;
    commands.reserve(records.size());
    for (auto const &record : records) {
        kimix::LMDBWriteCommand command;
        guid_key(record.guid, command.key);
        encode_vertex_record(record, command.value);
        commands.push_back(std::move(command));
    }
    _db.write_all(std::move(commands));
    return true;
}

bool ColdStorage::remove_keys(kimix::vector<kimix::vector<std::byte>> &&keys) const noexcept {
    if (!valid()) {
        return false;
    }
    _db.remove_all(std::move(keys));
    return true;
}

bool ColdStorage::remove_record(kimix::Guid const &guid) const noexcept {
    if (!valid()) {
        return false;
    }
    auto key = guid_key(guid);
    _db.remove(kimix::span<const std::byte>{key.data(), key.size()});
    return true;
}

bool ColdStorage::copy_to(kimix::filesystem::path const &target) const noexcept {
    if (!valid() || target.empty()) {
        return false;
    }
    _db.copy_to(target);
    return true;
}

} // namespace kimix::graph_db