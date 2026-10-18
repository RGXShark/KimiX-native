/*
 * ffi_sqlite.h -- the SQLite3 database C API, re-exported to C callers over the
 * vendored amalgamation (src/ext/sqlite_amalgamation/sqlite3.{c,h}, built as the
 * kimix-sqlite3 static target with FTS5 and SQLITE_THREADSAFE=1).
 *
 * SCOPE
 * -----
 * The core relational surface a foreign caller needs: open/close a database,
 * run one-shot SQL, prepare/step/finalize statements with parameter binding and
 * typed column access, plus the error-reporting and change-counting helpers.
 * The handles (`kimix_sqlite_db *`, `kimix_sqlite_stmt *`) are the library's
 * own `sqlite3` / `sqlite3_stmt` objects behind opaque typedefs -- this area is
 * a THIN wrapper, it adds no bookkeeping of its own:
 *
 *   - every function validates its arguments and maps a SQLite failure to a
 *     kimix_status (or the neutral value), so no return is ever undefined
 *     behaviour on a NULL handle;
 *   - everything else (result codes, type codes, open flags, column values)
 *     passes through EXACTLY as SQLite produces it.
 *
 * WHAT IS NOT HERE
 * ----------------
 * No callback-based sqlite3_exec (a C binding would have to marshal a
 * function pointer per row), no sqlite3_config / sqlite3_db_config knobs, no
 * collation / function registration, no backup / blob / vtab handles, no
 * progress handlers.  Callers that need rows walk them with
 * kimix_sqlite_step() + the kimix_sqlite_column_* family, which covers every
 * shape the callback API could deliver.  The FTS5 module the amalgamation is
 * compiled with (SQLITE_ENABLE_FTS5) is reachable through plain SQL:
 *     kimix_sqlite_exec(db, "CREATE VIRTUAL TABLE t USING fts5(x)", NULL);
 *
 * RESULT CODES
 * ------------
 * kimix_sqlite_step() returns the raw SQLite result code as an int so that
 * EXTENDED result codes pass through verbatim; kimix_sqlite_rc names the
 * values a caller tests against (KIMIX_SQLITE_ROW / KIMIX_SQLITE_DONE for the
 * step loop, KIMIX_SQLITE_OK for everything that is not a step).  The numeric
 * values are SQLite's own published constants and are frozen across SQLite
 * releases, so a binding may also hard-code them.
 *
 * ONE HEAP
 * --------
 * The ONLY owned buffer this area ever hands to the caller is the error
 * message of kimix_sqlite_exec(): a NUL-terminated UTF-8 string allocated on
 * this library's mimalloc heap (the wrapper copies the SQLite text so the
 * contract holds even if SQLite's allocator changes).  Release it with
 * kimix_mem_free() (api/ffi_mem.h) -- never with the caller's own free() and
 * never with sqlite3_free().  Every other pointer this area returns
 * (errmsg(), libversion(), column_text(), column_blob(), column_name()) is
 * BORROWED: valid until the owning call documented in each entry point.
 *
 * THREADING
 * ---------
 * The library is compiled with SQLITE_THREADSAFE=1 and every connection is
 * opened with SQLITE_OPEN_FULLMUTEX, so a handle may be used from several
 * threads (serialized, like the sqlite3 serialized threading mode).  The
 * usual SQLite rule still applies: a statement belongs to one connection, and
 * one statement must not be stepped from two threads at the same instant.
 *
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h
 * for the rules shared by the whole surface.  The index is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_SQLITE_H
#define KIMIX_API_FFI_SQLITE_H

#include <api/ffi_common.h>

/* The database connection and the prepared statement.  Incomplete on purpose:
 * the library's sqlite3 / sqlite3_stmt objects sit behind them, and a caller
 * only ever holds the pointer (the one place the tag is completed is the
 * translation unit that includes <sqlite3.h>, api/ffi_sqlite.cpp).  The tag
 * name is the real `sqlite3` so the typedef stays link-compatible with any
 * other copy of the SQLite headers a caller may already use. */
