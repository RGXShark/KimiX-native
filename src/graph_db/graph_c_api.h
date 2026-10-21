/*
 * graph_c_api.h -- the C ABI of kimix-graph-db (a Semevia.Gdi.Native adapter's
 * binding surface).
 *
 * This header is pure 7-bit ASCII and valid for BOTH a C99/C11 compiler and C++.
 * It uses no templates, no namespaces, no `enum class`, no references, no
 * default arguments, no `bool` (1-byte `uint8_t` instead) and no C++ types:
 * opaque handles, fixed-size POD structs, `<stdint.h>` scalars, `size_t` and
 * `const char *`. Include it as <graph_db/graph_c_api.h>; it is NOT part of the
 * C++ umbrella (graph_db.h), exactly like src/api's headers.
 *
 * CONTRACT (read before binding)
 * 1. Every entry point returns `int32_t`, one of the kimix_gdb_status codes
 *    below. The numbers are IDENTICAL to kimix::graph_db::GdiStatus and to
 *    Semevia.Gdi.GdiError (0..2 warnings, 3..62 errors), so a managed wrapper can
 *    switch on the same integers. KIMIX_GDB_OK is 0, so `if (call(...))` reads as
 *    failure.
 * 2. An out-parameter is written ONLY on KIMIX_GDB_OK (or a warning). A failed
 *    call leaves the caller's memory untouched.
 * 3. NO LIBRARY ALLOCATION ON READ PATHS. Every variable-length result uses the
 *    two-phase pattern: call once with the array pointer NULL (or the buffer
 *    length 0) and the `out_*` parameters filled with the required counts, then
 *    call again with caller-owned storage. A buffer that is too small returns
 *    KIMIX_GDB_ERR_BUFFER and still reports the required sizes. The ONLY object
 *    the library ever allocates is the session (open/close) -- there is no
 *    cross-heap free.
 * 4. Variable-length payloads (property values, names) are delivered into a
 *    caller-provided SCRATCH buffer: each record carries (offset, length) into
 *    it. That keeps one flat allocation per read instead of one per value.
 * 5. Threading: one session per engine worker, single-threaded per session (the
 *    IGraphBackend contract). Reads inside one session may not run concurrently
 *    with writes to the same session.
 * 6. Strings and byte blobs are (pointer, length) pairs of UTF-8 / binary data.
 *    Nothing is NUL-terminated or NUL-free; a length of 0 is a valid empty value.
 * 7. ABI changes bump KIMIX_GDB_ABI_VERSION and the values reported by
 *    kimix_gdb_layout_info().
 *
 * IO ERRORS: kimix::LMDB is abort-on-error by design, so KIMIX_GDB_ERR_IO is
 * only produced for RECOVERABLE preconditions this library can detect itself
 * (store not open, no store configured). A disk failure inside LMDB remains
 * fatal: the adapter must treat it as process death, not as a return code.
 */
#ifndef KIMIX_GDB_GRAPH_C_API_H
#define KIMIX_GDB_GRAPH_C_API_H

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint8_t ... uint64_t, int32_t, int64_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Versioning / linkage
 * ------------------------------------------------------------------------- */

#define KIMIX_GDB_ABI_VERSION 1u

/* Status codes: the exact values of kimix::graph_db::GdiStatus and
 * Semevia.Gdi.GdiError. Only the ones a caller of this surface can actually
 * receive are listed; the rest exist so a wrapper can name them. */
