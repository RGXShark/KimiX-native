# kimix-graph-db (`src/graph_db`)

The graph database layer of KimixBase: an in-memory vertex/edge container with a
two-level lock discipline, plus a parallel incremental serializer to LMDB cold
storage and a dual-stage, cycle/corruption-tolerant loader.

Design authority: `graph_db_plan.md` (§3.1 data model, §3.2 concurrency, §3.3
serialization format & disk layout, §3.4 error handling, §6 tests). The product
requirement it implements is `graph_db.md`.

---

## 1. Scope

- **In scope**: the in-memory graph (`Graph`, `Vertex`), the on-disk record
  format and key scheme, the LMDB cold store, incremental save, full save,
  dual-stage load, checkpoint copies.
- **Out of scope (later subtasks)**: the typed property model (a `PropertyValue`
  tagged union, labels, `(label, external_id)` keys — `Vertex::_content` is a
  deliberately opaque, already-serialized blob with a cached MD5 for now; see the
  TODO on `Vertex::_content`), the read/write benchmarks and the optimization
  epochs, sharded-cursor parallel loading, lazily hydrated content.
- **Dependency rule**: `kimix-core` only. Third-party code (LMDB, mimalloc) is
  reached through it, never by an `add_deps()` on a `src/ext` target.

## 2. File map

| File | Contents |
|---|---|
| `graph_db_api.h` | `KIMIX_GRAPH_DB_API` linkage-macro family (three states, static today). |
| `graph_db.h` | Umbrella include (`graph.h`, `utils.h`, `vertex.h`) + `KIMIX_GRAPH_DB_VERSION` (generated from `version.txt`). |
| `utils.h` / `utils.cpp` | Key scheme (`"v/"` + 16 binary guid bytes, `meta/root` reserved), the byte-exact `VertexRecord` codec (`encode_vertex_record` / `decode_vertex_record`, all little-endian), `guid_key` / `guid_from_key` / `md5_of`, filesystem helpers, and `ColdStorage` — a thin RAII front end over `kimix::LMDB` with batched transactional writes, a full scan and `copy_to()`. |
| `vertex.h` / `vertex.cpp` | One vertex: guid, opaque content blob + cached MD5, dependency links in **both** forms (guid vectors = the serialized source of truth; `Vertex*` sets = the resolved in-memory form), an `alignas(64)` `kimix::shared_atomic_mutex` and the atomic dirty flag. Also the `VertexUniqueLock` / `VertexSharedLock` RAII adapters. |
| graph.h / graph.cpp | The vertex container: a 64-way SHARDED vertex map (kGraphShardCount, one std::shared_mutex + one bucketed kimix::HashMap per shard), structural CRUD with a cascading remove_vertex (one shard lock at a time), connect / disconnect (endpoint shards, ascending order), connect_bulk (epoch 3: one endpoint resolution, one ascending lock set and one writer check for a whole edge batch), reserve_links, a version-keyed cached roots(), neighbors / dependencies / dependents plus the non-allocating AdjacencyBuffer / caller-span forms, cycle-safe BFS has_path / shortest_path (one shard lock per visit), dirty bookkeeping (mark_dirty / clear_dirty / dirty_vertices), the single-writer contract check. graph.h carries the full concurrency contract, the deadlock-freedom argument and the epoch-1/2/3 results. |
| `serialize.h` / `serialize.cpp` | `GraphSerializer` (full + incremental save, `copy_to` checkpoint) and `GraphDeserializer` (dual-stage load), plus `GraphSerializeStats`. Details in §4 and §5. |
| `xmake.lua` | The `kimix-graph-db` static target and its public include dirs / defines. |
| `README.md` | This file (module overview + the benchmark table S3 fills in). |
| `tests/unit/graph_db/graph_db_test_util.h` | Shared fixtures: byte helpers, `temp_dir_guard`, the known 6-vertex DAG, `build_layered_dag`, record/store hand-crafting helpers, raw-store snapshots, guid/adjacency comparison helpers. |
| `tests/unit/graph_db/test_graph_db.cpp` | Container suite: CRUD, cascade remove, edge directions, roots, threaded content races, writer contract, traversals, record codec, cold-storage round trip, cache-line/alignment checks. |
| `tests/unit/graph_db/test_graph_db_serialize.cpp` | Serializer suite: round trip, incremental writes, cycle/dangling stores, determinism, corruption, merge policy, checkpoint. |
| `tests/unit/graph_db/test_graph_db_bench.cpp` | Read / write / serialization benchmarks (subtask S3). |

## 3. Concurrency contract (summary — `graph.h` is authoritative)

Two independent lock levels, always acquired in the order **shard → vertex** and
never the other way round:

1. **Graph level** — `kGraphShardCount` (64) **shard locks**, each a
   `std::shared_mutex` on its own cache line guarding one bucketed
   `kimix::HashMap` of the vertex map (`shard = hash(guid) & 63`). Epoch 1
   replaced the single graph-wide mutex with them; §6 has the measured result.
   - **UNIQUE** for anything that changes the vertex set or the edge set:
     `create_vertex`, `add_vertex`, `remove_vertex`, `connect`, `disconnect` —
     one shard for the single-vertex writes, the endpoint shards in ascending
     order for `connect`/`disconnect`, and ONE shard at a time for
     `remove_vertex`'s cascade.
   - **SHARED** for pure reads and traversals: `find`, `contains`, `roots`,
     `neighbors`, `dependencies`, `dependents`, `has_path`, `shortest_path`,
     `dirty_vertices`, `mark_dirty`, `clear_dirty`; and for the guid→`Vertex*`
     resolution in `update_content` (released before the vertex lock is taken).
     `size()` takes no lock at all (an atomic count the writers maintain).
   - A traversal holds ONE shard lock per vertex visit: acquire, copy the
     adjacency into a reused `AdjacencyBuffer`, release.
2. **Vertex level** — one `kimix::shared_atomic_mutex` per vertex, `alignas(64)`
   so neighbouring vertices never share its cache line (no false sharing).
   - **SHARED** to read `content` / `content_hash`.
   - **UNIQUE** to write them (`set_content`, `touch`).

Write policy: **structural writes are single-threaded by contract.** The graph
records the id of the thread that performed the first structural write; a later
structural write from a different thread is refused (`false` / `nullptr`) and, in
debug builds only, also reported on stderr and asserted. Reads are unrestricted.
`update_content` is a level-2 operation and is explicitly exempt: N threads may
update N distinct vertices concurrently.