typedef struct sqlite3 kimix_sqlite_db;
typedef struct sqlite3_stmt kimix_sqlite_stmt;

KIMIX_FFI_BEGIN

/* ---------------------------------------------------------------------------
 * Result / type / flag constants (SQLite's own values, frozen)
 * ------------------------------------------------------------------------- */

/* The SQLite result codes this surface produces or that a caller tests
 * against.  kimix_sqlite_step() and the error accessors return `int`, NOT this
 * enum, so extended codes (e.g. SQLITE_CONSTRAINT_UNIQUE == 2067) pass
 * through untouched -- compare with the same numeric operator against both. */
typedef enum kimix_sqlite_rc {
    KIMIX_SQLITE_OK = 0,         /* success (every call that is not a step) */
    KIMIX_SQLITE_ERROR = 1,      /* generic SQL error / missing database */
    KIMIX_SQLITE_BUSY = 5,       /* database file is locked by another connection */
    KIMIX_SQLITE_CONSTRAINT = 19,/* a NOT NULL / UNIQUE / CHECK / FK failed */
    KIMIX_SQLITE_MISUSE = 21,    /* the API was used in an invalid way */
    KIMIX_SQLITE_ROW = 100,      /* sqlite3_step(): a row is ready to read */
    KIMIX_SQLITE_DONE = 101      /* sqlite3_step(): the statement finished */
} kimix_sqlite_rc;

/* The storage-class code kimix_sqlite_column_type() returns.  Same freeze as
 * the result codes: these are SQLite's constants. */
typedef enum kimix_sqlite_type {
    KIMIX_SQLITE_INTEGER = 1,
    KIMIX_SQLITE_FLOAT = 2,
    KIMIX_SQLITE_TEXT = 3,
    KIMIX_SQLITE_BLOB = 4,
    KIMIX_SQLITE_NULL = 5
} kimix_sqlite_type;

/* The open-mode flags for kimix_sqlite_open_v2(); OR them together.  These
 * are SQLite's SQLITE_OPEN_* values. */
typedef enum kimix_sqlite_open_flags {
    KIMIX_SQLITE_OPEN_READONLY = 0x00000001,  /* fail when the file is missing */
    KIMIX_SQLITE_OPEN_READWRITE = 0x00000002, /* read and write (no create) */
    KIMIX_SQLITE_OPEN_CREATE = 0x00000004,    /* create the file when missing */
    KIMIX_SQLITE_OPEN_URI = 0x00000040,       /* filename is a URI ("file:...") */
    KIMIX_SQLITE_OPEN_MEMORY = 0x00000080,    /* private in-memory database */
    KIMIX_SQLITE_OPEN_FULLMUTEX = 0x00010000  /* serialized threading (kimix_sqlite_open always sets it) */
} kimix_sqlite_open_flags;

/* ===========================================================================
 * 1. Library
 * ======================================================================== */

/* The SQLite library version string, e.g. "3.46.1".  Static storage owned by
 * the library: never free it.  Never fails. */
KIMIX_FFI const char *kimix_sqlite_libversion(void);

/* The SQLite library version as (major * 1 000 000) + (minor * 1 000) +
 * patch, e.g. 3046001 for 3.46.1.  Never fails. */
KIMIX_FFI int kimix_sqlite_libversion_number(void);

/* True when the SQLite library was compiled serialized or multi-threaded
 * (this build: SQLITE_THREADSAFE=1, so always true here).  Never fails. */
KIMIX_FFI bool kimix_sqlite_threadsafe(void);

/* ===========================================================================
 * 2. Connection lifetime
 * ======================================================================== */

