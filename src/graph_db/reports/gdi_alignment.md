# GDI alignment report — `src/graph_db` vs. `Semevia.GDI`

Source requirement: `C:\dev\semevia-next\graph_report.md` (Gap Report: kimix-native/graph_db
vs. the Semevia.GDI command interface).
Reference semantics: `C:\dev\semevia-next\src\backend\database\gdi\` —
`IGraphBackend.cs`, `Backends/BackendSchema.cs`, `Backends/Capability.cs`,
`Backends/InMemory/{InMemoryGdiBackend,InMemorySchema,InMemoryStore}.cs`,
`Schema/GdiError.cs`, `Model/*.cs`.

Every semantic decision below is **copied from that reference implementation**, not
invented; where this module deliberately does something the reference cannot (it has
no key-label concept, and it persists), the deviation is named and justified.

Status: **all of §2.1 – §2.11 and §2.13 – §2.16 are closed**; §2.12 is the deferred
P9 bucket and the capability flags for it stay honestly `false`.

---

## §2.1 Typed property model — CLOSED

- `property.h` / `property.cpp`: `PropertyKind` (the same 7 kinds as
  `PropertyValue.ValueKind`), `PropertyValue` (scalars inline, string/blob/array
  payloads in native containers), `PropertyBag` (interned name id -> value),
  `GdiDataType`, `GdiContainerType`, `GdiOp`, `evaluate()` (numeric cross-kind
  ordering, ordinal strings, equality-only for blob/arrays) and the record-level
  property codec.
- `Vertex::{add,set,put,remove,get}_property` / `property_list()`; the edge
  equivalent lives on `Graph`/`EdgeTable`; `Database` adds the schema validation and
  the exact GDI status codes.
- The legacy opaque content blob is kept as its own field, so the lower-level
  consumers and their tests are untouched.
- Tests: `tests/unit/graph_db/test_graph_db_property.cpp` (kind matrix, error
  matrix, determinism, save/load, concurrency, coexistence with `content()`).
- Error semantics (from `InMemoryGdiBackend.AddPropertyAsync/Set/Update/Remove`):
  Add -> `error_property_exists`, Set -> `error_no_property`, Update = upsert,
  Remove ignores an absent name. This module additionally validates the value
  against the property-type DECLARATION (`error_wrong_type` / `error_count`), which
  the reference cannot do because its declarations carry no dtype check — an
  honest superset, documented in `database.h`.

## §2.2 Labels — CLOSED

- `Vertex::add_label/remove_label/labels`, `Graph::edge_add_label/edge_remove_label`,
  the label registry in `schema.h`, and the `Database` commands (add / remove /
  get, on vertices and edges) plus `create_label` / `get_label_by_name` /
  `get_all_labels` / `update_label` / `free_label`.
- `GetLabelsOfVertex` returns the **key label first**, then the entity's labels
  (the reference's order). The key label is NOT part of the explicit label set, so
  constraint matching behaves exactly like `InMemoryStore.Matches`.
- Rename is O(1) on entities (they store ids); `FreeLabel` cascades to every vertex
  and edge. Freeing a label a live vertex uses as its KEY is refused with
  `error_state` — documented deviation: the reference has no key-label concept, and
  freeing it here would orphan a `KeyIndex` entry.
- Tests: `test_graph_db_property.cpp` (label matrix), `test_graph_db_schema.cpp`
  (lifecycle, cascade, key-label refusal).

## §2.3 Vertex primary key `(label, external_id)` — CLOSED

- `key.h`: `ExternalId` (arbitrary binary, 16-byte SSO so a normal key allocates
  nothing), `VertexKey` / `VertexKeyView`, the sharded `KeyIndex`.
- `Graph::create_vertex` (keyed), `get_or_create_vertex` (MERGE: a match returns the
  uid UNTOUCHED), `translate_vertex_id` (`ok` + `found == false` for a miss, never
  an error), `KeyMode::enforce_unique` (default, `error_non_unique_id`) and
  `KeyMode::report_warning` (`warning_non_unique_id` + the existing uid — GDI's
  `SimulateNonUniqueId` path).
- `EnforcesUniqueExternalIds = true` in `capability.h`.
- Tests: `test_graph_db_key.cpp` (translate, uniqueness, MERGE, binary ids at every
  SSO boundary, label isolation, 10k-key shard spread, persistence, churn).

## §2.4 Edge identity and payload — CLOSED

- `edge.h`: `EdgeId` (= `kimix::Guid`, stable across save/load), `Edge` (origin,
  target, direction, labels, properties), `EdgeLink`, `EdgeRecord`, the sharded
  `EdgeTable`, and the `"GDE2"` codec.
- `Graph::edge_create / edge_free / edge_set_origin / edge_set_target /
  edge_set_direction`, plus the payload accessors; `Database` mirrors them with the
  GDI status codes. `Database::get_edge_properties` / `get_edge_property_types`
  therefore answer, so `SupportsEdgePropertyRead = true`, and
  `SupportsEdgeByStableUid = true`.
- Tests: `test_graph_db_edge.cpp` (identity, parallel edges, direction/orientation
  matrix, endpoint mutation, self-loop option, cascade, persistence with an
  incremental edge write, the unchanged legacy contract).

## §2.5 Schema: property types & constraints — CLOSED

- `schema.h`: `LabelRegistry` + `PropertyTypeRegistry` (name <-> dense id, ids never
  recycled), `Constraint` / `ConstraintCondition` / `ConstraintHandle` and the
  registry; `Database` exposes the 10 GDI schema commands plus the three constraint
  commands.
- `FreePropertyType` removes every value of that type from every vertex AND edge;
  `UpdatePropertyType` refuses a shape change while a value exists
  (`error_read_only_property_type`), matching the reference's intent.
- Persistence: `"s/labels"` / `"s/proptypes"` records, restored before the entities
  that name them; the id counters ride in the meta record so a reload continues the
  id space instead of reusing dead ids.
- Tests: `test_graph_db_schema.cpp` (lifecycle, cascades, persistence, auto-create
  vs. strict, 10k ids dense and never recycled).

## §2.6 Constraint-filtered, oriented, paged reads — CLOSED

- `query.h`: `Page<T>` with `items` + `total_count`, `matches()` for the AND-ed
  conditions, `make_page()` (cap 0 = count probe, cap N = at most N items + the full
  total).
- `Database::get_edges_of_vertex` (filters the EDGE), `get_neighbor_vertices`
  (filters the NEIGHBOUR VERTEX, deduplicated — GDI §5.10), `count_neighbors`
  (exact) and `exists_neighbor` (cap-1 short circuit).
- Incidence is by ORIGIN/TARGET, exactly like the reference's `IsIncident`: the
  direction flag is metadata a reader reports, it does not move an edge between
  lists. (The plan's prose said "undirected counts for both"; the reference code
  does not, and the reference wins — recorded here so the decision is auditable.)
- Tests: `test_graph_db_read.cpp` (incidence matrix, constraint matrix over every
  kind and op, paging matrix, edge-vs-neighbour filtering, determinism, a
  4-reader/1-writer stress run).

## §2.7 Transactions — CLOSED (with a documented durability model)

- `transaction.h`: `Transaction`, `JournalEntry` (an UNDO log, not a snapshot) and
  the journal cap (`GraphOptions::max_journal_bytes`, default 64 MiB ->
  `error_size_limit` with the transaction still abortable).
- `Database::begin_transaction / commit / abort / begin_collective /
  close_collective`: read-your-writes inside a write transaction; `abort()` replays
  the journal in reverse and restores the graph bit-identically (verified by a
  fingerprint comparison in `test_graph_db_tx.cpp`); a read-only (collective) scope
  returns `error_read_only_transaction` for every write; one write transaction at a
  time (`error_transaction`); a `Transaction` destroyed while open aborts.
- **Durability** is the module's save model, not a per-commit fsync: `commit()`
  persists only when `GraphOptions::persist_on_commit` is set. Documented in
  `transaction.h` and `database.h`.
- `supports_collective_transactions = true`, with the caveat that there is NO
  snapshot isolation (single-writer store): a collective scope is consistent iff no
  writer runs concurrently — the same contract the reference advertises.
- Tests: `test_graph_db_tx.cpp` (abort restore, commit persist, read-only
  enforcement, state machine, journal cap, auto-transaction, structural rollback,
  commit-then-save).

## §2.8 Bulk vertex creation — CLOSED

- `Database::create_vertices_bulk` (one transaction, row-ordered results,
  `continue_on_error` = per-row isolation vs. all-or-nothing) and
  `create_edges_bulk`, both also exposed through the C ABI.
- Tests: `test_graph_db_bulk.cpp` (10k rows in one transaction, row order, both
  duplicate policies, a refused edge row).

## §2.9 MERGE / get-or-create — CLOSED

`Database::get_or_create_vertex` returns `{uid, created}`; a match never mutates the
vertex (the reference's exact behaviour). Test: `test_graph_db_key.cpp`.

**Persistence of a delete**: a save is a write transaction, so a deleted vertex or
edge is written as an EMPTY value under its key (a tombstone) in the same
transaction as the records; the loader recognises it and never resurrects the
entity. `test_graph_db_edge.cpp`'s
`edge_and_vertex_deletion_survive_a_reload` is the guard.

## §2.10 Guarded vs. cascading delete — CLOSED

`Database::free_vertex` cascades (DETACH DELETE: every incident edge goes with it,
in either direction) and `free_vertex_guarded` refuses with `error_edge` and removes
nothing. Tests: `test_graph_db_edge.cpp` (cascade) and `test_graph_db_bulk.cpp`
(guarded, for outgoing / incoming / undirected edges).

## §2.11 Error model — CLOSED

- `status.h`: `GdiStatus` with the 63 `GdiError` values PINNED (a unit test asserts
  every one), `status_name`, `status_message` (the `GdiErrorTable` text),
  `is_warning` / `is_error` / `is_success`.
- Every `Database` method returns `GdiStatus`; the C ABI returns the same integers.
- **Honest limitation**, documented in `status.h`, this report and `graph_c_api.h`:
  `kimix::LMDB` is abort-on-error by design, so `error_io` is only produced for
  recoverable preconditions (store not open, empty path). A disk failure inside
  LMDB remains fatal — the adapter must treat it as process death.
- Tests: `test_graph_db_status.cpp` (all 63 values + names + messages + severity
  boundaries) and the `error_surface_sweep` case in `test_graph_db_bulk.cpp`
  (~40 failing calls, each with its exact code).

## §2.12 Query engine & advanced reads — DEFERRED (P9), advertised honestly

Global scan, native multi-hop, GDI-shaped BFS/shortest-path composites and global
aggregates are NOT implemented: the frontend validator rejects those commands today,
so shipping them would be untested dead surface. `capability.h` reports
`supports_global_scan = false`, `supports_native_multi_hop = false`
(`max_native_hop_depth = 0`) and `supports_global_aggregates = false`, and
`test_graph_db_c_api.cpp` asserts exactly that, so the gap cannot regress into a
silent `true`.

## §2.13 FFI / interop surface — CLOSED

- `graph_c_api.h` (pure 7-bit ASCII, valid C99 and C++) + `graph_c_api.cpp`:
  `KIMIX_GDB_ABI_VERSION`, `kimix_gdb_abi_version/version_string/layout_info/
  status_name`, an opaque `kimix_gdb_session`, and one function per GDI command.
- Contract: every call returns an `int32_t` status with the SAME numbers as
  `GdiError`; out-params are written only on success; **no library allocation on
  read paths** (two-phase size query then fetch, `KIMIX_GDB_ERR_BUFFER` when the
  caller's buffer is too small, and one caller-owned scratch buffer for the
  variable-length payloads). The session is the only owned object.
- The header is verified to compile as **C99** (`clang -std=c99 -Wall -Wextra
  -Wpedantic -fsyntax-only`), and `tests/unit/graph_db/test_graph_db_c_api.cpp`
  includes ONLY that header.
- Follow-up (not this plan): a `src/api/ffi_graph_db.*` wrapper + `docs/ffi.md`
  section + a `KIMIX_API_ABI_VERSION` bump, if the adapter should load
  `kimix_api.dll` instead of `kimix-graph-db`.

## §2.14 Direction & multiplicity semantics — CLOSED

Per-edge `EdgeDirectionType` (mutable), parallel edges between the same ordered pair
and (option-gated) self loops on `create_edge`, while the legacy `connect()` keeps
its strict one-edge-per-ordered-pair, no-self-loop contract. Tests:
`test_graph_db_edge.cpp` (parallel edges, direction toggling, self loops).

## §2.15 Store metadata / versioning — CLOSED

`"meta/root"` (`"GDM2"`) is written by every full save and by an incremental save
  `"meta/root"` (`"GDM2"`) is written by every full save and by an incremental save
  that writes anything: format version, store uuid, both save timestamps, schema
  version, the two id counters and the graph name. `KIMIX_GRAPH_DB_FORMAT_VERSION`
  is exported by `graph_db.h`; a store announcing a NEWER format is refused with
  `error_bad_file` before anything is materialized, and a store WITHOUT the record is
  a v1 store (loaded and migrated). Test: `test_graph_db_migrate.cpp`.
  **Content digest policy (plan v3 §4.5, ABI 6):** the vertex record's 16-byte
  content hash is MD5 by default or XXH3-128 behind
  `GraphOptions::content_digest` / `open_options.content_digest`. The choice is a
  session option, NOT a format version: both algorithms fill the same field, the
  loader recomputes the cached hash at materialization (so a store written under
  one policy reads cleanly under the other, loaded vertices stay clean, and a
  dirty vertex is saved under the graph's own policy — the post-write clear-dirty
  comparison keeps matching). The meta record does not record the policy; the
  hash is a change detector, never a signature. Equivalence test:
  `test_graph_db_serialize.cpp` (`serialize_content_digest_policy_round_trip`).
  ## §2.16 Multi-graph / session object — CLOSED (single graph; the catalog seam exists)

`Database` is the session object the report asks for: it owns the graph, the
schema, the cold store and the options, and it is what the C ABI wraps. A named
multi-graph catalog slots in at `Database` (the options already carry
`graph_name`, and the meta record persists it) without an ABI change. The C ABI
documents "one session per engine worker", matching `IGraphBackend`'s
not-thread-safe contract.

---

## Test map

| Suite | Covers |
|---|---|
| `test_graph_db` (unchanged) | the container contract: CRUD, cascade, BFS, writer contract, codec, cold storage |
| `test_graph_db_serialize` | save/load round trip, incremental exactness, corruption, determinism (one expectation updated for the edge-record model, see the README's epoch-4 note) |
| `test_graph_db_bench` | read / write / serialize numbers (no timing asserts) |
| `test_graph_db_status` | all 63 status codes, names, messages, severity, capability advertisement |
| `test_graph_db_migrate` | meta record, key-space partitioning, v1 -> v2 migration, newer-format refusal, `error_io` |
| `test_graph_db_property` | property kinds, error matrix, labels, determinism, persistence, concurrency |
| `test_graph_db_key` | translate, uniqueness, MERGE, binary ids, isolation, persistence, churn |
| `test_graph_db_edge` | identity, parallel edges, direction/orientation, endpoint mutation, self loops, cascade, persistence, legacy contract |
| `test_graph_db_schema` | label/property-type lifecycle, renames, free cascades, persistence, auto-create, 10k ids |
| `test_graph_db_read` | incidence matrix, constraint matrix, paging, edge-vs-neighbour filtering, determinism, stress |
| `test_graph_db_tx` | abort restore (fingerprint), commit persist, read-only scope, state machine, journal cap, auto-transactions |
| `test_graph_db_bulk` | bulk create (10k, one transaction), bulk edges, guarded delete, the error-surface sweep |
| `test_graph_db_c_api` | ABI/layout, status names, session lifecycle, full command walk through the two-phase protocol, persistence, churn |

## Known deviations from the plan's prose (each deliberate, each documented above)

1. **Undirected incidence** follows the reference `IsIncident` (origin/target only),
   not the plan's "counts for both" sentence.
2. **`error_wrong_type` / `error_count` on property writes** are a superset: the
   reference accepts any value for a declared type; this module validates it,
   because the declaration exists precisely to be validated.
3. **`FreeLabel` of a live key label is refused** (`error_state`); the reference
   has no key-label concept.
4. **`BadFile`/`error_io`** only cover recoverable preconditions (LMDB is
   abort-on-error by design).
5. **The schema table codecs live in `schema.cpp`**, not `utils.cpp` (they speak
   `PropertyTypeInfo`, which is `property.h`'s vocabulary; `utils.h` stays free of
   the schema types).