#define KIMIX_GDB_OK 0
#define KIMIX_GDB_WARNING_NON_UNIQUE_ID 1
#define KIMIX_GDB_WARNING_OTHER 2
#define KIMIX_GDB_ERR_ACCESS 3
#define KIMIX_GDB_ERR_ARGUMENT 4
#define KIMIX_GDB_ERR_BAD_FILE 5
#define KIMIX_GDB_ERR_BUFFER 6
#define KIMIX_GDB_ERR_CONSTRAINT 7
#define KIMIX_GDB_ERR_CONVERSION 8
#define KIMIX_GDB_ERR_COUNT 9
#define KIMIX_GDB_ERR_DATABASE 10
#define KIMIX_GDB_ERR_DATATYPE 11
#define KIMIX_GDB_ERR_INVALID_DATE 52
#define KIMIX_GDB_ERR_EDGE 15
#define KIMIX_GDB_ERR_EDGE_ORIENTATION 16
#define KIMIX_GDB_ERR_EMPTY_NAME 17
#define KIMIX_GDB_ERR_LABEL 23
#define KIMIX_GDB_ERR_NAME_EXISTS 24
#define KIMIX_GDB_ERR_NO_PROPERTY 26
#define KIMIX_GDB_ERR_NO_SUCH_FILE 28
#define KIMIX_GDB_ERR_NON_UNIQUE_ID 29
#define KIMIX_GDB_ERR_OP 32
#define KIMIX_GDB_ERR_OP_DATATYPE_MISMATCH 33
#define KIMIX_GDB_ERR_PROPERTY_EXISTS 34
#define KIMIX_GDB_ERR_PROPERTY_TYPE 35
#define KIMIX_GDB_ERR_PROPERTY_TYPE_EXISTS 36
#define KIMIX_GDB_ERR_READ_ONLY_PROPERTY_TYPE 39
#define KIMIX_GDB_ERR_READ_ONLY_TRANSACTION 40
#define KIMIX_GDB_ERR_SIZE 42
#define KIMIX_GDB_ERR_SIZE_LIMIT 43
#define KIMIX_GDB_ERR_STATE 45
#define KIMIX_GDB_ERR_SUBCONSTRAINT 46
#define KIMIX_GDB_ERR_TRANSACTION 48
#define KIMIX_GDB_ERR_UID 49
#define KIMIX_GDB_ERR_VERTEX 50
#define KIMIX_GDB_ERR_WRONG_TYPE 51
#define KIMIX_GDB_ERR_IO 60
#define KIMIX_GDB_ERR_OTHER 61
#define KIMIX_GDB_ERR_UNKNOWN 62

/* ---------------------------------------------------------------------------
 * POD types
 * ------------------------------------------------------------------------- */

/* A 128-bit id (a vertex or edge uid). `lo` is the first 64 bits of
 * kimix::Guid::to_binary().data0/data1, `hi` the second. */
typedef struct kimix_gdb_guid {
    uint64_t lo;
    uint64_t hi;
} kimix_gdb_guid;

/* A borrowed byte range. Never owned by the library on a read path. */
typedef struct kimix_gdb_bytes {
    const uint8_t *data;
    size_t len;
} kimix_gdb_bytes;

/* Property kinds: kimix::graph_db::PropertyKind (Semevia.Gdi ValueKind). */
#define KIMIX_GDB_KIND_NONE 0
#define KIMIX_GDB_KIND_INT64 1
#define KIMIX_GDB_KIND_DOUBLE 2
#define KIMIX_GDB_KIND_STRING 3
#define KIMIX_GDB_KIND_BLOB 4
#define KIMIX_GDB_KIND_INT64_ARRAY 5
#define KIMIX_GDB_KIND_DOUBLE_ARRAY 6

/* Property container types (GDI "etype"). */
#define KIMIX_GDB_CONTAINER_SINGLE 0
#define KIMIX_GDB_CONTAINER_ARRAY 1

/* GDI datatypes (gdi_datatype.h values). */
#define KIMIX_GDB_DTYPE_CHAR 100
#define KIMIX_GDB_DTYPE_BYTE 104
#define KIMIX_GDB_DTYPE_INT64 112
#define KIMIX_GDB_DTYPE_UINT64 113
#define KIMIX_GDB_DTYPE_DOUBLE 114

/* Comparison operators (gdi_operation.h). */
#define KIMIX_GDB_OP_EQUAL 0
#define KIMIX_GDB_OP_NOT_EQUAL 1
#define KIMIX_GDB_OP_GREATER 2
#define KIMIX_GDB_OP_GREATER_OR_EQUAL 3
#define KIMIX_GDB_OP_LESS 4
#define KIMIX_GDB_OP_LESS_OR_EQUAL 5

/* Edge direction (GDI_EDGE_DIRECTED / GDI_EDGE_UNDIRECTED). */
#define KIMIX_GDB_EDGE_DIRECTED 0
#define KIMIX_GDB_EDGE_UNDIRECTED 1