/* Open (or create) a database.  `filename` is a UTF-8 path in the platform's
 * filename encoding rules -- ":memory:" opens a private in-memory database,
 * "" opens a private temporary on-disk database.  The connection is opened
 * READWRITE | CREATE | FULLMUTEX.
 *
 * On success *out_db receives the connection (only written on success).  On
 * failure *out_db is untouched: there is no connection to call
 * kimix_sqlite_errmsg() on afterwards, so probe failures by attempting the
 * first real operation (SQLite opens lazily; even a successful kimix_sqlite_open
 * reports a missing/corrupt file only at the first SQL).
 *
 * KIMIX_ERR_INVALID_ARG for a NULL `filename` or `out_db`; KIMIX_OK (with a
 * live connection) otherwise.  Release with kimix_sqlite_close() exactly
 * once. */
KIMIX_FFI kimix_status kimix_sqlite_open(KIMIX_IN const char *filename,
                                         KIMIX_OUT kimix_sqlite_db **out_db);

/* kimix_sqlite_open() with the caller choosing the open mode; `flags` is an
 * OR of kimix_sqlite_open_flags.  Passing 0 for `flags` behaves like
 * READONLY.  The same error behaviour as kimix_sqlite_open() applies. */
KIMIX_FFI kimix_status kimix_sqlite_open_v2(KIMIX_IN const char *filename,
                                            KIMIX_OUT kimix_sqlite_db **out_db,
                                            KIMIX_IN int flags);

/* Close the connection and release every statement the caller still holds on
 * it (SQLite finalizes them silently -- the C API cannot force the caller's
 * hands, so a forgotten kimix_sqlite_finalize() is cleaned up here, exactly
 * like sqlite3_close()).  The handle dangles afterwards: a second close is
 * undefined behaviour, like sqlite3_close() twice.
 *
 * KIMIX_ERR_INVALID_ARG for a NULL `db`; KIMIX_OK on a clean close. */
KIMIX_FFI kimix_status kimix_sqlite_close(KIMIX_TRANSFER kimix_sqlite_db *db);

/* ===========================================================================
 * 3. Error reporting
 * ======================================================================== */

/* The English error message of the MOST RECENT failed call on this connection
 * (SQLite's own "not an error" text when nothing failed yet).  BORROWED
 * storage owned by the connection:
 * valid until the next SQLite call on the same connection or until
 * kimix_sqlite_close(); never free it.  A NULL `db` yields "" (the neutral
 * string, since there is no status channel on this entry point).  Never
 * fails. */
KIMIX_FFI const char *kimix_sqlite_errmsg(KIMIX_IN const kimix_sqlite_db *db);

/* The primary result code of the most recent failed call (KIMIX_SQLITE_OK
 * when nothing failed yet).  A NULL `db` yields KIMIX_SQLITE_MISUSE.  Never
 * fails. */
KIMIX_FFI int kimix_sqlite_errcode(KIMIX_IN const kimix_sqlite_db *db);

/* The EXTENDED result code of the most recent failed call (the primary code
 * ORed with the sub-code, e.g. 2067 for SQLITE_CONSTRAINT_UNIQUE; == the
 * primary code when there is no sub-code).  Same NULL rule as
 * kimix_sqlite_errcode().  Never fails. */
KIMIX_FFI int kimix_sqlite_extended_errcode(KIMIX_IN const kimix_sqlite_db *db);

/* ===========================================================================
 * 4. One-shot execution
 * ======================================================================== */

/* Run one or more `;`-separated SQL statements (no parameters, no result
 * rows: CREATE / INSERT / UPDATE / DELETE / PRAGMA; for SELECT use a prepared
 * statement).  `sql` is UTF-8.
 *
 * On failure, when `out_errmsg` is not NULL, *out_errmsg receives a
 * NUL-terminated UTF-8 message allocated on THIS library's mimalloc heap --
 * release it with kimix_mem_free() (api/ffi_mem.h), never with sqlite3_free()
 * or the caller's own free().  On success *out_errmsg is untouched (so a
 * caller's NULL sentinel survives).  The same message stays readable through
 * kimix_sqlite_errmsg(db) without any heap involvement.
 *
 * Returns KIMIX_OK when every statement ran.  KIMIX_ERR_INVALID_ARG for a
 * NULL `db` or `sql`; KIMIX_ERR_FAILED when a statement failed (read the
 * message, or the connection error through kimix_sqlite_errcode(db)). */
