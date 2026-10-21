/*
 * serialize.cpp — the parallel incremental save and the dual-stage load.
 * See serialize.h for the stage split, the guarantees and the caller contract;
 * this file only carries the plumbing.
 *
 * Stage layout (never mixed):
 *   save   collect(dirty) -> [fiber parallel] encode -> [calling thread] sort into
 *          LMDB key order -> [calling thread] 1 LMDB txn -> clear the dirty flags
 *          of exactly the written vertices.
 *   load   [sequential] LMDB scan -> [fiber parallel] decode -> [fiber parallel]
 *          materialize bare vertices -> [single thread] insert into the Graph
 *          -> [single thread] reserve the recorded degrees -> [single thread] link
 *          pass (Graph::connect) -> warnings.
 *
 * The load's ORDER is what makes it dependency-proof: no vertex is in the graph
 * while its edges are resolved, so no record can be rejected because of an edge,
 * and a cycle simply becomes two resolvable edges. See serialize.h.
 *
 * Nothing here blocks a fiber: the two fiber fan-outs only copy bytes and hash
 * them, and the one blocking step (the LMDB write transaction) runs on the
 * calling thread outside every fiber job — the graph_db_plan.md §9 risk note. On
 * a thread with no scheduler bound kimix::fiber::parallel() runs the bodies
 * inline, so both stages are also correct (just serial) from a foreign thread.
 */
#include <core/kimix_core.h> // umbrella first: it fixes the winsock2/windows.h order

#include "serialize.h"

#include <core/clock.h>
#include <core/fiber.h>
#include <core/stl/string.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace kimix::graph_db {

namespace {

// Fan-out budget: the serializer never claims more workers than it can keep
// busy (a save is memory-bandwidth work, not latency work), and never more than
// the ambient pool has. `kimix::fiber::parallel` runs inline when this is 1,
// which is exactly the "no scheduler bound" case.
constexpr uint32_t k_max_fan_out = 8u;
// Records one fiber claim takes at a time. Small enough that the pack of
// workers still shares the work evenly for a handful of dirty vertices.
constexpr uint32_t k_claim_size = 4u;
// Individual stderr warnings printed per load; a badly broken store would
// otherwise flood the console. The counters in the stats stay exact.
constexpr size_t k_max_warnings = 8u;

[[nodiscard]] inline uint32_t fan_out_width() noexcept {
    return std::min<uint32_t>(kimix::fiber::worker_thread_count(), k_max_fan_out);
}

// Run [0, count) over the ambient fiber pool, `k_claim_size` ids per claim and at
// most `fan_out_width()` tasks at a time. Inline (and therefore deterministic in
// order) whenever the pool cannot help. `body` takes a uint32_t job id and must
// only touch the slot that id names.
template<typename Body>
void parallel_over(size_t count, Body &&body) noexcept {
    uint32_t const jobs =
        static_cast<uint32_t>(count > 0xffffffffull ? 0xffffffffull : count);
    kimix::fiber::parallel(jobs, std::forward<Body>(body), k_claim_size,
                           fan_out_width());
}

// Guid as text for a warning. Only used on the warning path, which runs on the
// calling thread after every fan-out has joined.
[[nodiscard]] kimix::string guid_text(kimix::Guid const &guid) noexcept {
    return guid.to_string();
}

void warn_record(char const *reason, kimix::Guid const &guid) noexcept {
    std::fprintf(stderr, "[kimix][graph_db] deserialize: %s, guid %s - record skipped\n",
                 reason, guid_text(guid).c_str());
}

void warn_edge(kimix::Guid const &from, kimix::Guid const &to,
               char const *reason) noexcept {
    std::fprintf(stderr,
                 "[kimix][graph_db] deserialize: %s, edge %s -> %s - dependency skipped\n",
                 reason, guid_text(from).c_str(), guid_text(to).c_str());
}

// One collected vertex record: the stage-1 collection slot, filled by the
// parallel decode, turned into a bare Vertex by the parallel materialize.
// Only the OUTGOING guid list is kept: the reverse list a record carries is a
// cache of the forward edges (Graph::connect rebuilds it), and the stored content
// hash is recomputed by the Vertex constructor, so keeping either here would be
// dead state. Both are still decoded (so a truncated or impossible count is
// caught), just not retained.
struct LoadSlot {
    kimix::Guid key_guid;                 // guid parsed out of the "v/" key
    kimix::vector<std::byte> raw;         // record bytes copied out of LMDB
    kimix::vector<std::byte> content;     // decoded content blob
    kimix::vector<kimix::Guid> depending; // outgoing edges (resolved in the link pass)
    size_t depended_count{0};             // incoming edge count, for the reserve hint
    kimix::Guid guid;                     // guid carried by the record itself
    kimix::unique_ptr<Vertex> vertex;     // materialized vertex (stage 2)
    Vertex *placed{nullptr};              // non-owning: the vertex now inside the graph
    bool valid{false};                    // decoded and self-consistent
};

} // namespace

