/*
 * serialize.h — parallel incremental save and dual-stage load for kimix-graph-db.
 *
 * Two classes over the ColdStorage of utils.h:
 *
 *   GraphSerializer    Graph  -> LMDB  (full save / incremental save)
 *   GraphDeserializer  LMDB   -> Graph (dual-stage, cycle- and corruption-proof)
 *
 * The byte layout is entirely utils.h's: a value is the "GDB1" VertexRecord
 * written by encode_vertex_record() and read by decode_vertex_record(), and the
 * key is guid_key()'s "v/" + 16 binary guid bytes. Nothing here re-implements
 * little-endian encoding — both classes go through those two helpers.
 *
 * ===========================================================================
 * SAVE — two stages, one transaction
 * ===========================================================================
 *   stage 1 (collect)  Graph::dirty_vertices() — graph SHARED lock for the map,
 *                      each vertex's SHARED lock for the cached hash. The result
 *                      is the exact set this call will write; nothing else is
 *                      ever encoded.
 *   stage A (encode)   kimix::fiber::parallel over that set: each job copies the
 *                      content (Vertex::content_copy(), vertex SHARED lock),
 *                      copies the vertex's two guid lists, and encodes the record
 *                      straight into its own LMDBWriteCommand slot. Jobs write
 *                      DISJOINT slots, so the fan-out needs no lock.
 *   stage B (write)    ONE ColdStorage::save_batch() call — a single LMDB
 *                      transaction, therefore atomic. It runs on the CALLING
 *                      thread, never inside a fiber job: write_all() is a batch
 *                      of blocking LMDB syscalls and would park a pool worker
 *                      for its whole duration (graph_db_plan.md §9 risk note).
 *                      A caller that already runs inside a fiber should wrap the
 *                      whole save in kimix::fiber::blocking_call().
 *                      EPOCH 2 sorts the batch into LMDB key order first (see
 *                      stage A' below); that changes only the order in which
 *                      mdb_put walks the B-tree, never the record set, the byte
 *                      image of a record, or the single-transaction atomicity.
 *   stage A' (order)   std::sort of the finished batch by its "v/"+guid key,
 *                      i.e. by the exact bytes LMDB compares. A random-insert
 *                      B-tree build becomes a bulk-load one: measured here, 100k
 *                      records went from 11.9k to 9.2k dirty pages and the commit
 *                      from ~1.04 s to ~0.80 s (sort cost 14-16 ms, one thread,
 *                      against a ~200 ms commit win).
 *   then              the dirty flag of every written vertex is cleared — but
 *                      only when its cached content hash still equals the hash
 *                      that was written, so a concurrent level-2 content update
 *                      keeps the vertex dirty for the next save instead of
 *                      losing the change.
 *
 * save()             FULL save: marks every vertex dirty first, so every vertex
 *                    is re-encoded and rewritten (checkpoint semantics). It does
 *                    NOT delete the records of vertices that are gone from the
 *                    graph — a full save is one atomic write transaction, so
 *                    removal stays an explicit ColdStorage::remove_record() /
 *                    copy_to() step.
 * save_incremental() the primary path: ONLY the dirty vertices are encoded and
 *                    written; a clean vertex is neither re-encoded nor rewritten,
 *                    so its stored bytes stay bit-identical.
 * copy_to(path)      passthrough to ColdStorage::copy_to() (compacted
 *                    snapshot/checkpoint of the whole environment).
 *
 * The record's content hash is recomputed from the bytes actually copied, so a
 * content update racing with a save can never produce a record whose stored MD5
 * disagrees with its own content.
 *
* CALLER CONTRACT (inherited from Graph, see graph.h):
 *  - No structural write (create/remove vertex, connect/disconnect) may run
 * concurrently with a save: the graph is structurally quiescent while it is
 * saved. Concurrent level-2 content updates ARE allowed (see below); the
 * dependency lists are read through the shard-locked Graph accessors, so each
 * vertex's pair of lists is consistent even if a reader races them.
 *  - Graph::connect/disconnect change BOTH endpoints' serialized dependency
 *    lists but do not set their dirty flags (the dirty flag is content
 *    bookkeeping). After a structural change the caller must mark the affected
 *    vertices dirty (Vertex::mark_dirty()) or call Graph::mark_dirty(); a full
 *    save() does that for the whole graph. This is the same "bookkeeping is the
 *    caller's" rule dirty_vertices() documents.
 *  - Removing a vertex does not delete its record and no save prunes one: a save
 *    is a write transaction, so dropping a record stays an explicit
 *    ColdStorage::remove_record() step (copy_to() then compacts the store).
 *
 * ===========================================================================
 * LOAD — the dual-stage guarantee
 * ===========================================================================
 *   stage 1 (collect)   ColdStorage::load_all_records() — ONE sequential scan,
 *                       because kimix::LMDB's iterator holds a single cursor
 *                       (graph_db_plan.md §9: scan first, shard later). Every
 *                       "v/" key becomes a collection slot; then a fiber-parallel
 *                       pass runs decode_vertex_record() on the copied bytes,
 *                       filling (guid, outgoing dependency guids, content,
 *                       validity). The stored content hash and reverse list are
 *                       decoded (so a bad buffer is caught) but not retained: the
 *                       Vertex constructor re-hashes and Graph::connect rebuilds
 *                       the reverse edges. Corrupt values are flagged, never fatal.
 *   stage 2 (materialize) A fiber-parallel pass builds a bare
 *                       kimix::unique_ptr<Vertex> per valid slot — guid plus
 *                       content — and NOTHING else: no dependency list, no
 *                       pointer, no graph mutation. The single-threaded insert
 *                       then adopts them into the target Graph through
 *                       Graph::add_vertex (a structural write, so one thread);
 *                       the target graph itself is the staging container, since
 *                       it already owns the guid -> Vertex mapping a second index
 *                       would only duplicate.
 *   link pass           Only AFTER every vertex is in the graph are the recorded
 *                       guid lists resolved into edges, through the graph's edge
 *                       API — which fills both the guid lists and the Vertex* sets
 *                       on both endpoints. This ordering is what makes a dependency
 *                       cycle (A<->B) harmless and a missing guid harmless: an
 *                       edge is never the reason a vertex fails to load. A
 *                       dependency that cannot be resolved (missing target, self
 *                       edge, refused edge) is counted in
 *                       GraphSerializeStats::skipped_edge_count and reported on
 *                       stderr; the load continues.
 *                       EPOCH 2 runs it in two sweeps so the four per-vertex edge
 *                       containers are sized once from the degree counts the
 *                       record already carries (Graph::reserve_links): this pass
 *                       was 87 % of a 100k load, and roughly half of that was the
 *                       geometric growth of those containers. The reservation
 *                       only ever raises capacity, so it cannot add, drop or
 *                       reorder a single edge.
 *                       EPOCH 3 builds the whole edge list and hands it to ONE
 *                       Graph::connect_bulk call: every distinct endpoint is
 *                       resolved once (one shard shared lock per unique endpoint,
 *                       instead of one Graph::find per edge), every touched shard's
 *                       unique lock is taken once for the batch in ascending order,
 *                       and the single-writer check runs once. Semantics are
 *                       identical to the per-edge Graph::connect loop it replaced —
 *                       same shared _connect_resolved_locked body, same duplicate
 *                       probe, same order, same skipped-edge arithmetic (each of the
 *                       n>created outcomes from EdgeOutcome is one skip) — and the
 *                       edge phase of a 100k load went 183 ms -> ~110 ms.
 *
 *   Corrupt record (bad magic, truncated, impossible dependency count, or a
 *   record whose embedded guid disagrees with its key) => the vertex is skipped,
 *   warned about, counted in skipped_count, and the rest of the store loads.
 *
 *   Loaded vertices end up CLEAN, not dirty: their content and edges are exactly
 *   what the store holds, so an immediate save_incremental() writes nothing. A
 *   loaded vertex also has its content hash recomputed on materialization (the
 *   Vertex constructor always hashes), so a store with a stale stored hash is
 *   repaired rather than propagated.
 *
 *   Edges resolve against the LOADED vertices plus whatever the target graph
 *   already holds: a dependency that names a guid present in the graph resolves
 *   to it, so a partially loaded store still wires into an existing graph. Load
 *   is not a MERGE, though: a record whose guid is already present in the target
 *   graph is skipped (counted, warned) instead of overwriting the live vertex,
 *   and any edge pointing at a guid that exists in neither place is skipped and
 *   counted in skipped_edge_count.
 *
 *   load() performs structural writes, so it must run on the graph's writer
 *   thread (graph.h's write policy) and on a graph no other thread is touching.
 *
 * FIBER NOTES
 *   Every fan-out is kimix::fiber::parallel over JOB IDS (0 .. slot_count) with an
 *   explicit per-claim size and task_limit = the ambient pool width capped at 8, so
 *   the serializer never claims more workers than it can use; each job fills the
 *   pre-sized slot its id names, so no two jobs touch the same memory. On a thread
 *   with no scheduler bound (worker_thread_count() == 1) parallel() degrades to
 *   running the bodies inline — the results and the order are identical, which is
 *   why the load is deterministic either way. Job bodies never block, never assert
 *   and never print: warnings are emitted single-threaded after the parallel pass,
 *   so stderr stays readable. The blocking LMDB batch runs outside every fiber.
 *
 * Example:
 *   kimix::graph_db::ColdStorage cold;
 *   cold.open(kimix::graph_db::default_cold_dir());
 *   kimix::graph_db::GraphSerializer serializer{cold};
 *   kimix::graph_db::GraphSerializeStats saved = serializer.save_incremental(graph);
 *   serializer.copy_to(checkpoint_dir);          // optional checkpoint
 *
 *   kimix::graph_db::Graph restored;
 *   kimix::graph_db::GraphDeserializer deserializer;
 *   kimix::graph_db::GraphSerializeStats loaded = deserializer.load(cold, restored);
 *   std::printf("%zu saved, %zu loaded, %zu skipped, %zu bad edges\n",
 *               saved.saved_count, loaded.loaded_count, loaded.skipped_count,
 *               loaded.skipped_edge_count);
 */