Deadlock freedom: shard locks are taken in ascending shard index whenever more
than one is needed, shard before vertex, and never the inverse. No path acquires a
shard lock while holding a vertex lock, so a wait-for cycle is impossible
(`graph.h` carries the full argument).

Read atomicity NARROWED in epoch 1: a single call is still self-consistent for its
subject vertex, but a multi-vertex traversal may now observe a structural mutation
on one visit and not the next — the epoch-0 whole-graph snapshot is what parallel
shard reads give up. Memory safety is unaffected (`remove_vertex` keeps the victim
alive for its whole cascade and returns it to the caller), and a caller that needs
a consistent whole-graph picture must keep the graph structurally quiescent,
exactly as the serializer's caller contract already required.

Serializer additions to the contract:

- Save may run concurrently with `update_content` (level-2 only). The record's
  content hash is recomputed from the bytes actually copied, so a racing update
  cannot produce a record whose stored MD5 disagrees with its own content, and a
  vertex whose hash changed mid-save is left dirty.
- Save must **not** overlap a structural write, and load performs structural
  writes, so both run on the graph's writer thread.
- `connect` / `disconnect` change both endpoints' serialized dependency lists but
  do not set their dirty flags; the caller marks the affected vertices (or calls
  `Graph::mark_dirty()` / `GraphSerializer::save()`, which marks everything).
- Removing a vertex does not delete its record; use
  `ColdStorage::remove_record()` or a checkpoint to drop it.

## 4. Save pipeline (`GraphSerializer`)

| Stage | Thread | Work |
|---|---|---|
| 1 collect | caller | `graph.dirty_vertices()` — graph SHARED lock for the map, each vertex's SHARED lock for the cached hash. This vector **is** the write set. |
| A encode | fiber fan-out | Per dirty vertex: `content_copy()` under the vertex SHARED lock, copy both guid lists, `encode_vertex_record` straight into its own `LMDBWriteCommand` slot. Jobs write disjoint slots, so no lock. |
| B write | **calling thread** | One `ColdStorage::save_batch()` = one LMDB transaction ⇒ the batch is atomic. Kept out of the fan-out: it is a blocking syscall batch and would park a pool worker. |
| settle | caller | Clear the dirty flag of each written vertex, but only when its cached hash still equals the written hash (a racing update stays dirty). |

- `save_incremental(graph)` — the primary path: only the dirty vertices are
  encoded and written. A clean vertex is never re-encoded and never rewritten, so
  its stored bytes stay bit-identical.
- `save(graph)` — full save: marks every vertex dirty first, then runs the same
  pipeline, so the whole graph is rewritten in one transaction.
- `copy_to(path)` — passthrough to `ColdStorage::copy_to()` (compacted snapshot).

## 5. Load pipeline (`GraphDeserializer`) — the dual-stage guarantee

| Stage | Thread | Work |
|---|---|---|
| 1 collect | sequential scan | `ColdStorage::load_all_records()` — one LMDB cursor (the iterator is single-cursor), every `"v/"` key copied out with its value. |
| 1' decode | fiber fan-out | `decode_vertex_record` per slot into (guid, hash, dependency guids, content, validity). No shared state. |
| 2 materialize | fiber fan-out | Bare `Vertex` per valid slot: guid + content, **no dependency list, no pointer, no edge**. |
| insert | caller | `Graph::add_vertex` per materialized vertex (a structural write, so single-threaded) + a staging guid→`Vertex*` index. |
| link | caller | Only after every vertex is in the graph AND after the reserve sweep: the whole recorded edge list goes to ONE `Graph::connect_bulk` call (epoch 3), which resolves each distinct endpoint once, takes each touched shard's unique lock once in ascending order, checks the single writer once, and fills both directions on both endpoints for every edge. |
| settle | caller | Loaded vertices are marked **clean** (they match the store), so an immediate incremental save writes nothing. |

**Why a cycle or a missing guid can never fail a load**: a vertex's existence
depends on nothing but its own record — no pointer is resolved and no edge is
created while vertices are being inserted, so a dependency problem cannot reject
a record, and a cycle is simply two resolvable edges once everything exists.
Unresolvable dependencies (missing target, self edge, refused edge) are skipped,
counted in `skipped_edge_count` and warned about on stderr. A corrupt record (bad
magic, truncated, impossible dependency counts, or an embedded guid that
disagrees with its key) is skipped, counted in `skipped_count` and warned about;
the rest of the store loads. Load is not a merge: a record whose guid already
exists in the target graph is skipped rather than overwriting the live vertex,
while an edge may still resolve to a vertex the graph already held.

Determinism: the fan-outs fill pre-sized slots by index, and the link pass walks
the slots in LMDB key order, so loading the same store twice yields identical
guid sets and identical adjacency regardless of scheduling.

6. Benchmarks

test_graph_db_bench.cpp (subtask S3) reports read, write and serialization
numbers to stderr via tests/unit/native/bench_util.h; there are no timing
asserts. Run it with `xmake run test_graph_db_bench` (release) — the whole suite
takes ~1 minute, and the 1M-vertex serialization pass is reduced to 200k in
debug builds so a debug run stays a quick smoke check.

Workload (all sizes are the constants at the top of the bench file):

- READ graph — 100k vertices, out-degree 4..8 acyclic by construction, 64-byte
  content. One "user op" = find + content read + dependencies + neighbors + a
  depth-3/node-64-bounded expansion + has_path + shortest_path on a pre-sampled
  1-2-hop pair. Reported single-threaded and with cores/2, cores, 2*cores plain
  std::threads (20 hardware threads here ⇒ 10/20/40), 1 s per configuration.
  Per-call baselines (find, content read, dependencies, neighbors, bounded reach,
  has_path, shortest_path) are printed too: they are the epoch-1 profile start.
- WRITE — (a) 200k create_vertex+connect on a fresh graph from ONE thread (the
  single-writer contract); (b) update_content from W threads on DISJOINT
  partitions of a prebuilt 40k pool, next to a `Vertex::set_content` probe that
  takes the vertex lock only (isolates the graph-lock tax from the level-2 path);
  (c) mixed: 1 structural writer + cores content updaters, plus the same
  structural loop alone as the starvation baseline.
