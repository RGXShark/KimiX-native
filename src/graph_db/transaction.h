/*
 * transaction.h — transactions for the GDI layer (GDI §2.7).
 *
 * GDI has three transaction shapes and this file implements all three:
 *   auto        no transaction at all: every command applies immediately (the
 *               container's own behaviour, zero journal cost).
 *   explicit    begin_transaction() / commit() / abort(): the Database journals
 *               the previous state of everything it is about to change, so an
 *               abort restores the graph bit-identically.
 *   collective  begin_collective() / close_collective(): a READ-ONLY scope. Every
 *               write returns error_read_only_transaction. There is no snapshot
 *               isolation (the store is single-writer by contract): a collective
 *               transaction is consistent exactly when no writer runs during it,
 *               which is what Capability::supports_collective_transactions
 *               advertises and what the C# reference backend does too.
 *
 * THE JOURNAL IS AN UNDO LOG, NOT A SNAPSHOT. Each mutation appends the inverse
 * information it needs (the removed object, or the previous payload of an edited
 * one); abort replays it in REVERSE. That keeps a transaction's memory
 * proportional to what it changed instead of to the size of the graph, and it is
 * bounded: GraphOptions::max_journal_bytes (64 MiB by default) turns an oversized
 * transaction into error_size_limit with the transaction still abortable.
 *
 * ONE WRITE TRANSACTION AT A TIME. A second begin_transaction() returns
 * error_transaction; that is the container's single-writer contract surfaced as a
 * status instead of an assert.
 *
 * A Transaction object that is destroyed while still open ABORTS in its
 * destructor (noexcept), so an early return can never leave the database in a
 * half-applied state.
 *
 * Example:
 *   kimix::graph_db::Database db;
 *   db.open(path, {});
 *   db.begin_transaction();
 *   ... a batch of writes, any of which may fail ...
 *   db.abort();                    // the graph is exactly as it was
 */
#pragma once

#include "edge.h"
#include "graph_db_api.h"
#include "property.h"
#include "schema.h"
#include "status.h"
#include "vertex.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/stl/memory.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

class Database;

// One undo record. The `kind` says which payload member is meaningful; the
// rollback in database.cpp switches on it. Everything a record owns is moved in,
// so replaying it needs no lookup of the removed entity's old contents.
struct KIMIX_GRAPH_DB_API JournalEntry {
    enum class Kind : uint8_t {
        vertex_created = 0,      // undo: remove uid
        vertex_removed,          // undo: re-adopt `vertex` and `removed_edges`
        vertex_payload,          // undo: restore labels/properties of uid
        edge_created,            // undo: free edge_id
        edge_removed,            // undo: re-adopt `edge`
        edge_state,              // undo: restore endpoints/direction/labels/props
        label_created,           // undo: free label id
        label_renamed,           // undo: rename label id back
        label_freed,             // undo: re-register entry
        property_type_created,   // undo: free property type id
        property_type_updated,   // undo: restore the previous declaration
        property_type_freed,     // undo: re-register info
    };

    Kind kind{Kind::vertex_created};
    kimix::Guid uid;                 // vertex or edge id
    kimix::Guid origin;              // edge_state / vertex_removed helpers
    kimix::Guid target;
    EdgeDirectionType direction{EdgeDirectionType::directed};
    LabelId label{kNoLabel};
    PropNameId property{kNoPropName};
    kimix::string name;              // label / property-type name
    PropertyTypeInfo info;           // property_type_* payload
    kimix::vector<LabelId> labels;   // vertex_payload / edge_state payload
    PropertyList properties;         // vertex_payload / edge_state payload
    kimix::unique_ptr<Vertex> vertex;                       // vertex_removed
    kimix::vector<kimix::unique_ptr<Edge>> removed_edges;   // vertex_removed (cascade)
    kimix::unique_ptr<Edge> edge;                           // edge_removed

    // Rough heap footprint of this entry, for the journal cap. Exact enough to
    // bound a transaction; the plan only needs "does not grow without limit".
    [[nodiscard]] size_t approximate_bytes() const noexcept;
};

/*
 * One open transaction. Created by Database::begin_transaction() and owned by the
 * Database (which also deletes it on commit/abort); a caller never constructs one.
 */
class KIMIX_GRAPH_DB_API Transaction : public kimix::IOperatorNewBase {
public:
    Transaction(Database &database, bool read_only, size_t max_journal_bytes) noexcept;
    Transaction(Transaction const &) = delete;
    Transaction &operator=(Transaction const &) = delete;
    Transaction(Transaction &&) = delete;
    Transaction &operator=(Transaction &&) = delete;
    // Destroying an open transaction aborts it (noexcept: a failure here is a bug,
    // and there is nothing a caller could do about it during unwinding).
    ~Transaction() noexcept;

    [[nodiscard]] bool read_only() const noexcept { return _read_only; }
    [[nodiscard]] bool collective() const noexcept { return _collective; }
    [[nodiscard]] bool closed() const noexcept { return _closed; }
    [[nodiscard]] uint64_t id() const noexcept { return _id; }
    [[nodiscard]] size_t journal_bytes() const noexcept { return _journal_bytes; }
    [[nodiscard]] size_t journal_entries() const noexcept { return _journal.size(); }
    [[nodiscard]] bool journal_overflowed() const noexcept { return _overflowed; }

    // Append an undo record. Returns false (and sets the overflow flag) when the
    // journal cap would be exceeded; the caller reports error_size_limit and leaves
    // the transaction abortable.
    bool push(JournalEntry entry);
    void clear_journal() noexcept;

    [[nodiscard]] kimix::vector<JournalEntry> const &journal() const noexcept {
        return _journal;
    }
    // Take the journal (the rollback walks it in reverse and consumes it).
    [[nodiscard]] kimix::vector<JournalEntry> take_journal() noexcept;

private:
    friend class Database;

    Database &_database;
    kimix::vector<JournalEntry> _journal;
    size_t _journal_bytes{0};
    size_t _max_journal_bytes{0};
    uint64_t _id{0};
    bool _read_only{false};
    bool _collective{false};
    bool _closed{false};
    bool _overflowed{false};
};

} // namespace kimix::graph_db
