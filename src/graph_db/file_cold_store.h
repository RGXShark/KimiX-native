/*
 * file_cold_store.h — the flat-file cold-store backend of kimix-graph-db: ONE
 * append-only file per store, written with one sync() per save batch and read
 * with one positioned sequential pass.
 *
 * This is the second backend behind the internal cold_store.h seam
 * (kimix::graph_db::ColdStore); ColdStorage (utils.h, LMDB) stays the default.
 * Nothing in the public graph_db.h surface mentions this header — a caller
 * selects it through GraphOptions::store_backend, and Database is what opens it.
 *
 * WHY IT EXISTS
 *   LMDB on Windows opens its data file FILE_FLAG_WRITE_THROUGH, so a commit is a
 *   chain of synchronous write-through page flushes (README §6/§8: ~20-24 MB/s
 *   save at 100k/1M vertices). A log-structured file only has to pay ONE
 *   fsync/FlushFileBuffers per save batch, and it needs no page mapping at all:
 *   the transport is plain positioned CRT I/O (fopen + _fseeki64/fseeko) with one
 *   open per pass. The reader is not faster than fread on a page-cached regular
 *   file — it IS fread — the win is the write side and the ordinary, 64-bit-offset
 *   I/O shape.
 *
 * THE FILE — <dir>\graph.xdb  (kColdFileName)
 *   [0..8)    magic "KXDB0001"  (ASCII; a FILE LAYOUT tag, see A5 below)
 *   [8..12)   u32 LE header_size == kHeaderSize (20); the segment starts here, so a
 *             later revision may grow the header additively and an older reader
 *             still skips it
 *   [12..16)  u32 LE header_flags == 0 (reserved; a non-zero value is a different
 *             layout this build does not know, so open() refuses it)
 *   [16..20)  u32 LE format_version == MetaRecord::format_version (== kFormatVersion,
 *             2). Informational only: the AUTHORITY is the "meta/root" record,
 *             because that is what serialize.cpp negotiates on (amendment A5) — a
 *             store that announces a NEWER format must still open, so the load can
 *             refuse it with error_bad_file instead of turning it into error_io.
 *   [header_size..)  the segment: a sequence of FRAMES, one per save_batch():
 *       u64 LE frame_len | payload
 *       payload = repeat of:  u32 LE key_len | key bytes | u64 LE value_len | value bytes
 *
 *   The keys are the existing store spellings — "v/" + 16 guid bytes, "e/" + 16
 *   edge-id bytes, "s/labels", "s/proptypes", "meta/root" (utils.h) — so the
 *   loader's prefix partition works unchanged (amendment A9), and a value with
 *   value_len == 0 IS a tombstone, exactly as in LMDB (amendment A4).
 *
 *   FRAMING IS THE ATOMICITY BOUNDARY (amendment A2)
 *   save_batch() is one frame, so a crash in the middle of an append leaves a
 *   frame whose declared length runs past the end of the file. A load walks the
 *   FRAME headers (8 bytes each — far cheaper than parsing records), stops at the
 *   first frame that does not fit, and reads only the complete prefix: the
 *   previous batch is the last consistent state. That is the flat-file analogue
 *   of LMDB's one-transaction commit, and it is what makes "one sync() per batch"
 *   a durability story rather than just a fast one. The same walk also repairs a
 *   torn tail before the next append (the file is truncated back to the last
 *   complete frame), so a crash cannot poison later batches.
 *
 *   DUPLICATE KEYS: LAST ONE WINS (amendment A3)
 *   LMDB is a map; this file is a log. An incremental save appends a new record
 *   for a key that already exists there, so load_all_records() collapses
 *   duplicates while it scans and keeps the LAST record of each key — every
 *   consumer (record_count(), stored_value(), the loader's skipped_count) is
 *   written against map semantics and would otherwise read stale bytes or count a
 *   guid twice. The output stays a kimix::vector<ColdRecord> in first-appearance
 *   order, so a given file always scans to the same sequence.
 *
 *   META IS A RECORD, NOT A HEADER FIELD (amendment A1)
 *   MetaRecord is variable length (graph_name) and already has a versioned,
 *   total codec (encode_meta_record), so it is written as the ordinary "meta/root"
 *   record the serializer already appends — same bytes, same decode path, no
 *   second copy of the format. read_meta() resolves it ONCE per open (one pass
 *   over the segment, which is also the pass a load needs) and then serves it from
 *   memory, so the four read_meta() calls of a save/load cycle cost nothing.
 *
 *   WRITES
 *     incremental save -> save_batch(): one frame appended at the end of the last
 *                         complete frame: one open (r+b), one positioned write,
 *                         ONE sync, close.
 *     full save        -> rebuild(): header + one frame, one open (wb), two
 *                         positioned writes, one sync, close. This is the only
 *                         path that bounds the file, so it is also the compaction
 *                         remedy for an append-bloated store; it drops records of
 *                         earlier save rounds that LMDB would keep (plan §5.4 item
 *                         1 — parity is asserted on the loaded graph, not on the
 *                         log).
 *   A seam batch carries no "full" flag (serialize.cpp writes both kinds through
 *   the same save_batch()), so Database::persist(full=true) asks for the rebuild
 *   explicitly: set_rebuild_next_batch(true) makes the NEXT save_batch() a full
 *   save. rebuild() itself is public, so a caller can ask for it directly.
 *
 *   FILE SIZE / HANDLES
 *   Offsets are 64-bit (_fseeki64 / fseeko), so a store has no file-size wall: the
 *   segment simply grows until the volume is full, and a failure there is an
 *   ordinary write error. Every writer is synced and CLOSED before its call
 *   returns, because Windows cannot delete a file (or its directory) while a handle
 *   still holds it open.
 *
 *   PATHS (amendment A7)
 *   open()/path()/copy_to() speak DIRECTORIES, like ColdStorage: the store file is
 *   always <dir>\graph.xdb, so `path() == dir` and a copy_to() checkpoint is a
 *   directory that reopens as a store of its own. copy_to() closes its writer
 *   before returning — Windows cannot delete a file an open handle still refers to.
 *
 * CONTRACT (identical to ColdStorage's, and to the seam's)
 *   Every operation reports what the backend reports: false is "not open / absent /
 *   failed", never an exception (the project is built without them). An empty scan
 *   result is NOT an error. accepted-but-ignored: open()'s map_size
 *   (GraphOptions::cold_map_size is an LMDB knob — this backend has no map) and any
 *   ColdStorage-only operation, which FileColdStore deliberately does not grow (see
 *   cold_store.h).
 *
 * Example:
 *   kimix::graph_db::FileColdStore store;
 *   if (store.open(kimix::graph_db::default_cold_dir())) {  // creates <dir>\graph.xdb
 *       kimix::graph_db::ColdStore seam{store};             // what the serializer takes
 *       kimix::graph_db::MetaRecord meta;
 *       if (seam.valid() && seam.read_meta(meta)) { ... }
 *   }
 */