- SERIALIZE — fresh 100k graph and a fresh 1M graph: full save, dual-stage load
  and a 1 %-dirty incremental save (saved_count == N/100 is asserted), warmup pass
  then measured pass, timed by steady_clock and cross-checked against the
  serializer's own save_ms / load_ms. bytes_written / bytes_read are reported.

Table columns are fixed per epoch row so rows stay comparable:

- machine — CPU model / hardware threads / RAM of the host that ran the epoch.
- mode — xmake build mode (always release for a recorded epoch) + commit.
- epoch — 0 is the baseline; each later epoch is one optimization pass
  (graph_db_plan.md §7). Stop when a full epoch yields < 5 %.
- read 1T / read 10T/20T/40T — user-ops per second, aggregate over the threads.
- content w/s 1T, W=10/20/40 — update_content per second, aggregate; the
  `vertex-lock only` number in parentheses is the same workers calling
  Vertex::set_content, i.e. what the level-2 path would do without the graph lock.
- structural w/s — create+connect pairs per second, single thread.
- save/load/incremental — milliseconds (seconds for 1M).

| machine | mode | epoch | date | read 1T (ops/s) | read 10T/20T/40T (ops/s) | content w/s 1T | content w/s 10T/20T/40T | content w/s, vertex-lock only 10T/20T/40T | structural w/s 1T | mixed: structural vs content (w/s) | save 100k | load 100k | incr 1 % @100k (1000 rec) | save 1M | load 1M | incr 1 % @1M (10k rec) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Intel Core Ultra 7 265K, 20 hw threads, 64 GB | release | 0 | 2026-10-21 | 256 327 | 473 933 / 416 047 / 414 033 | 4 642 309 | 7 306 194 / 6 141 672 / 6 307 799 | 45 525 895 / 86 915 505 / 79 383 416 | 1 064 918 | 320 struct (1 365 960 alone, run-to-run 107-1024: the unique lock is unfair) vs 5 819 021 content | 1121 ms (30.0 MB) | 285 ms (30.0 MB, 105 MB/s) | 114 ms (744 KB) | 12 075 ms (300.1 MB) | 4 837 ms (300.1 MB, 62 MB/s) | 949 ms (7.4 MB) |
| Intel Core Ultra 7 265K, 20 hw threads, 64 GB | release | 1 | 2026-10-21 | 276 240 | 2 364 399 / 4 091 673 / 4 041 547 | 4 496 762 | 34 989 715 / 66 475 564 / 60 075 280 | 45 496 126 / 88 818 892 / 79 604 865 | 1 119 361 | 945 946 struct (1 363 556 alone) vs 56 952 424 content | 1145 ms (30.0 MB) | 308 ms (30.0 MB, 102 MB/s) | 100 ms (744 KB) | 12 403 ms (300.1 MB) | 4 911 ms (300.1 MB, 61 MB/s) | 861 ms (7.4 MB) |
| Intel Core Ultra 7 265K, 20 hw threads, 64 GB | release | 2 | 2026-10-21 | 302 046 | 2 556 243 / 4 455 914 / 4 026 305 | 4 639 496 | 35 226 859 / 67 249 889 / 56 567 417 | 45 757 777 / 88 481 725 / 83 060 833 | 1 149 725 | 920 702 struct (1 352 563 alone) vs 58 398 088 content | 774 ms (30.0 MB, 38.8 MB/s) | 232 ms (30.0 MB, 129 MB/s) | 74 ms (744 KB) | 8 124 ms (300.1 MB, 36.9 MB/s) | 3 842 ms (300.1 MB, 78 MB/s) | 737 ms (7.4 MB) |
| Intel Core Ultra 7 265K, 20 hw threads, 64 GB | release | 3 | 2026-10-21 | 280 503 | 2 562 777 / 4 467 412 / 4 105 975 | 4 562 709 | 35 195 843 / 67 095 474 / 61 565 193 | 45 722 950 / 89 113 384 / 81 686 037 | 1 123 154 | 1 057 298 struct (1 334 778 alone) vs 57 844 083 content | 786 ms (30.0 MB, 38.1 MB/s) | 168 ms (30.0 MB, 178 MB/s) | 78 ms (744 KB) | 8 576 ms (300.1 MB, 35.0 MB/s) | 3 099 ms (300.1 MB, 97 MB/s) | 776 ms (7.4 MB) |

Epoch-1 row caveat (read this before comparing the two rows): the host carried
unrelated desktop load (Windows reported 60-99 % CPU in OTHER processes) for the
whole epoch-1 session, and this CPU is hybrid (8 P-cores + 12 E-cores), so a
single run's ns/op swings 2-3x with which core a worker lands on — the untouched
`content_read_only` control moved 49.5 -> 64.4 ns between two idle-ish runs,
which is machine noise, not code. Every epoch-1 cell above is therefore the
MINIMUM ns/op observed for the COMMITTED code over the whole session (4 whole-suite
runs, including one interleaved with a control build), i.e. the uncontended
P-core number every configuration is equally entitled to. The epoch-0 row was a
single clean-machine run, which is why the two rows are close to comparable at
1T but NOT at the loaded 10/20/40T settings. The authoritative epoch-1 verdict is
the interleaved same-binary A/B below, not a row-to-row read.

Single-thread per-call breakdown from the same run (epoch-1 profile input):
find 14.9 ns, content read 49.5 ns, dependencies 67.8 ns, neighbors 369 ns,
has_path (1-2 hops) 405 ns, shortest_path (1-2 hops) 534 ns, and the
depth-3/node-64 bounded expansion 2795 ns — that expansion is ~72 % of the whole
user op. The find-only probe under threads: 67.1M ops/s at 1T vs 84.5M / 72.4M /
83.6M at 10T / 20T / 40T — only ~1.25x, with the per-call cost going from 14.9 ns
to 118-138 ns. That plateau is the read ceiling.
Epoch-0 findings (what epoch 1 should attack, in measured order):

1. READ does not scale: 256k user-ops/s on 1 thread, and 414-474k with 10/20/40
   threads — a ~1.8x ceiling, and adding threads does not reach it either (40T is
   no better than 20T). The find-only probe above isolates the cause: every
   structural read takes the one graph std::shared_mutex in SHARED mode, so the
   lock word is the bottleneck, not the container. Sharding that lock (or moving
   traversal to a per-vertex-lock + guid-snapshot scheme) is the single biggest
   win available.