/* Read orientation (a distinct set of values from the direction above). */
#define KIMIX_GDB_ORIENTATION_OUTGOING 0
#define KIMIX_GDB_ORIENTATION_INCOMING 1
#define KIMIX_GDB_ORIENTATION_BOTH 2

/* One property value as an INPUT: the same struct is both the input and the
 * output shape. `payload` is only read for the string/blob/array kinds (for the
 * array kinds it is the little-endian element data). */
typedef struct kimix_gdb_property {
    uint8_t kind;  /* KIMIX_GDB_KIND_* */
    uint8_t dtype; /* KIMIX_GDB_DTYPE_*, informational on input */
    uint16_t count;
    int64_t i64;
    double f64;
    kimix_gdb_bytes payload;
} kimix_gdb_property;

/* One property as an OUTPUT record: the value plus where its variable-length
 * payload landed inside the caller's scratch buffer. */
typedef struct kimix_gdb_property_record {
    uint32_t name_id;
    uint8_t kind;
    uint8_t dtype;
    uint16_t count;
    int64_t i64;
    double f64;
    uint32_t payload_offset;
    uint32_t payload_len;
} kimix_gdb_property_record;

/* The (label, external_id) primary key of a vertex. */
typedef struct kimix_gdb_vertex_key {
    uint32_t label;
    kimix_gdb_bytes external_id;
} kimix_gdb_vertex_key;

/* One edge record as a read returns it (GDI's GdiEdgeRecord). */
typedef struct kimix_gdb_edge_record {
    kimix_gdb_guid uid;
    kimix_gdb_guid origin;
    kimix_gdb_guid target;
    uint8_t direction;
} kimix_gdb_edge_record;

/* A property-type declaration. `name_offset`/`name_len` point into the scratch
 * buffer of the call that returned it. */
typedef struct kimix_gdb_property_type {
    uint32_t id;
    uint8_t container;
    uint8_t dtype;
    uint16_t reserved;
    uint32_t count;
    uint32_t name_offset;
    uint32_t name_len;
} kimix_gdb_property_type;

/* A registered label. Same scratch convention as above. */
typedef struct kimix_gdb_label {
    uint32_t id;
    uint32_t name_offset;
    uint32_t name_len;
} kimix_gdb_label;

/* What the backend can do (mirrors Semevia.Gdi.Capability, same names). */
typedef struct kimix_gdb_capability {
    uint8_t supports_global_scan;
    uint8_t supports_edge_by_stable_uid;
    uint8_t supports_edge_property_read;
    uint8_t supports_native_multi_hop;
    int32_t max_native_hop_depth;
    uint8_t supports_global_aggregates;
    uint8_t supports_collective_transactions;
    uint8_t enforces_unique_external_ids;
} kimix_gdb_capability;

/* Struct sizes, reported so a binding can verify it was built against the same
 * layout it is calling. */
typedef struct kimix_gdb_layout {
    uint32_t abi_version;
    uint32_t sizeof_property;
    uint32_t sizeof_property_record;
    uint32_t sizeof_vertex_key;
    uint32_t sizeof_edge_record;
    uint32_t sizeof_property_type;
    uint32_t sizeof_capability;
    uint32_t sizeof_vertex_spec;
    uint32_t sizeof_size_t;
    uint32_t alignof_guid;
} kimix_gdb_layout;

/* An open() request. A NULL/empty `directory` opens a purely in-memory database
 * (persist() then reports KIMIX_GDB_ERR_NO_SUCH_FILE). */
typedef struct kimix_gdb_open_options {
    const char *directory; /* UTF-8, NUL-terminated (this is a path, not payload) */
    uint8_t auto_create_schema;
    uint8_t key_mode; /* 0 = enforce unique, 1 = report warning */
    uint8_t allow_parallel_edges;
    uint8_t allow_self_edges;
    uint8_t persist_on_commit;
    uint8_t reserved[3];
    size_t max_journal_bytes;
    size_t cold_map_size;
} kimix_gdb_open_options;