// ===========================================================================
// Save
// ===========================================================================

GraphSerializeStats GraphSerializer::save(Graph &graph) {
    // FULL save = the incremental pipeline over EVERY vertex: mark first so the
    // collection below is the whole graph, whatever the flags said before.
    graph.mark_dirty();
    return _write(graph);
}

GraphSerializeStats GraphSerializer::save_incremental(Graph &graph) {
    return _write(graph);
}

GraphSerializeStats GraphSerializer::_write(Graph &graph) {
    _last_stats = GraphSerializeStats{};
    if (!_cold.valid()) {
        return _last_stats;
    }
    kimix::Clock clock;
    GraphSerializeStats stats;
    stats.worker_count = fan_out_width();

    // --- stage 1: collect ---------------------------------------------------
    // Graph::dirty_vertices() is the ONLY source of the write set: it takes the
    // graph SHARED lock for the map and each vertex's SHARED lock for the cached
    // hash, and returns guid + hash + Vertex* per dirty vertex. Clean vertices
    // never enter this vector, so an incremental save cannot touch them.
    // The null filter is defensive only (dirty_vertices() already skips nulls);
    // it keeps the parallel slots below dense, so no slot can end up with an
    // empty key that would be written as a real (broken) LMDB entry.
    kimix::vector<DirtyVertex> dirty = graph.dirty_vertices();
    dirty.erase(std::remove_if(dirty.begin(), dirty.end(),
                               [](DirtyVertex const &item) { return item.vertex == nullptr; }),
                dirty.end());
    size_t const count = dirty.size();
    if (count == 0) {
        _last_stats = stats;
        return _last_stats;
    }

    // One command + one written-hash per dirty vertex; the fan-out below fills
    // DISJOINT slots, so it needs no lock and no atomic.
    kimix::vector<kimix::LMDBWriteCommand> commands(count);
    kimix::vector<kimix::MD5> written_hashes(count);

    // --- stage A: encode (fiber parallel) -----------------------------------
    // Each job copies its vertex's content under the vertex SHARED lock, copies
    // both guid lists under the vertex's SHARD SHARED lock (Graph::dependencies /
    // Graph::dependents — the epoch-0 code read the raw Vertex accessors, which
    // the sharded lock cannot protect), and encodes the record straight into its
    // own command slot. The content hash is recomputed from the bytes actually
    // copied, so the record can never claim a hash that does not belong to its
    // content (a content update racing this save would otherwise pair old and new).
    parallel_over(count, [&graph, &dirty, &commands,
                          &written_hashes](uint32_t id) noexcept {
        DirtyVertex const &item = dirty[id];
        Vertex *vertex = item.vertex; // never null: filtered above
        VertexRecord record;
        record.guid = item.guid;
        // The buffer form fills a caller-owned scratch; the record's owning vectors
        // are then built from it. (The span form needs a PRE-SIZED buffer, so it
        // cannot grow an empty vector.)
        AdjacencyBuffer adjacency;
        if (graph.dependencies(item.guid, adjacency)) {
            record.depending_guids.assign(adjacency.begin(), adjacency.end());
        }
        if (graph.dependents(item.guid, adjacency)) {
            record.depended_guids.assign(adjacency.begin(), adjacency.end());
        }
        vertex->content(record.content); // vertex SHARED lock, copies the blob
        record.content_hash = md5_of(record.content);
        guid_key(record.guid, commands[id].key);
        encode_vertex_record(record, commands[id].value);
        written_hashes[id] = record.content_hash;
    });

    size_t bytes = 0;
    for (auto const &command : commands) {
        bytes += command.value.size();
    }
    stats.bytes_written = bytes;

    // --- stage A': bulk-load order (epoch 2) --------------------------------
    // Sort the batch into LMDB key order before the transaction. The batch is
    // already fully materialized here, so the sort cannot race anything, and it
    // changes neither the records nor the atomicity (still ONE transaction, still
    // the same key -> value set). It changes only the ORDER in which mdb_put
    // walks the B-tree, i.e. it turns a random-insert tree build into a
    // bulk-load one: measured on this host, 100k receipts dropped from 11.9k
    // dirty pages to 9.2k and the commit from ~1.04 s to ~0.80 s. The keys are
    // "v/" + 16 raw guid bytes, i.e. already the exact LMDB sort order
    // (memcmp on the key), so the comparator is a plain byte compare and no
    // collation step is involved.
    //
    // Sorting `commands` alone is enough: nothing below indexes it again. The
    // clear-the-dirty-flag pass reads `dirty[]` / `written_hashes[]`, which stay
    // in collection order — and both are keyed by the same vertex either way.
    if (commands.size() > 1) {
        std::sort(commands.begin(), commands.end(),
                  [](kimix::LMDBWriteCommand const &lhs,
                     kimix::LMDBWriteCommand const &rhs) noexcept {
                      size_t const common = std::min(lhs.key.size(), rhs.key.size());
                      int const order = common == 0
                                            ? 0
                                            : std::memcmp(lhs.key.data(), rhs.key.data(), common);
                      if (order != 0) {
                          return order < 0;
                      }
                      return lhs.key.size() < rhs.key.size();
                  });
    }

    // --- stage B: write (blocking, on the CALLING thread) -------------------
    // ONE ColdStorage::save_batch() = one LMDB transaction, i.e. the whole batch
    // is atomic: a crash leaves either every record or none. It stays out of the
    // fan-out on purpose - write_all() is a batch of blocking LMDB syscalls, so
    // running it inside a fiber job would park a pool worker for its duration
    // (graph_db_plan.md §9). A caller that is itself a fiber wraps the whole
    // save in kimix::fiber::blocking_call().
    if (!_cold.save_batch(std::move(commands))) {
        _last_stats = stats;
        return _last_stats;
    }
    stats.saved_count = count;

    // --- clear the flags of exactly the written vertices --------------------
    // A vertex is only marked clean when its cached hash still equals the hash
    // that was just written: a concurrent level-2 update between the copy and
    // this loop leaves it dirty, so the next incremental save rewrites it and
    // the change is not lost. Never touches a vertex that was not written.
    for (size_t i = 0; i < count; ++i) {
        Vertex *vertex = dirty[i].vertex;
        if (vertex->content_hash() == written_hashes[i]) {
            vertex->clear_dirty();
        }
    }

    stats.save_ms = clock.toc();
    _last_stats = stats;
    return _last_stats;
}