KIMIX_FFI kimix_status kimix_sqlite_exec(KIMIX_IN_OUT kimix_sqlite_db *db,
                                         KIMIX_IN const char *sql,
                                         KIMIX_OUT KIMIX_NULLABLE char **out_errmsg);

/* ===========================================================================
 * 5. Changes
 * ======================================================================== */

/* The rowid of the most recent successful INSERT on this connection (0 when
 * none, and never for tables without a rowid alias).  Neutral value 0 for a
 * NULL `db`.  Never fails.  SQLite resets this on every new INSERT; read it
 * right after the step that inserted. */
KIMIX_FFI int64_t kimix_sqlite_last_insert_rowid(KIMIX_IN const kimix_sqlite_db *db);

/* The number of rows modified by the most recent completed INSERT / UPDATE /
 * DELETE on this connection (0 for a NULL `db` and for SELECT-only work).
 * Never fails.  SQLite resets this on every new statement that can change
 * data; read it right after the step that changed rows. */
KIMIX_FFI int64_t kimix_sqlite_changes(KIMIX_IN const kimix_sqlite_db *db);

/* How long SQLite waits (milliseconds) for a locked database file before
 * returning KIMIX_SQLITE_BUSY.  0 means "no wait" (the SQLite default).
 * KIMIX_ERR_INVALID_ARG for a NULL `db`; KIMIX_OK otherwise. */
KIMIX_FFI kimix_status kimix_sqlite_busy_timeout(KIMIX_IN_OUT kimix_sqlite_db *db,
                                                 KIMIX_IN int milliseconds);

/* ===========================================================================
 * 6. Prepared statements: lifetime
 * ======================================================================== */

/* Compile `sql` (UTF-8, ONE statement -- trailing text after the first
 * complete statement is ignored; only the first `?` / `:name` parameter set
 * is usable) into a statement owned by `db`.  On success *out_stmt receives
 * the statement (only written on success).  On failure the connection's error
 * message (kimix_sqlite_errmsg(db)) describes the problem.
 *
 * KIMIX_ERR_INVALID_ARG for a NULL `db`, `sql` or `out_stmt`;
 * KIMIX_ERR_FAILED for a compile error.  Release with kimix_sqlite_finalize()
 * exactly once; a statement borrows its connection and dangles with it. */
KIMIX_FFI kimix_status kimix_sqlite_prepare(KIMIX_IN kimix_sqlite_db *db,
                                            KIMIX_IN const char *sql,
                                            KIMIX_OUT kimix_sqlite_stmt **out_stmt);

/* Advance the statement: KIMIX_SQLITE_ROW when a result row is ready (read it
 * with the kimix_sqlite_column_* family, then step again),
 * KIMIX_SQLITE_DONE when the statement finished (INSERT/UPDATE/DELETE after
 * all changes; SELECT after the last row).  Any other value is an error --
 * read kimix_sqlite_errmsg() on the owning connection.
 *
 * A fresh statement starts BEFORE the first row; the first call steps INTO the
 * first row.  Returns KIMIX_SQLITE_MISUSE for a NULL `stmt`. */
KIMIX_FFI int kimix_sqlite_step(KIMIX_IN_OUT kimix_sqlite_stmt *stmt);

/* Release the statement.  KIMIX_OK when the statement ran cleanly to
 * completion; KIMIX_ERR_FAILED when the last step of the statement itself
 * failed (this call reports what step() could not deliver through its return
 * -- the error code the statement died with).  KIMIX_ERR_INVALID_ARG for a
 * NULL `stmt`. */
KIMIX_FFI kimix_status kimix_sqlite_finalize(KIMIX_TRANSFER kimix_sqlite_stmt *stmt);

