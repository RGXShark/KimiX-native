/*
 * status.h — GdiStatus: the typed error/status surface of kimix-graph-db.
 *
 * Every fallible entry point of the GDI-shaped API (Database, the schema, the
 * C ABI) returns one of these codes instead of a bool, because the GDI command
 * layer switches on the exact integers (Semevia.Gdi.GdiError, GDI-RMA
 * src/gdi.h). The numeric values below are therefore PINNED: 0 = success,
 * 1..2 = warnings, 3..62 = errors, and a unit test asserts every one of them
 * against this list. Never renumber a code; add a new one at the next free
 * value instead (see kLastStatusValue).
 *
 * RELATIONSHIP TO bool
 * The pre-existing container API (Graph, ColdStorage, GraphSerializer) keeps
 * its bool returns: a bool is exactly "status == GdiStatus::ok" and the plan
 * says the old API stays a thin wrapper over the new one, so nothing that
 * currently compiles changes shape. New code returns GdiStatus.
 *
 * HONEST LIMITATION — io errors
 * kimix::LMDB is abort-on-error by design (a failing mdb_* call is reported to
 * stderr and the process aborts, see <core/lmdb.h>). error_io is therefore only
 * produced for RECOVERABLE preconditions this library can detect itself (store
 * not open, empty path, refused format). A disk failure inside LMDB stays
 * fatal: the GDI adapter above this ABI must treat it as process death, never
 * as a return code it can recover from. Documented in README.md and in
 * graph_c_api.h as well, because it is a property of the contract, not of one
 * function.
 *
 * Example:
 *   GdiStatus status = database.create_vertex(key, labels, props, result);
 *   if (is_error(status)) { log(status_name(status), status_message(status)); }
 *   if (is_warning(status)) { ... }   // e.g. warning_non_unique_id
 */
#pragma once

#include "graph_db_api.h"

#include <core/stl/string.h>

#include <cstdint>

namespace kimix::graph_db {

/*
 * GDI status code. Mirrors Semevia.Gdi.GdiError / GDI-RMA src/gdi.h verbatim;
 * the enumerator names are the snake_case spellings of the C# members so a
 * reader of either side can match them one to one.
 */
enum class GdiStatus : int32_t {
    // --- warnings (0..2) ---------------------------------------------------
    ok = 0,
    warning_non_unique_id = 1,
    warning_other = 2,

    // --- errors (3..62) ----------------------------------------------------
    error_access = 3,
    error_argument = 4,
    error_bad_file = 5,
    error_buffer = 6,
    error_constraint = 7,
    error_conversion = 8,
    error_count = 9,
    error_database = 10,
    error_datatype = 11,
    error_date = 12,
    error_datetime = 13,
    error_decimal = 14,
    error_edge = 15,
    error_edge_orientation = 16,
    error_empty_name = 17,
    error_error_code = 18,
    error_file_exists = 19,
    error_file_in_use = 20,
    error_incompatible_transactions = 21,
    error_index = 22,
    error_label = 23,
    error_name_exists = 24,
    error_no_memory = 25,
    error_no_property = 26,
    error_no_space = 27,
    error_no_such_file = 28,
    error_non_unique_id = 29,
    error_not_same = 30,
    error_object_mismatch = 31,
    error_op = 32,
    error_op_datatype_mismatch = 33,
    error_property_exists = 34,
    error_property_type = 35,
    error_property_type_exists = 36,
    error_range = 37,
    error_read_only_file = 38,
    error_read_only_property_type = 39,
    error_read_only_transaction = 40,
    error_resource = 41,
    error_size = 42,
    error_size_limit = 43,
    error_stale = 44,
    error_state = 45,
    error_subconstraint = 46,
    error_time = 47,
    error_transaction = 48,
    error_uid = 49,
    error_vertex = 50,
    error_wrong_type = 51,
    error_invalid_date = 52,
    error_communicator = 53,
    error_block_size = 54,
    error_quota = 55,
    error_truncate = 56,
    error_transaction_critical = 57,
    error_transaction_commit_fail = 58,
    error_intern = 59,
    error_io = 60,
    error_other = 61,
    error_unknown = 62,
};

// Highest defined status value; the code-to-name tables are sized from it.
inline constexpr int32_t kLastStatusValue = 62;

// Short, stable identifier of a status code ("ok", "error_vertex", ...).
// Returns "status_unknown" for a value outside the table, never nullptr, so a
// logger can always print it.
[[nodiscard]] KIMIX_GRAPH_DB_API char const *status_name(GdiStatus status) noexcept;

// Human-readable message, the mirror of Semevia.Gdi.GdiErrorTable.GetMessage().
// Non-empty for every enumerator (asserted by the suite).
[[nodiscard]] KIMIX_GRAPH_DB_API kimix::string status_message(GdiStatus status);

// True for 1..2 (a success with a caveat — e.g. the non-unique-id warning).
[[nodiscard]] constexpr bool is_warning(GdiStatus status) noexcept {
    return static_cast<int32_t>(status) > 0 && static_cast<int32_t>(status) <= 2;
}
// True for >= 3. `ok` is neither a warning nor an error.
[[nodiscard]] constexpr bool is_error(GdiStatus status) noexcept {
    return static_cast<int32_t>(status) > 2;
}
// ok or a warning: the call did what it was asked to do.
[[nodiscard]] constexpr bool is_success(GdiStatus status) noexcept {
    return static_cast<int32_t>(status) <= 2;
}

} // namespace kimix::graph_db