2. The 1T user op is dominated by the traversal, not by lookup: find 14.9 ns,
   content read 49.5 ns, dependencies 67.8 ns, has_path 405 ns, shortest_path
   534 ns, but the depth-3/node-64 bounded expansion is 2795 ns — ~72 % of the
   whole op. Each `dependencies()` call pays a shared-lock acquire plus a full
   `kimix::vector` copy of the guid list; a ~30-vertex expansion issues ~30 of
   them. Epoch 1: an adjacency accessor that does not allocate, and a bounded
   BFS that walks its inner loop under one lock acquisition.
3. LEVEL-2 WRITES are the one thing that already scales and then some:
   update_content goes 4.64M -> 7.31M ops/s (1.57x at 10T), while
   `Vertex::set_content` on the same disjoint partitions goes 5.05M -> 86.9M
   ops/s (17.2x at 20T). The gap IS update_content's graph-SHARED-lock guid
   resolution; the per-vertex alignas(64) locks do not false-share at all (11.5
   ns/op at 20T is a lock-free-looking write). Removing the graph lock from the
   update path (epoch 1: resolve the guid once per partition, or a
   lock-free/per-shard guid index) is worth up to 17x on this bench.
4. The mixed phase is the contract's honest cost: 320 structural ops/s with 20
   updaters running vs 1 365 960/s alone, i.e. ~4000x writer starvation. This is
   the least reproducible number in the table by far — repeated runs produced 54,
   129, 161 and 514 creates in the same 1 s window, which is itself the finding:
   the unique graph lock is not fair, so a structural writer waiting behind a
   constant stream of shared-lock readers makes progress in bursts. Content updates
   meanwhile lose almost nothing (5.82M/s mixed vs 6.14M/s alone) — they are
   correctly on the other lock level. Epoch 2 candidate: a writer-preferring (or
   queued) graph lock so a structural writer cannot starve.
5. SERIALIZATION scales acceptably but is LMDB-bound, not CPU-bound: the save
   fan-out is capped at 8 workers and 1M vertices cost 12.1 s (24.9 MB/s), while
   the dual-stage load reaches 62 MB/s at 1M. The 1 % incremental save is 949 ms
   at 1M for 10 000 records (7.4 MB) — 12.7x cheaper than the full save, so the
   dirty tracking pays off. Epoch 1/2: batch-size and map-size tuning, sharded
   load cursors, and checking whether the encode fan-out or the single LMDB write
   transaction is the cost (stats.save_ms == wall time here, so stage B
   dominates: 8 workers of parallel encode finish inside a memory-bound ~1 s
   write transaction).

Epoch-1 notes (read scaling, non-allocating adjacency, sharded write path)

WHAT CHANGED (all in src/graph_db, gated by the unchanged tests):

1. The single graph-wide `std::shared_mutex` became 64 SHARD locks
   (`kGraphShardCount`, power of two; `shard = hash(guid) & 63`), each owning one
   bucketed `kimix::HashMap` on its own cache line. A reader of vertex V touches
   only V's shard; structural writes take the endpoint shard(s) UNIQUE in
   ascending shard order (connect/disconnect touch at most two; `remove_vertex`
   takes ONE shard at a time and releases it between neighbours, so it never
   holds two). Traversals copy one vertex's adjacency under one shard shared lock
   into a local buffer and release it before the next hop, so a BFS holds no lock
   between visits and parallelizes across shards. Lock order is written down in
   graph.h, with the deadlock-freedom argument and the narrowed read-atomicity
   guarantee (per-vertex atomic instead of whole-graph snapshot).
2. Non-allocating adjacency: `AdjacencyBuffer` (16 guids inline, one retained
   heap vector beyond) plus `size_t dependencies(guid, span<Guid>)` (snprintf
   convention: returns the TOTAL count). `dependencies`/`dependents`/`neighbors`
   all use that discipline internally; `has_path`/`shortest_path` reuse one buffer
   per traversal and replaced their per-hop hash-set membership test with a
   linear scan of the BFS queue up to 32 discovered vertices (the queue IS the
   visited set), falling back to a hash set above that.
3. The level-2 write path lost the global lock: `update_content` takes ONE shard
   SHARED lock for the guid -> Vertex* lookup and releases it before the vertex
   unique lock. This is the option the epoch-1 brief called the "cheaper correct
   step" — proof sketch in graph.h: the shard lock is held only across the map
   read, the returned pointer is stable because removal is a structural write
   excluded by the single-writer contract, and the vertex's own lock then
   excludes every other content access.
4. `neighbors()` dedups into a 16-entry stack array and materialises the result
   vector once (was: a `kimix::unordered_set` per call). The serializer now reads
   the dependency lists through the shard-locked Graph accessors instead of the
   raw `Vertex` accessors (the raw ones are no longer safe against a concurrent
   reader, and `dirty_vertices()`/save are documented as runnable next to
   level-2 updates).

MEASURED BEFORE/AFTER. Rows compare the committed epoch-0 code with the epoch-1
code; because the host was loaded, the authoritative comparison is the
interleaved same-session A/B between the epoch-1 build and the SAME code compiled
with `KIMIX_GRAPH_DB_SHARD_COUNT=1` (which reproduces the epoch-0 single-lock
structure exactly). min-of-N, whole suite, alternating runs:

| metric | 1-shard (== epoch 0) | 64-shard (epoch 1) | delta |
|---|---|---|---|
| read 1T user op | 3 502 ns/op | 3 516 ns/op | +0.4 % |
| read 10T user op | 2 503 ns/op | 448 ns/op | **-82 %** |
| read 20T user op | 2 876 ns/op | 241 ns/op | **-92 %** |
| read 40T user op | 2 830 ns/op | 253 ns/op | **-91 %** |
| find-only 10T | 147 ns/op | 9.7 ns/op | **-93 %** |
| find-only 20T | 159 ns/op | 6.0 ns/op | **-96 %** |
| find-only 40T | 126 ns/op | 7.3 ns/op | **-94 %** |
| update_content 10T | 143 ns/op | 29 ns/op | **-80 %** |
| update_content 20T | 167 ns/op | 15 ns/op | **-91 %** |
| update_content 40T | 159 ns/op | 17 ns/op | **-90 %** |
| Vertex::set_content 20T (control) | 11.4 ns/op | 11.6 ns/op | +2 % |
| structural 1T | 830 ns/op | 892 ns/op | +7 % |
| mixed structural (1 writer + 20 updaters) | 19.6 ms/op (51 creates/s) | 931 ns/op (1.07M/s) | **~20 000x** |
| save/load/incremental | — | — | within +/-8 % (LMDB-bound) |