#pragma once

#include "utils.h"

#include <core/memory.h>
#include <core/stl/filesystem.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

// Name of the store file inside the store directory. The DIRECTORY is the unit
// that open() / path() / copy_to() speak (the same unit ColdStorage's LMDB
// environment directory is), so a caller's path handling is backend-agnostic;
// kColdDirName (utils.h) still names the default directory.
inline constexpr kimix::string_view kColdFileName = "graph.xdb";

/*
 * The flat-file backend: one append-only <dir>\graph.xdb, move-only RAII, never
 * throwing. See the file comment for the layout, the framing/durability contract
 * and the limits.
 *
 * Threading: like ColdStorage, it is NOT internally synchronized — one writer (the
 * single-writer contract the LMDB backend also relies on) and readers that do not
 * race a save. The lazily resolved caches (frame geometry, meta record) belong to
 * that same contract.
 */
class KIMIX_GRAPH_DB_API FileColdStore : public kimix::IOperatorNewBase {
public:
    // The 8 ASCII bytes that open every graph.xdb. Distinct from every record
    // magic ("GDB1"/"GDB2"/"GDE2"/"GDM2"/"GDS2") and versioned as a FILE LAYOUT
    // tag — never confused with MetaRecord::format_version (amendment A5).
    static constexpr kimix::string_view kFileMagic{"KXDB0001"};
    // magic(8) | header_size(4) | flags(4) | format_version(4). The segment starts
    // at the header_size the file declares, not at this constant.
    static constexpr size_t kHeaderSize{20};
    static constexpr uint32_t kHeaderFlags{0};

    FileColdStore() noexcept = default;
    // Opens (and creates) the store in `dir`; see open().
    explicit FileColdStore(kimix::filesystem::path const &dir,
                           size_t map_size = 0) noexcept;
    FileColdStore(FileColdStore const &) = delete;
    FileColdStore &operator=(FileColdStore const &) = delete;
    FileColdStore(FileColdStore &&) noexcept = default;
    FileColdStore &operator=(FileColdStore &&) noexcept = default;
    ~FileColdStore() noexcept = default;