/* One keyed vertex to create in a bulk call. */
typedef struct kimix_gdb_vertex_spec {
    kimix_gdb_vertex_key key;
    const uint32_t *labels;
    size_t label_count;
    /* `properties[i]` is named by `property_names[i]` (a property type id). */
    const kimix_gdb_property *properties;
    const uint32_t *property_names;
    size_t property_count;
    kimix_gdb_bytes content;
} kimix_gdb_vertex_spec;

/* One edge to create in a bulk call. */
typedef struct kimix_gdb_edge_spec {
    kimix_gdb_guid origin;
    kimix_gdb_guid target;
    uint8_t direction;
    uint8_t reserved[3];
    const uint32_t *labels;
    size_t label_count;
    const kimix_gdb_property *properties;
    const uint32_t *property_names;
    size_t property_count;
} kimix_gdb_edge_spec;

/* A constraint condition (an INPUT; constraints are built incrementally). */
typedef struct kimix_gdb_constraint_spec {
    uint32_t id;      /* returned by kimix_gdb_constraint_create */
    uint32_t reserved;
    uint32_t label;   /* label condition when is_property == 0 */
    uint32_t property;/* property condition when is_property == 1 */
    uint8_t is_property;
    uint8_t op;
    uint16_t reserved2;
    kimix_gdb_property value;
    uint32_t count;
} kimix_gdb_constraint_spec;

/* Opaque session handle (one per database; see contract point 5). */
typedef struct kimix_gdb_session kimix_gdb_session;

/* ---------------------------------------------------------------------------
 * Library / ABI queries
 * ------------------------------------------------------------------------- */

uint32_t kimix_gdb_abi_version(void);
const char *kimix_gdb_version_string(void);
int32_t kimix_gdb_layout_info(kimix_gdb_layout *out);
/* Never returns NULL. Unknown codes answer "status_unknown". */
const char *kimix_gdb_status_name(int32_t status);

/* ---------------------------------------------------------------------------
 * Session lifecycle
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_open(const kimix_gdb_open_options *options, kimix_gdb_session **out);
void kimix_gdb_close(kimix_gdb_session *session);
int32_t kimix_gdb_capabilities(kimix_gdb_session *session, kimix_gdb_capability *out);
/* full != 0 rewrites every record, 0 writes only the dirty ones. */
int32_t kimix_gdb_persist(kimix_gdb_session *session, uint8_t full);
/* Re-read the store into the open session. */
int32_t kimix_gdb_load(kimix_gdb_session *session);

/* ---------------------------------------------------------------------------
 * Vertices
 * ------------------------------------------------------------------------- */

/* out_uid and out_warning (0/1) are written on OK and on the warning. */
int32_t kimix_gdb_vertex_create(kimix_gdb_session *session,
                                const kimix_gdb_vertex_spec *spec,
                                kimix_gdb_guid *out_uid, uint8_t *out_warning);
/* MERGE: out_created is 1 when a vertex was made, 0 when an existing one matched. */
int32_t kimix_gdb_vertex_get_or_create(kimix_gdb_session *session,
                                       const kimix_gdb_vertex_spec *spec,
                                       kimix_gdb_guid *out_uid, uint8_t *out_created);
int32_t kimix_gdb_vertex_translate(kimix_gdb_session *session,
                                   const kimix_gdb_vertex_key *key,
                                   kimix_gdb_guid *out_uid, uint8_t *out_found);
int32_t kimix_gdb_vertex_free(kimix_gdb_session *session, kimix_gdb_guid vertex);
int32_t kimix_gdb_vertex_free_guarded(kimix_gdb_session *session, kimix_gdb_guid vertex);

/* ---------------------------------------------------------------------------
 * Labels on vertices and edges
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_vertex_label_add(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                   uint32_t label);
int32_t kimix_gdb_vertex_label_remove(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t label);
/* Two-phase: labels == NULL reports *out_count. The FIRST label is the vertex's
 * key label (GDI's GetLabelsOfVertex order). */
int32_t kimix_gdb_vertex_labels(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                uint32_t *labels, size_t capacity, size_t *out_count);

int32_t kimix_gdb_edge_label_add(kimix_gdb_session *session, kimix_gdb_guid edge,
                                 uint32_t label);