Aggregate headline move against the recorded epoch-0 row: read
474k/416k/414k -> 2.36M/4.09M/4.04M ops/s (5.0x/9.8x/9.8x) and update_content
7.3M/6.1M/6.3M -> 35.0M/66.5M/60.1M ops/s (4.8x/10.8x/9.5x). The contended-find
plateau epoch 0 measured (7-8M ops/s per thread at 118-138 ns) is gone: 10/20/40
threads now reach 102M/168M/140M find ops/s aggregate, i.e. 9.8/6.0/7.1 ns per
call — the same as single-threaded (16.5 ns), so the read lock is no longer the
ceiling. Epoch-0's finding #1 ("read does not scale: a ~1.8x ceiling") is
answered: the ceiling is now the machine, not the lock.
The 1T op is unchanged by the sharding itself (the shard index and the extra
bounds check cancel against the cheaper lock), but its COMPONENTS moved:
`neighbors` 369 -> 186 ns (stack dedup) and the depth-3 bounded expansion
2 795 -> 3 108+ ns depending on machine state (the expansion now uses the
non-allocating form, but this host's load makes that comparison unsafe to
over-read; the allocation removal is visible in `dependencies` 67.8 -> 63.2 ns
for the buffer form).

Correctness was NOT traded for speed. The narrowed guarantee is read atomicity:
a multi-vertex traversal may now observe a structural mutation on one visit and
not the next (it can no longer see an inconsistent single vertex, ever). That is
inherent to letting readers of different shards run in parallel, it is documented
in graph.h, and it is what the serializer's "structurally quiescent" caller
contract already assumed. Memory safety is preserved: `remove_vertex` keeps the
victim alive for the whole cascade and returns it to the caller, so no reader can
dereference a freed Vertex. Verified by an 8-reader / 1-writer stress harness
(create + connect + cascade remove, 80 000 removals, readers resolving and
reading vertices mid-removal): zero invariant violations, zero dangling edges,
post-run sweep clean; ASan debug and release runs of both test binaries are
clean, and all 216 + 303 assertions pass with the tests UNMODIFIED.

EPOCH 3 SHOULD ATTACK, in measured order (rewritten by epoch 2, which answered
the previous list):

1. LMDB durability policy — THE finding of epoch 2, and the only remaining lever
   on save. LMDB on Windows opens its data file with FILE_FLAG_WRITE_THROUGH, so
   every 4 KiB page of a commit is a separate synchronously-completed
   write-through WriteFile: measured 105 us per 4 KiB page through an overlapped
   write-through handle against 5.7 us buffered, i.e. **~40 MB/s hard ceiling on
   the commit**, and that is what the save numbers track (30.0 MB in 774 ms = the
   serialized-page count, not the payload). `kimix::LMDB` passes only
   `MDB_NORDAHEAD` (src/core/lmdb.cpp), so MDB_NOSYNC / MDB_NOMETASYNC /
   MDB_WRITEMAP are unreachable from src/graph_db — a 100k save commit drops from
   712 ms to 38 ms with MDB_NOSYNC in the standalone probe. Making the sync policy
   a parameter of `kimix::LMDB` (and a ColdStorage knob) is a src/core change that
   this epoch was not allowed to make, and it is the highest-value next step.
   Epoch-2's sort already banked the part that needs no policy change.
2. Load's link pass is still ~80 % of a load (2.4 s of 3.8 s at 1M after the
   epoch-2 reservation): the remaining cost is Graph::connect per edge, i.e. two
   shard unique locks plus two vertex uniques plus the duplicate probe. A
   bulk-link entry point that takes the endpoint shards once for a whole
   adjacency list (still one edge at a time for correctness, still single-writer)
   is the next step, and it must not weaken the dual-stage guarantee.
3. Stage 1 of the load is now the second-largest piece at 1M (135-330 ms scan +
   19-31 ms slot build + 55-58 ms decode). Sharded cursors stay blocked on
   `kimix::LMDBIterator` (one cursor per iterator, no key-range seek in the
   wrapper), so the available win is avoiding the key+value copy in
   load_all_records, not concurrency.
4. Structural 1T and the 1T user op are both at parity with epoch 1 (867 vs 879
   ns/op; 3.31 vs 3.34 us). Epoch 2 measured the two candidates and neither pays:
   reserving the per-vertex edge containers at construction is a wash (955 vs 955
   ns/op), and the MD5 inside create_vertex is 180 ns of the 900 (20 %) but is
   required by the content-hash contract. Caching the thread-id hash is also a
   wash (927 vs 930). A real win there needs a different content-hash policy, not
   a code tweak.
5. roots() / dirty_vertices() are still O(n) with one lock acquisition per shard;
   the incremental serializer calls the latter on every save (1.0 ms at 100k).

Epoch 1 improved the headline read and level-2-write numbers by far more than the
5 % stop threshold (read 5.0x/9.8x/9.8x, update_content 4.8x/10.8x/9.5x, find-only
~13-25x), so the loop continues to epoch 2.

EPOCH-2 NOTES (serialization: order the save, size the load)

PROFILE FIRST — where the time actually went (temporary stderr probes, removed
from the committed code; all numbers release, 100k graph unless stated):

- SAVE was 96 % LMDB and 0.02 % the encode fan-out. `save_ms` decomposes as
  collect 9 ms, encode 0.2-27 ms, sort 14 ms, **one `write_all` transaction
  1139-1413 ms**. A standalone C probe against the vendored `mdb.c` (same TU
  flags, same volume) reproduced it and explained it: `mdb_txn_commit` is
  680-780 ms of the 1040 ms while `mdb_put` is 330-350 ms, and neither
  `map_size` (1 GiB vs 4 GiB vs 64 GiB: 848/1336/1120 ms) nor `MDB_NOMETASYNC`
  (1167 ms) moves it. `MDB_NOSYNC` moves it to 386 ms. Root cause read out of
  mdb.c: on Windows the data file is opened FILE_FLAG_WRITE_THROUGH and dirty
  pages are flushed individually through an overlapped write-through handle.
  Measured directly on this host: 105 us per 4 KiB write-through page against
  5.7 us buffered. That is the 4-8x "slower than load per byte" the brief asked
  about, and it is not the encoder, not the batch shape, and not `src/graph_db`.