#pragma once

#include "graph_db_api.h"
#include "graph.h"
#include "utils.h"
#include "vertex.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/md5.h>
#include <core/stl/filesystem.h>
#include <core/stl/memory.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdio>

namespace kimix::graph_db {

/*
 * Outcome of one save or one load. Counts are exact; the timings are wall-clock
 * milliseconds of the whole call (fan-out included) and are informational only —
 * nothing in the library or the tests asserts on them.
 */
struct KIMIX_GRAPH_DB_API GraphSerializeStats {
    // Records successfully written by this save.
    size_t saved_count{0};
    // Vertices adopted into the target graph by this load.
    size_t loaded_count{0};
    // Records dropped instead of materialized: corrupt value, or a guid that is
    // already present in the target graph.
    size_t skipped_count{0};
    // Dependency edges that could not be resolved: missing target vertex, self
    // edge, or an edge Graph::connect refused.
    size_t skipped_edge_count{0};
    // Bytes of encoded records handed to the LMDB batch by this save.
    size_t bytes_written{0};
    // Bytes of scanned record values read from the store by this load (corrupt
    // ones included, since they were read before being rejected).
    size_t bytes_read{0};
    // Fan-out width used by the fiber parallel stages (1 == ran inline because
    // the calling thread had no scheduler bound).
    size_t worker_count{0};
    // Whole-call wall-clock time.
    double save_ms{0.0};
    double load_ms{0.0};
};

/*
 * Writes a Graph into a cold store: parallel encode (stage A), one atomic LMDB
 * transaction on the calling thread (stage B). The store is referenced, not
 * owned — it must outlive the serializer.
 */
class KIMIX_GRAPH_DB_API GraphSerializer : public kimix::IOperatorNewBase {
public:
    explicit GraphSerializer(ColdStorage &cold) noexcept
        : _cold(cold) {
    }
    GraphSerializer(GraphSerializer const &) = delete;
    GraphSerializer &operator=(GraphSerializer const &) = delete;
    GraphSerializer(GraphSerializer &&) = delete;
    GraphSerializer &operator=(GraphSerializer &&) = delete;
    ~GraphSerializer() noexcept = default;