int32_t kimix_gdb_edge_label_remove(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t label);
int32_t kimix_gdb_edge_labels(kimix_gdb_session *session, kimix_gdb_guid edge,
                              uint32_t *labels, size_t capacity, size_t *out_count);

/* ---------------------------------------------------------------------------
 * Properties on vertices and edges
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_vertex_property_add(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_vertex_property_set(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_vertex_property_update(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                         uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_vertex_properties_remove(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                           const uint32_t *names, size_t name_count);
/* Two-phase read: items/scratch == NULL reports *out_count and *out_scratch_bytes. */
int32_t kimix_gdb_vertex_properties(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                    kimix_gdb_property_record *items, size_t capacity,
                                    uint8_t *scratch, size_t scratch_capacity,
                                    size_t *out_count, size_t *out_scratch_bytes);
int32_t kimix_gdb_vertex_property_types(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                        kimix_gdb_property_type *items, size_t capacity,
                                        uint8_t *scratch, size_t scratch_capacity,
                                        size_t *out_count, size_t *out_scratch_bytes);

int32_t kimix_gdb_edge_property_add(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_edge_property_set(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_edge_property_update(kimix_gdb_session *session, kimix_gdb_guid edge,
                                       uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_edge_properties_remove(kimix_gdb_session *session, kimix_gdb_guid edge,
                                         const uint32_t *names, size_t name_count);
int32_t kimix_gdb_edge_properties(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_property_record *items, size_t capacity,
                                  uint8_t *scratch, size_t scratch_capacity,
                                  size_t *out_count, size_t *out_scratch_bytes);
int32_t kimix_gdb_edge_property_types(kimix_gdb_session *session, kimix_gdb_guid edge,
                                      kimix_gdb_property_type *items, size_t capacity,
                                      uint8_t *scratch, size_t scratch_capacity,
                                      size_t *out_count, size_t *out_scratch_bytes);

/* ---------------------------------------------------------------------------
 * Edges
 * ------------------------------------------------------------------------- */

/* Initial `properties` are NOT stored by this entry point: a stored value needs
 * its property-type NAME, and a bare kimix_gdb_property has none to give (this
 * is the documented §8.8 degrade, same shape as GDI-RMA).  To create an edge
 * carrying properties in one call, use kimix_gdb_edges_create_bulk with a
 * one-row spec (kimix_gdb_edge_spec carries property_names), or create the
 * edge here and kimix_gdb_edge_property_add each value.  The properties /
 * property_count arguments are accepted for signature symmetry and ignored. */
int32_t kimix_gdb_edge_create(kimix_gdb_session *session, kimix_gdb_guid origin,
                              kimix_gdb_guid target, uint8_t direction,
                              const uint32_t *labels, size_t label_count,
                              const kimix_gdb_property *properties, size_t property_count,
                              kimix_gdb_guid *out_edge);
int32_t kimix_gdb_edge_free(kimix_gdb_session *session, kimix_gdb_guid edge);
int32_t kimix_gdb_edge_set_origin(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_guid origin);
int32_t kimix_gdb_edge_set_target(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_guid target);
int32_t kimix_gdb_edge_set_direction(kimix_gdb_session *session, kimix_gdb_guid edge,
                                     uint8_t direction);
int32_t kimix_gdb_edge_endpoints(kimix_gdb_session *session, kimix_gdb_guid edge,
                                 kimix_gdb_guid *out_origin, kimix_gdb_guid *out_target);

/* ---------------------------------------------------------------------------
 * Filtered, oriented, paged reads
 * ------------------------------------------------------------------------- */

/* `constraint` is a constraint id from kimix_gdb_constraint_create (0 = none). */
int32_t kimix_gdb_edges_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, int64_t cap,
                                  kimix_gdb_edge_record *items, size_t capacity,
                                  size_t *out_count, size_t *out_total);
int32_t kimix_gdb_neighbors_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t constraint, uint8_t orientation, int64_t cap,
                                      kimix_gdb_guid *items, size_t capacity,
                                      size_t *out_count, size_t *out_total);
int32_t kimix_gdb_count_neighbors(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, size_t *out_count);
int32_t kimix_gdb_exists_neighbor(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, uint8_t *out_exists);

/* ---------------------------------------------------------------------------
 * Transactions (one open transaction per session)
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_tx_begin(kimix_gdb_session *session);
int32_t kimix_gdb_tx_commit(kimix_gdb_session *session);
int32_t kimix_gdb_tx_abort(kimix_gdb_session *session);
int32_t kimix_gdb_tx_begin_collective(kimix_gdb_session *session);
int32_t kimix_gdb_tx_close_collective(kimix_gdb_session *session, uint8_t commit);

/* ---------------------------------------------------------------------------
 * Schema: labels and property types
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_label_create(kimix_gdb_session *session, const char *name, size_t name_len,
                               uint32_t *out_label);
int32_t kimix_gdb_label_find(kimix_gdb_session *session, const char *name, size_t name_len,
                             uint32_t *out_label, uint8_t *out_found);
/* Two-phase, in id order. */
int32_t kimix_gdb_label_get_all(kimix_gdb_session *session, kimix_gdb_label *items,
                                size_t capacity, uint8_t *scratch, size_t scratch_capacity,
                                size_t *out_count, size_t *out_scratch_bytes);
int32_t kimix_gdb_label_update(kimix_gdb_session *session, uint32_t label, const char *name,
                               size_t name_len);
int32_t kimix_gdb_label_free(kimix_gdb_session *session, uint32_t label);
/* The name of one label into a caller buffer (two-phase via out_required). */
int32_t kimix_gdb_label_name(kimix_gdb_session *session, uint32_t label, char *out,
                             size_t capacity, size_t *out_required);

int32_t kimix_gdb_property_type_create(kimix_gdb_session *session, const char *name,
                                       size_t name_len, uint8_t container, uint8_t dtype,
                                       uint32_t count, uint32_t *out_type);
int32_t kimix_gdb_property_type_find(kimix_gdb_session *session, const char *name,
                                     size_t name_len, uint32_t *out_type, uint8_t *out_found);
int32_t kimix_gdb_property_type_get_all(kimix_gdb_session *session,
                                        kimix_gdb_property_type *items, size_t capacity,
                                        uint8_t *scratch, size_t scratch_capacity,
                                        size_t *out_count, size_t *out_scratch_bytes);
int32_t kimix_gdb_property_type_update(kimix_gdb_session *session, uint32_t type,
                                       const char *name, size_t name_len, uint8_t container,
                                       uint8_t dtype, uint32_t count);
int32_t kimix_gdb_property_type_free(kimix_gdb_session *session, uint32_t type);

/* ---------------------------------------------------------------------------
 * Constraints
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_constraint_create(kimix_gdb_session *session, uint32_t *out_constraint);
int32_t kimix_gdb_constraint_add_label(kimix_gdb_session *session, uint32_t constraint,
                                       uint32_t label, uint8_t op);
int32_t kimix_gdb_constraint_add_property(kimix_gdb_session *session, uint32_t constraint,
                                          uint32_t property, uint8_t op,
                                          const kimix_gdb_property *value, uint32_t count);
int32_t kimix_gdb_constraint_free(kimix_gdb_session *session, uint32_t constraint);

/* ---------------------------------------------------------------------------
 * Bulk
 * ------------------------------------------------------------------------- */

/* N vertex creates in ONE transaction (continue_on_error != 0 isolates the rows,
 * 0 makes the batch all-or-nothing). `out_uids`, `out_status` and `out_warning`
 * are caller arrays of `count` entries; any of them may be NULL. */
int32_t kimix_gdb_vertices_create_bulk(kimix_gdb_session *session,
                                       const kimix_gdb_vertex_spec *specs, size_t count,
                                       uint8_t continue_on_error, kimix_gdb_guid *out_uids,
                                       int32_t *out_status, uint8_t *out_warning);
/* The same shape for edges. */
int32_t kimix_gdb_edges_create_bulk(kimix_gdb_session *session,
                                    const kimix_gdb_edge_spec *specs, size_t count,
                                    uint8_t continue_on_error, kimix_gdb_guid *out_uids,
                                    int32_t *out_status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KIMIX_GDB_GRAPH_C_API_H */