- The 1 % incremental at 1M (10k records, 7.4 MB) taking 1.27 s was NOT a
  suspicious fixed overhead: 7.4 MB of payload is ~9.2 MB of fresh + COW pages,
  i.e. ~2300 write-through pages. The probe puts the same batch at 10.7 ms per
  1k records warm, and at 87 ms per 1k records when it is the first commit after
  a full save (the OS cache is still flushing the 300 MB that save just wrote).
  A 1-record transaction into a warm store commits in 0.09 ms and an empty one
  in ~0 ms, so there is no per-transaction fixed cost to remove in src/graph_db.
- LOAD was 87 % link pass, and stage 1 was never the problem: 100k decomposes as
  scan 12-31 ms, slot build 2-3 ms, decode 5-6 ms, materialize 4 ms, insert
  10-12 ms, **link 245-329 ms**. Inside the link pass, 600 054 edges cost
  connect 197-213 ms + a `Graph::find` per edge 77-80 ms + a made-a-copy
  `pending` list 10 ms; and inside connect, the four per-vertex edge containers'
  geometric growth was 170-302 ms of 600k calls. So: the pass's cost was
  per-VERTEX container growth, not the per-edge work.

WHAT CHANGED (src/graph_db only, tests unmodified):

1. `serialize.cpp` sorts the finished save batch into LMDB key order (stage A',
   after the encode fan-out, before the single transaction). The key is "v/" + 16
   raw guid bytes, i.e. already the bytes LMDB compares, so the comparator is a
   memcmp. This turns a random-insert B-tree build into a bulk-load one: 100k
   went from 11.9k to 9.2k dirty pages. Nothing else about the save changes — same
   records, same bytes per record, same ONE transaction (still atomic), same
   single-writer contract, and the dirty-flag settle pass still reads the
   collection-ordered `dirty[]`/`written_hashes[]`, which the sort does not touch.
2. `Graph::reserve_links(guid, outgoing, incoming)` (graph.h/.cpp) +
   `Vertex::reserve_links` (vertex.h/.cpp): reserve room for the four per-vertex
   edge containers. `reserve()` only ever RAISES capacity and never touches an
   element, so it cannot add, drop or reorder an edge. The deserializer's link
   pass now runs two sweeps: reserve every vertex's recorded degree (the record
   carries both counts), then create the edges. This is a structural write, so it
   takes that one shard's UNIQUE lock and obeys the single-writer contract like
   every other structural mutation; a no-op for an unknown guid.
3. Documentation only: the before/after numbers and the LMDB-durability finding
   are written into graph.h (EPOCH-2 RESULT), serialize.h (stage A', the sorted
   order, the two-sweep link pass) and this file.

MEASURED BEFORE/AFTER. All numbers are release, same machine, same session,
min-of-N whole-suite runs interleaved with the epoch-1 binary as the control
(3 epoch-2 runs, 2 epoch-1 runs; 100k/1M serialization also A/B'd 4x/2x on its
own because a whole suite is 55 s):

| metric | epoch 1 | epoch 2 | delta |
|---|---|---|---|
| save 100k | 1015 ms | 774 ms | **-23.7 %** |
| load 100k | 266 ms | 232 ms | **-12.7 %** |
| incremental 1 % @100k | 76 ms | 74 ms | -2.1 % (noise) |
| save 1M | 10 570 ms | 8 124 ms | **-23.1 %** |
| load 1M | 4 723 ms | 3 842 ms | **-18.6 %** |
| incremental 1 % @1M | 827 ms | 737 ms | -11.0 % |
| read 1T user op | 3338 ns | 3312 ns | -0.8 % (unchanged) |
| read 10/20/40T user op | 387/222/239 ns | 391/223/247 ns | +1.2/+0.5/+3.3 % (noise) |
| find-only 1T / 10/20/40T | 16.8 / 9.3/5.8/6.9 ns | 15.8 / 9.3/5.8/7.0 ns | flat |
| content read / deps / neighbors (controls) | 53.5 / 61.9 / 179.8 ns | 53.5 / 62.0 / 178.2 ns | flat |
| 1T bounded reach (3 hop / 64 nodes) | 2431 ns | 2316 ns | -4.7 % (noise) |
| structural 1T create+connect | 879 ns | 867 ns | -1.4 % (unchanged) |
| update_content 1T / 10/20/40T | 218 / 28.4/14.8/17.2 ns | 214 / 28.4/14.8/16.8 ns | flat |
| Vertex::set_content 20T (control) | 11.2 ns | 11.2 ns | flat |
| mixed: structural / content | 1030 / 17.0 ns | 1049 / 17.0 ns | flat |

Isolating the two changes inside ONE binary (temporary env knobs, removed from
the committed code) so host drift cannot be blamed: 100k save with the sort off
1138/1168/1183/1193 ms vs on 844/892/854/865 ms (**-26 %**, i.e. the whole save
win is the sort — the sort itself costs 14-16 ms of that); 100k load with the
reservation off 262/268/294/302/328 ms vs on 241/237/248/250/253 ms (**-10 %**,
5 interleaved reps, and the two effects are independent). At 1M the same two
changes give save 10.75 -> 8.03 s and load 4.70 -> 3.92 s.