/* Reset the statement back to its starting state WITHOUT clearing the
 * parameter bindings: after KIMIX_OK it can be stepped again (the same bound
 * values apply).  This is the cheap way to re-run an INSERT in a loop.
 * KIMIX_ERR_INVALID_ARG for a NULL `stmt`; KIMIX_ERR_FAILED when the most
 * recent step of the statement failed. */
KIMIX_FFI kimix_status kimix_sqlite_reset(KIMIX_IN_OUT kimix_sqlite_stmt *stmt);

/* Clear every parameter binding back to SQL NULL (the VALUES, not the
 * parameter COUNT).  KIMIX_ERR_INVALID_ARG for a NULL `stmt`; KIMIX_OK
 * otherwise. */
KIMIX_FFI kimix_status kimix_sqlite_clear_bindings(KIMIX_IN_OUT kimix_sqlite_stmt *stmt);

/* ===========================================================================
 * 7. Prepared statements: parameter binding
 * ======================================================================== */

/* The number of parameters in the compiled statement (`?`, `?NNN`, `:name`,
 * `@name`, `$name` -- the largest NNN for anonymous forms).  0 for a NULL
 * `stmt` and for statements without parameters.  Never fails. */
KIMIX_FFI int kimix_sqlite_bind_parameter_count(KIMIX_IN const kimix_sqlite_stmt *stmt);

/* The 1-based index of the parameter called `name` (`:name` / `@name` /
 * `$name`, leading sign INCLUDED in the lookup), 0 when the name is not a
 * parameter of this statement.  A NULL `stmt` or `name` yields 0 (the neutral
 * value).  Never fails. */
KIMIX_FFI int kimix_sqlite_bind_parameter_index(KIMIX_IN const kimix_sqlite_stmt *stmt,
                                                KIMIX_IN const char *name);

/* Bind the value of parameter `index` (1-based; an index outside [1,
 * bind_parameter_count()] is KIMIX_ERR_OUT_OF_RANGE).  The statement COPIES
 * bound text/blob bytes at bind time (SQLITE_TRANSIENT), so the caller's
 * buffer may die immediately after the call.  A binding survives step/reset;
 * it is cleared by kimix_sqlite_clear_bindings() or by the next bind on the
 * same index.  All five entry points return KIMIX_ERR_INVALID_ARG for a NULL
 * `stmt` and KIMIX_ERR_OUT_OF_RANGE for a bad `index`. */

/* A signed 64-bit integer. */
KIMIX_FFI kimix_status kimix_sqlite_bind_int64(KIMIX_IN_OUT kimix_sqlite_stmt *stmt,
                                               KIMIX_IN int index, KIMIX_IN int64_t value);

/* An IEEE-754 double. */
KIMIX_FFI kimix_status kimix_sqlite_bind_double(KIMIX_IN_OUT kimix_sqlite_stmt *stmt,
                                                KIMIX_IN int index, KIMIX_IN double value);

/* The SQL NULL value. */
KIMIX_FFI kimix_status kimix_sqlite_bind_null(KIMIX_IN_OUT kimix_sqlite_stmt *stmt,
                                              KIMIX_IN int index);

/* UTF-8 text, `length` BYTES (not code points).  A NULL `text` binds SQL
 * NULL; any other pointer binds exactly `length` bytes, so `""` / 0 is the
 * EMPTY string (use kimix_sqlite_bind_null() for SQL NULL).
 * `length` < 0 is KIMIX_ERR_INVALID_ARG. */
KIMIX_FFI kimix_status kimix_sqlite_bind_text(KIMIX_IN_OUT kimix_sqlite_stmt *stmt,
                                              KIMIX_IN int index,
                                              KIMIX_IN KIMIX_NULLABLE const char *text,
                                              KIMIX_IN int length);