    /*
     * Open (and create) the store in `dir`, i.e. <dir>\graph.xdb.
     *
     * Returns false — leaving the store closed, with every operation a no-op that
     * reports false — for: an empty path, a directory that cannot be created, a
     * file whose magic/header_size/flags are not this layout, a file that is
     * truncated inside its header, and an I/O error while reading that header.
     *
     * `map_size` is accepted and IGNORED: it is ColdStorage's LMDB map size
     * (GraphOptions::cold_map_size) and this backend has no map. The parameter stays
     * so the two backends have one open() signature.
     */
    bool open(kimix::filesystem::path const &dir, size_t map_size = 0) noexcept;
    // Close the store and drop the resolved caches. Idempotent; nothing is written.
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept { return _is_open; }
    explicit operator bool() const noexcept { return valid(); }
    // The store DIRECTORY (empty while closed) — the file inside it is
    // kColdFileName, never the path this returns (amendment A7).
    [[nodiscard]] kimix::filesystem::path path() const noexcept { return _path; }

    // The store's "meta/root" record. False when the store is closed, when the
    // file carries no meta record (a v1 store: no metadata at all) or when the
    // record does not decode. Resolved once per open and then cached, so the
    // repeated reads of one save/load cycle never touch the disk.
    [[nodiscard]] bool read_meta(MetaRecord &out) const noexcept;

    // ONE sequential pass over the segment: every record of the store, duplicate
    // keys collapsed LAST-WINS (A3), tombstones included as empty values (A4).
    // Returns false only when the store is closed or its geometry is unreadable;
    // an empty store scans to an empty vector and returns true.
    [[nodiscard]] bool
    load_all_records(kimix::vector<ColdRecord> &out) const noexcept;

    // Append ONE frame holding the whole batch: one open, one positioned write of
    // the frame, ONE sync, one close. The command order is preserved (it is already
    // key-sorted by the serializer, so a frame is deterministic — A6), and the keys
    // must already be store spellings. Returns false — with nothing appended — when
    // the store is closed, when a key is empty or absurdly long, or when the
    // write/sync fails.
    [[nodiscard]] bool
    save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept;

    // FULL save: rebuild the file from scratch — truncate, write the header and
    // the batch as ONE frame, one sync, close. Only valid when `commands` is a
    // COMPLETE snapshot of what the store must hold: an incremental (dirty-only)
    // batch written this way would drop everything else. Returns false without
    // destroying the old file when the store is closed, when a key is invalid, or
    // when the write/sync fails.
    [[nodiscard]] bool
    rebuild(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept;

    // Route the NEXT save_batch() through rebuild() instead of appending (the flag
    // is consumed by that call, whether it succeeds or not). Database::persist()
    // sets it for a full save, because the seam's save_batch() cannot tell a full
    // batch from an incremental one. Only ever set it when the batch about to be
    // written IS the complete store.
    void set_rebuild_next_batch(bool rebuild) noexcept { _rebuild_next = rebuild; }

    // Compacted snapshot of the whole store into the `target` DIRECTORY as
    // <target>\graph.xdb (created on demand), by a sequential 1 MiB-chunk copy of
    // the complete frames. The writer is synced and CLOSED before returning, so the
    // target directory is immediately deletable/reopenable on Windows (A7).
    [[nodiscard]] bool copy_to(kimix::filesystem::path const &target) const noexcept;

private:
    // The single write path behind save_batch() and rebuild().
    [[nodiscard]] bool
    _write(kimix::vector<kimix::LMDBWriteCommand> &&commands, bool rebuild) const noexcept;
    // Resolve the frame geometry: file size, segment start and the end of the last
    // COMPLETE frame (everything after it is a torn tail and is ignored).
    [[nodiscard]] bool _ensure_geometry() const noexcept;
    // The one scan: walks the complete frames, tracks "meta/root", and (when `out`
    // is not null) collapses the records last-wins into it.
    [[nodiscard]] bool _scan(kimix::vector<ColdRecord> *out) const noexcept;
    // Validate (or create) the header of an existing/new file; false means "not
    // this layout".
    [[nodiscard]] bool _check_or_create_file() noexcept;
    // Encode kHeaderSize bytes of header into `out`.
    static void _encode_header(std::byte *out) noexcept;
    // Drop every resolved cache (close(), and after a failed write).
    void _forget() const noexcept;

    // The store directory, the file inside it and its narrow spelling (the private
    // CRT helpers below take kimix::string, not a filesystem::path).
    kimix::filesystem::path _path;
    kimix::filesystem::path _file;
    kimix::string _file_name;
    bool _is_open{false};
    // Lazily resolved, invalidated by close(): the segment start (the header size
    // the file declares), the file size and the end of the last complete frame.
    mutable bool _geometry_known{false};
    mutable uint64_t _segment_start{kHeaderSize};
    mutable uint64_t _file_size{0};
    mutable uint64_t _frames_end{0};
    // Lazily resolved "meta/root" record: `_meta_known` false = not looked at yet.
    mutable bool _meta_known{false};
    mutable bool _meta_present{false};
    mutable MetaRecord _meta{};
    // Consumed by the next _write() (see set_rebuild_next_batch()).
    mutable bool _rebuild_next{false};
};

} // namespace kimix::graph_db