HONEST LIMITS OF THIS EPOCH. The headline read/write/traversal numbers did not
move, and they were not supposed to: epoch 2 changed the serializer and added one
structural hook that only the loader calls, so every other path had to measure
flat — and it did, within +/-3 % run-to-run noise on this hybrid host. The
remaining save time is now 95 % one LMDB commit whose cost is the Windows
write-through page policy inside src/core/lmdb.cpp (LMDB itself, not the
wrapper's misuse of it); the only ways past it are an env flag this library
cannot reach (MDB_NOSYNC: 38 ms for the same 100k commit in the probe) or
splitting the atomic transaction, which is a contract change. Epoch 2 therefore
lands at -23.7 % save / -12.7 % load at 100k and -23.1 % / -18.6 % at 1M, i.e.
above the 5 % stop threshold on the targeted paths and exactly 0 % everywhere
else — so the loop continues, but the NEXT epoch's honest expectation is that the
serialization win must come from src/core (a sync-policy knob) or from the load's
remaining link-pass cost, not from more work inside src/graph_db.

CORRECTNESS ARGUMENT (nothing was traded). The dual-stage load guarantee is
untouched: no pointer is resolved at insert time, `reserve_links` runs in a
separate sweep AFTER every vertex is in the graph and before any edge exists, and
it only raises capacity. Edge creation still goes through `Graph::connect`, in the
same LMDB-key order and the same per-record list order, so the resulting guid
sets, adjacency and a subsequent save's bytes are bit-identical — verified by the
existing round-trip, determinism and incremental-byte-identity tests, all
unmodified. The incremental-save byte-identity property survives: the sort
reorders the WRITE BATCH, never the set of records, and a clean vertex is still
never re-encoded or rewritten. The single-writer contract is preserved
(`reserve_links` calls `_check_writer()`), lock ordering is preserved (shard
unique, never two shards at once, no vertex lock held while a shard lock is
taken), and the dual-stage/incrementality tests assert exactly the same counts as
before. Verified: debug (216 + 303 asserts + the bench's 43, green), ASan debug
(both suites green, no reports), release (bench + both suites green); no changes
outside src/graph_db.

Rows are appended, never rewritten: a later epoch that trades correctness for
speed is rejected, and every number must come from the same machine and mode as
the row above it.

EPOCH-3 NOTES (bulk edge creation + a cached roots())

PROFILE FIRST — where the time actually went (temporary stderr probes in
serialize.cpp, removed from the committed code; release, 100k store, the load's
own stage marks):

- The load decomposed as scan 8-22 ms, slot build 2 ms, decode 4 ms,
  materialize 6 ms, insert 10 ms, reserve sweep 16-28 ms, EDGE SWEEP 183-283 ms,
  settle 0.8 ms — i.e. the edge sweep was 78-85 % of the whole load, exactly what
  epoch 2 predicted.
- Inside the edge sweep, instrumenting the two calls per edge put Graph::find at
  77-80 ms and Graph::connect at 106-180 ms of a 283 ms sweep (the probe itself
  added ~100 ms of clock reads, which is why the sweep inflated from 183 to 283 ms
  while the breakdown was taken). So the find elimination was worth roughly a
  quarter of the pass and the lock choreography the rest.
- roots(): 0.593 ms at 100k vertices (14268 roots), one shared lock acquisition
  per shard. Nothing in the bench suite hammered it, but it is O(n) on demand and
  the incremental serializer's dirty_vertices() neighbour, so it was the second
  item.

WHAT CHANGED (src/graph_db only, tests unmodified):

1. Graph::connect_bulk(span<EdgeRequest const>, span<EdgeOutcome>) (graph.h/.cpp)
   + EdgeRequest / EdgeOutcome, and the deserializer's link pass now builds ONE
   edge list and hands it to ONE bulk call instead of N per-edge Graph::connect
   calls. Three things are hoisted out of the per-edge loop:
   (a) ENDPOINT RESOLUTION — every DISTINCT endpoint is resolved once into a
   staging guid -> endpoint-id index (one shard SHARED lock per unique endpoint,
   i.e. ~100k lookups instead of ~600k Graph::find calls at 100k vertices);
   (b) SHARD LOCKING — the set of touched shards is collected and then taken
   UNIQUE in ascending index order, once each, for the whole batch, so no edge
   pays an acquire/release pair and a same-shard edge needs no ordering decision;
   (c) THE WRITER CHECK — one _check_writer() per batch instead of per edge.
   The edge-creation body itself is shared, not copied: connect() and
   connect_bulk() both call _connect_resolved_locked, so the duplicate probe, the
   two vertex UNIQUE locks and the two-sided _add_depending/_add_depended
   bookkeeping are literally the same code. Vertex::_add_depending_if_absent is
   new and collapses the duplicate probe and the insert into one hash operation
   (measured separately: 118-134 ms -> 108-122 ms on the edge phase).
2. roots() is now a version-keyed cache (graph.h/.cpp): a `std::atomic<uint64_t>
   _structure_version` bumped by ONE relaxed increment inside each structural
   writer (create/add/remove vertex, connect/disconnect, connect_bulk — all
   already holding their shard lock), plus a dedicated `std::shared_mutex
   _roots_mutex` guarding the cached vector. A warm call is a shared-lock version
   compare plus one vector copy; a stale one takes the mutex unique and reruns the
   old O(n) scan (_compute_roots). Level-2 content writes and reserve_links do
   NOT bump: they cannot change the root set, which is defined by INCOMING EDGES.
3. Load stage 1 (item 3 of the brief) was measured and LEFT ALONE: at 8-22 ms of
   a 166 ms load it is now ~5-13 %, and item 1 cut the denominator.
4. Documentation only: the before/after numbers, the equivalence argument and the
   lock-order/versioning rules are written into graph.h (EPOCH-3 RESULT, LOCK
   ORDER item 3), serialize.h (the link pass) and this file.

MEASURED BEFORE/AFTER. Release, same machine, same session, min-of-N interleaved
A/B against the epoch-2 binary (kept as a copy in .kimix_cache/epoch3/, 6-12
whole-suite or single-suite runs alternating new/old):

| metric | epoch 2 | epoch 3 | delta |
|---|---|---|---|
| load 100k | 229.2 ms | 160.8 ms | **-29.9 %** |
| load 1M | 3892.6 ms | 3058.0 ms | **-21.4 %** |
| save 100k | 738.6 ms | 751.8 ms | +1.8 % (noise; untouched, LMDB-bound) |
| save 1M | 8276 ms | 7950 ms | -3.9 % (noise; per-run spread 7 950-9 433 ms) |
| incremental 1 % @100k | 69.2 ms | 68.5 ms | -1.1 % (noise) |
| incremental 1 % @1M | 753.4 ms | 729.0 ms | -3.2 % (noise) |
| roots() @100k, warm | 0.593 ms | 0.003 ms | **-99.4 % (152-172x)** |
| structural 1T create+connect | 881.7 ns | 881.2 ns | -0.06 % (unchanged) |
| read 1T / 10T / 20T / 40T user op | 3333.7 / 390.6 / 222.4 / 245.0 ns | 3434.0 / 388.7 / 222.8 / 249.3 ns | +3.0/-0.5/+0.2/+1.8 % (noise) |
| find-only 1T / 10T / 20T / 40T | 15.7 / 9.3 / 5.8 / 7.1 ns | 15.8 / 9.3 / 5.7 / 6.9 ns | flat |
| content read / deps / deps-noalloc / neighbors (controls) | 54.2 / 69.9 / 60.7 / 176.8 ns | 53.0 / 69.8 / 64.0 / 179.5 ns | flat |
| update_content 1T / 10T / 20T / 40T | 219.4 / 28.4 / 14.9 / 19.1 ns | 220.0 / 28.4 / 14.9 / 18.8 ns | flat |
| Vertex::set_content 1T / 10T / 20T / 40T (control) | 192.6 / 21.8 / 11.2 / 11.9 ns | 195.2 / 21.8 / 11.2 / 12.1 ns | flat |

Isolating each change inside ONE binary so host drift cannot be blamed:
- bulk edge creation, the only two temp knobs that shipped into the measurement
  build — single-insert off 133.8/120.4/126.7/120.4/123.0 ms vs on
  111.8/107.9/109.0/109.8/110.2 ms on the edge phase, so the one-hash-operation
  insert is ~-9 % of that phase and was kept;
- roots() bump cost, interleaved same-binary A/B with the increment compiled out:
  bump on 897.3 ms median 910.1 vs off 897.8 / 910.0 ns — -0.06 % / +0.02 %,
  i.e. a relaxed atomic increment inside a lock the writer already holds is free.
- A permanent-cost check that mattered: the FIRST version of connect_bulk stored
  Vertex* into the staging kimix::unordered_map (ankerl::unordered_dense), whose
  values MOVE on rehash; a 1M probe segfaulted. The committed version stores
  endpoint IDS into a stable kimix::vector and a guid -> id map, so a rehash only
  moves the ids.

CORRECTNESS ARGUMENT (nothing was traded). The bulk-edge equivalence was proven
mechanically rather than argued: a temporary env-gated harness (KIMIX_GDB_VERIFY_
LINK, removed from the committed code) re-ran the SAME decoded slots through the
OLD per-edge Graph::find + Graph::connect loop into a second Graph, then compared
the two graphs with the public accessors — every vertex's sorted outgoing guid list
and sorted incoming guid list — plus the vertex count and the skipped-edge count.
Result over the whole serialize suite: 11 loads compared, 0 adjacency mismatches,
skipped counts equal in every case (13/13 EQUIVALENT; the one "merge into a
non-empty graph" case is reported SKIPPED because the harness's second graph is
rebuilt from store slots only and legitimately lacks the pre-existing vertices the
bulk path links to). At scale on a 1M store: 1000000 vertices, 6 001 843 edges
created, 0 skipped, 0 adjacency mismatches. Because both paths call the same
_connect_resolved_locked body, the guid lists and the pointer sets stay in
lockstep and the reverse edges are filled identically.
The dual-stage guarantee is untouched: reserve_links still runs in a separate
sweep AFTER every vertex is in the graph and BEFORE any edge exists, and no
pointer is resolved at insert time. connect_bulk is called from the same
single-threaded link pass, so the single-writer contract holds — and it now takes
ONE writer check for the batch, which is strictly weaker than the per-edge version
it replaced (same thread, same phase). Lock ordering is preserved and extended:
the batch collects its touched shards, then acquires them once each in ASCENDING
index order, and the vertex locks are still taken shard-first and one edge at a
time. The roots() cache lock (_roots_mutex) is outside both levels and is never
taken by a writer, so it cannot join a shard-level cycle; a stale recompute reads
the version BEFORE its scan, so a structural write landing mid-scan leaves the
cache stale rather than caching a half-updated answer (verified: 13 loads × the
double-roots() probe, 0 mismatches, and every stale-recompute matched a fresh
scan). Skipped-edge semantics are unchanged: missing target, self edge and
refused edge each increment skipped_edge_count exactly once, from the same
EdgeOutcome that replaced connect's bool, never fatal.
The incremental-save byte-identity property is untouched (no save-side change).
Existing tests are UNMODIFIED. Verified: debug (216 + 303 asserts, plus the
bench's 43, green), ASan debug (both suites green, no reports), release (bench and
both suites green); `git status` shows only src/graph_db/** touched.

Epoch-3 row caveat: the row is ONE whole-suite release run (the step-3
verification run of the epoch-3 session), so its cell-to-cell noise is the
machine's — especially on the 10/20/40T cells and on the mixed-write and save
cells that epoch 0/1 already flagged as the least reproducible in the table (the
save cells here are the clearest example: 786 ms and 8 576 ms in this run vs
738/751 ms and 8 276/7 950 ms in the A/B runs minutes earlier, on code that
cannot differ because epoch 3 changed nothing on the save path). The
authoritative epoch-3 verdict is the interleaved min-of-N A/B in the epoch-3
notes, not a row-to-row read; the row is here so the columns stay comparable, and
its load cells (168 ms / 3 099 ms) agree with the A/B minima (160.8 ms /
3 058 ms) to within 5 %.

STOP RULE (graph_db_plan.md §7, "stop when a full epoch yields < 5 %"). The
headline targeted path moved -29.9 % at 100k and -21.4 % at 1M, i.e. the epoch is
well above the 5 % threshold, so the rule does not fire on this epoch's own
result. What epoch 4 would attack, in measured order:

1. The SAVE, and only through src/core. 95 % of a save is one LMDB commit (774 ms
   of 813 ms at 100k in epoch 2, and still 8.0 s of 8.1 s at 1M here – epoch 3
   changed nothing on the save path). The standalone probe already put the same
   100k commit at 38 ms with MDB_NOSYNC against 712 ms today, so making the sync
   policy a parameter of kimix::LMDB plus a ColdStorage knob is a ~20x save win
   sitting behind a file this loop is forbidden to touch. Nothing else in
   src/graph_db is worth 5 % on save.
2. Load stage 1, 8-22 ms of the remaining 166 ms (5-13 %): a sharded cursor is
   blocked on kimix::LMDBIterator (one cursor per iterator, no key-range seek in
   the wrapper), so the available win is avoiding the key+value copy in
   load_all_records — again a src/core change.
3. The load's edge phase is now ~110 ms of 166 ms, spread over 6 M container
   inserts at 1M; the measured sub-costs are the two vertex UNIQUE locks per edge
   (~8 % of the phase) and the hash inserts themselves. Dropping the vertex locks
   would be a documented invariant change for <5 % of the headline, so it is
   rejected on the plan's own rule ("reject epochs that trade correctness for
   speed").
None of those is a ≥5 % headline inside src/graph_db, so per the plan's stop rule
the optimization loop is STOPPED at epoch 3: this is the final epoch. The next
real step is a src/core sync-policy decision, not another src/graph_db epoch.