    // True when the referenced cold store is open; every save returns empty
    // stats otherwise.
    [[nodiscard]] bool valid() const noexcept { return _cold.valid(); }

    /*
     * FULL save: mark every vertex dirty, then encode and write every vertex in
     * one transaction. Use it for a first save or a checkpoint. Records of
     * vertices that were removed from the graph are left in the store (see the
     * file header). Returns the stats of this call (also kept in last_stats()).
     */
    GraphSerializeStats save(Graph &graph);

    /*
     * INCREMENTAL save: encode and write ONLY the vertices whose dirty flag is
     * set. A clean vertex is never re-encoded and never rewritten, so the bytes
     * already in the store stay untouched. Returns the stats of this call.
     */
    GraphSerializeStats save_incremental(Graph &graph);

    // Compacted snapshot of the whole store into `target` (checkpoint).
    [[nodiscard]] bool copy_to(kimix::filesystem::path const &target) const noexcept {
        return _cold.copy_to(target);
    }

    [[nodiscard]] GraphSerializeStats const &last_stats() const noexcept {
        return _last_stats;
    }

private:
    // The shared pipeline behind save() and save_incremental().
    GraphSerializeStats _write(Graph &graph);

    ColdStorage &_cold;
    GraphSerializeStats _last_stats{};
};

/*
 * Reads a cold store into a Graph. Stateless: the store is passed per call, so
 * one deserializer may load several stores (or the same store into several
 * graphs) without any reset between calls.
 */
class KIMIX_GRAPH_DB_API GraphDeserializer : public kimix::IOperatorNewBase {
public:
    GraphDeserializer() noexcept = default;
    GraphDeserializer(GraphDeserializer const &) = delete;
    GraphDeserializer &operator=(GraphDeserializer const &) = delete;
    GraphDeserializer(GraphDeserializer &&) = delete;
    GraphDeserializer &operator=(GraphDeserializer &&) = delete;
    ~GraphDeserializer() noexcept = default;

    /*
     * Dual-stage load of every "v/" record of `cold` into `graph` (see the file
     * header for the stage split and the guarantees). Returns the stats of this
     * call (also kept in last_stats()). Never fails because of a dependency
     * problem and never fails because of a corrupt record: both are skipped and
     * counted. It returns empty stats only when the store is not open.
     */
    GraphSerializeStats load(ColdStorage &cold, Graph &graph);

    [[nodiscard]] GraphSerializeStats const &last_stats() const noexcept {
        return _last_stats;
    }

private:
    GraphSerializeStats _last_stats{};
};

} // namespace kimix::graph_db