/* An opaque blob, `length` BYTES.  A NULL `data` binds SQL NULL; any other
 * pointer binds exactly `length` bytes, so a non-NULL pointer with 0 is the
 * ZERO-LENGTH blob (use kimix_sqlite_bind_null() for SQL NULL).
 * `length` < 0 is KIMIX_ERR_INVALID_ARG. */
KIMIX_FFI kimix_status kimix_sqlite_bind_blob(KIMIX_IN_OUT kimix_sqlite_stmt *stmt,
                                              KIMIX_IN int index,
                                              KIMIX_IN KIMIX_NULLABLE const void *data,
                                              KIMIX_IN int length);

/* ===========================================================================
 * 8. Result columns
 * ======================================================================== */

/* The number of columns in the current (or next) result row -- the width of
 * the SELECT list, also for PRAGMA results.  0 for a NULL `stmt`.  Never
 * fails. */
KIMIX_FFI int kimix_sqlite_column_count(KIMIX_IN const kimix_sqlite_stmt *stmt);

/* The storage class of column `col` (a kimix_sqlite_type value) in the
 * CURRENT row.  Read it before choosing which typed accessor to call.
 * KIMIX_SQLITE_NULL for a NULL `stmt` or an out-of-range `col`.  Never fails
 * (SQLite converts the value on demand; the type follows the conversion). */
KIMIX_FFI int kimix_sqlite_column_type(KIMIX_IN const kimix_sqlite_stmt *stmt, KIMIX_IN int col);

/* The value of column `col` as a signed 64-bit integer (SQLite converts the
 * stored value; text that does not parse converts to 0; a REAL truncates
 * toward zero).  0 for a NULL `stmt` or an out-of-range `col`. */
KIMIX_FFI int64_t kimix_sqlite_column_int64(KIMIX_IN const kimix_sqlite_stmt *stmt, KIMIX_IN int col);

/* The value of column `col` as a double (integers widen exactly).  0.0 for a
 * NULL `stmt` or an out-of-range `col`. */
KIMIX_FFI double kimix_sqlite_column_double(KIMIX_IN const kimix_sqlite_stmt *stmt, KIMIX_IN int col);

/* The byte length of the column's text or blob value (call it AFTER the
 * matching column_text()/column_blob() when the length matters: the accessor
 * that produced the value defines the encoding width).  0 for a NULL `stmt`,
 * an out-of-range `col`, or a SQL NULL value. */
KIMIX_FFI int kimix_sqlite_column_bytes(KIMIX_IN const kimix_sqlite_stmt *stmt, KIMIX_IN int col);

/* The text value of column `col` as NUL-terminated UTF-8.  NULL when the
 * value is SQL NULL (test kimix_sqlite_column_type() first).  BORROWED:
 * valid until the next step/reset/finalize on this statement; NEVER free it.
 * NULL for a NULL `stmt` or an out-of-range `col`. */
KIMIX_FFI const char *kimix_sqlite_column_text(KIMIX_IN const kimix_sqlite_stmt *stmt, KIMIX_IN int col);

/* The blob value of column `col` as raw bytes, `kimix_sqlite_column_bytes()`
 * long.  NULL when the value is SQL NULL or empty (an empty blob reads back
 * with bytes == 0; treat 0 bytes as empty whether the pointer is NULL or
 * not).  BORROWED with the same lifetime as column_text(); NEVER free it.
 * NULL for a NULL `stmt` or an out-of-range `col`. */
KIMIX_FFI const void *kimix_sqlite_column_blob(KIMIX_IN const kimix_sqlite_stmt *stmt, KIMIX_IN int col);

/* The name of column `col` as declared in the SELECT list (the AS alias when
 * present), NUL-terminated UTF-8.  BORROWED with the same lifetime as
 * column_text().  NULL for a NULL `stmt` or an out-of-range `col`; the text
 * is whatever the query produced -- SQLite synthesizes names when the query
 * names nothing. */
KIMIX_FFI const char *kimix_sqlite_column_name(KIMIX_IN const kimix_sqlite_stmt *stmt, KIMIX_IN int col);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_SQLITE_H */