// ===========================================================================
// Load
// ===========================================================================

GraphSerializeStats GraphDeserializer::load(ColdStorage &cold, Graph &graph) {
    _last_stats = GraphSerializeStats{};
    if (!cold.valid()) {
        return _last_stats;
    }
    kimix::Clock clock;
    GraphSerializeStats stats;
    stats.worker_count = fan_out_width();

    // --- stage 1: collect (sequential scan, then parallel decode) -----------
    // kimix::LMDB's iterator owns ONE cursor over one read transaction, so the
    // scan itself is sequential (graph_db_plan.md §9: shard the cursor in a later
    // epoch). The spans it yields die on the next ++, hence the copy into `raw`.
    kimix::vector<ColdRecord> scanned;
    if (!cold.load_all_records(scanned)) {
        _last_stats = stats;
        return _last_stats;
    }


    kimix::vector<LoadSlot> slots;
    slots.reserve(scanned.size());
    for (auto &entry : scanned) {
        kimix::optional<kimix::Guid> key_guid =
            guid_from_key(kimix::span<const std::byte>{entry.key.data(), entry.key.size()});
        if (!key_guid) {
            continue; // not a vertex key ("meta/root", ...) - simply not a vertex
        }
        LoadSlot slot;
        slot.key_guid = *key_guid;
        slot.raw = std::move(entry.value);
        slots.push_back(std::move(slot));
    }
    scanned.clear();

    // Parallel decode: every slot parses its own bytes, no shared state at all.
    parallel_over(slots.size(), [&slots](uint32_t id) noexcept {
        LoadSlot &slot = slots[id];
        VertexRecord record;
        if (!decode_vertex_record(
                kimix::span<const std::byte>{slot.raw.data(), slot.raw.size()}, record)) {
            slot.valid = false; // corrupt: bad magic / truncated / impossible counts
            return;
        }
        // The record must agree with its own key, otherwise a copy of one record
        // could impersonate another vertex.
        if (record.guid != slot.key_guid) {
            slot.valid = false;
            return;
        }
        slot.guid = record.guid;
        slot.depended_count = record.depended_guids.size(); // reserve hint only
        slot.depending = std::move(record.depending_guids);
        slot.content = std::move(record.content);
        slot.valid = true;
    });


    size_t bytes_read = 0;
    for (auto const &slot : slots) {
        bytes_read += slot.raw.size();
    }
    stats.bytes_read = bytes_read;
    // The record bytes are no longer needed: the decode above moved everything
    // useful out of them. Releasing them here keeps the peak footprint at "the
    // scan copy plus the materialized vertices" instead of both at once.
    for (auto &slot : slots) {
        kimix::vector<std::byte>{}.swap(slot.raw);
    }

    // --- stage 2: materialize (fiber parallel) ------------------------------
    // A bare vertex: guid + content, and NOTHING else. No dependency list, no
    // pointer, no edge - a vertex's existence cannot depend on any other vertex,
    // which is the structural half of the dual-stage guarantee. The Vertex
    // constructor recomputes the content hash, so a store carrying a stale hash
    // is repaired here.
    parallel_over(slots.size(), [&slots](uint32_t id) noexcept {
        LoadSlot &slot = slots[id];
        if (!slot.valid) {
            return;
        }
        slot.vertex = kimix::unique_ptr<Vertex>{
            new Vertex(slot.guid,
                       kimix::span<const std::byte>{slot.content.data(), slot.content.size()})};
    });


    // --- insert (single thread: Graph::add_vertex is a structural write) ----
    // The target Graph IS the staging container: add_vertex adopts the vertex
    // under the graph unique lock, so a second guid -> Vertex* index would only
    // duplicate state the graph already owns (deviation from the plan's wording
    // "insert into a staging HashMap", same structure, one owner). `placed_slots`
    // below is what the link pass walks, in LMDB key order.
    kimix::vector<LoadSlot *> placed_slots;
    placed_slots.reserve(slots.size());
    size_t printed_records = 0;
    for (auto &slot : slots) {
        if (!slot.valid) {
            ++stats.skipped_count;
            if (printed_records < k_max_warnings) {
                ++printed_records;
                warn_record("corrupt or key/guid mismatch", slot.key_guid);
            }
            continue;
        }
        Vertex *raw = slot.vertex.get();
        if (raw != nullptr && graph.contains(slot.guid)) {
            // Load is not a merge: an already present guid is skipped, never
            // overwritten, and its edges still resolve to the live vertex.
            ++stats.skipped_count;
            if (printed_records < k_max_warnings) {
                ++printed_records;
                warn_record("guid already present in the target graph", slot.guid);
            }
            continue;
        }
        if (raw == nullptr || !graph.add_vertex(std::move(slot.vertex))) {
            // A refused insert (the caller is not the graph's writer thread, or the
            // guid appeared in between): the record is dropped, never forced.
            ++stats.skipped_count;
            if (printed_records < k_max_warnings) {
                ++printed_records;
                warn_record("vertex insert refused by the graph", slot.guid);
            }
            continue;
        }
        slot.placed = raw;
        placed_slots.push_back(&slot);
        ++stats.loaded_count;
    }


    // --- link pass (single thread, AFTER every vertex exists) ---------------
    // Only now are the recorded guid lists turned into edges, through
    // Graph::connect() - which fills both directions on both endpoints (the
    // pointer sets and the guid lists) while holding the endpoint shard unique
    // lock(s). The "depending" list is authoritative; the reverse list is rebuilt
    // by connect, so a store whose two directions disagree is repaired in favour
    // of the outgoing edge. The edge source is the DECODED guid list, never the
    // vertex: a vertex built in stage 2 carries no edges at all (that is the
    // dual-stage guarantee), so nothing can be resolved before this pass.
    //
    // EPOCH 2: this pass was 87 % of a 100k load (link 272 ms of 313 ms) while the
    // scan it supposedly waits on was 12-31 ms. The profile of the 6 graph calls
    // per edge was: connect's own shard locks + duplicate check ~40 %, and the
    // OTHER HALF was the geometric growth of the four per-vertex edge containers
    // (each vertex starts at the 16-slot minimum that kimix::vector /
    // kimix::unordered_set round a first insert up to, then reallocates per edge).
    // The record already carries both degree counts, so the pass now walks its
    // slots twice - a cheap first sweep that reserves the exact final degree on
    // every vertex, then the edge sweep - and the reallocations are gone. The
    // reservation changes NO edge: reserve() only raises capacity.
    //
    // Both sweeps keep the original order (LMDB key order, each slot's recorded
    // list order), so the resulting guid sets and adjacency - and therefore the
    // byte image of a subsequent save - stay exactly what they were.
    for (auto *slot : placed_slots) {
        if (slot->placed == nullptr) {
            continue;
        }
        graph.reserve_links(slot->guid, slot->depending.size(), slot->depended_count);
    }

    // EPOCH 3: ONE Graph::connect_bulk call for the whole edge set. Semantics are
    // exactly the per-edge Graph::connect loop it replaces (same duplicate probe,
    // same two-sided bookkeeping, same accept/refuse decisions and order), but the
    // batch resolves every distinct endpoint ONCE (one shard SHARED lock per
    // unique endpoint instead of one Graph::find per edge) and takes each touched
    // shard's UNIQUE lock once for the whole batch (instead of once per edge) with
    // the single-writer check hoisted out of the loop. See graph.h.
    //
    // The edge list is built first, in the SAME order the loop above visited it
    // (slot order = LMDB key order, each slot's recorded list order), so the
    // resulting guid lists, pointer sets and reverse edges are identical to the
    // connect-per-edge version - and a caller that needs the per-edge verdicts
    // (this one does, for the skip counters and the warnings) gets them back in
    // request order.
    kimix::vector<EdgeRequest> requests;
    requests.reserve(stats.loaded_count * 4);
    for (auto *slot : placed_slots) {
        if (slot->placed == nullptr) {
            continue;
        }
        for (auto const &target_guid : slot->depending) {
            requests.push_back(EdgeRequest{slot->guid, target_guid});
        }
    }

    kimix::vector<EdgeOutcome> outcomes(requests.size(), EdgeOutcome::created);
    graph.connect_bulk(kimix::span<EdgeRequest const>{requests.data(), requests.size()},
                       kimix::span<EdgeOutcome>{outcomes.data(), outcomes.size()});

    // Count and report the edge verdicts. Every outcome that is not `created` is
    // exactly one skipped_edge_count, i.e. the same arithmetic the per-edge loop
    // did with Graph::connect's bool. `missing_source` cannot actually happen for
    // a load (every source slot is in the graph by construction) but the outcome
    // exists for the general contract, so it is counted rather than assumed away.
    size_t printed_edges = 0;
    for (size_t i = 0; i < outcomes.size(); ++i) {
        switch (outcomes[i]) {
        case EdgeOutcome::created:
            continue;
        case EdgeOutcome::self_edge:
            ++stats.skipped_edge_count;
            if (printed_edges < k_max_warnings) {
                ++printed_edges;
                warn_edge(requests[i].from, requests[i].to, "self dependency");
            }
            continue;
        case EdgeOutcome::missing_target:
            ++stats.skipped_edge_count;
            if (printed_edges < k_max_warnings) {
                ++printed_edges;
                warn_edge(requests[i].from, requests[i].to, "missing dependency");
            }
            continue;
        case EdgeOutcome::duplicate_edge:
        case EdgeOutcome::missing_source:
        case EdgeOutcome::refused:
            ++stats.skipped_edge_count;
            if (printed_edges < k_max_warnings) {
                ++printed_edges;
                warn_edge(requests[i].from, requests[i].to, "edge refused by the graph");
            }
            continue;
        }
    }

    // --- settle the dirty flags --------------------------------------------
    // Loaded vertices are CLEAN: their content and edges are exactly what the
    // store holds, so an immediate save_incremental() writes nothing. (The flags
    // are already true from construction, hence the explicit clear.)
    for (auto *slot : placed_slots) {
        if (slot->placed != nullptr) {
            slot->placed->clear_dirty();
        }
    }

    // One-line summary, always: a broken store should never fail silently.
    if (stats.skipped_count > printed_records || stats.skipped_edge_count > printed_edges) {
        std::fprintf(stderr,
                     "[kimix][graph_db] deserialize: %zu record(s) and %zu edge(s) skipped "
                     "(%zu + %zu warning(s) shown)\n",
                     stats.skipped_count, stats.skipped_edge_count, printed_records,
                     printed_edges);
    }

    stats.load_ms = clock.toc();
    _last_stats = stats;
    return _last_stats;
}

} // namespace kimix::graph_db