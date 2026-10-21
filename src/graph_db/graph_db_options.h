/*
 * graph_db_options.h — GraphOptions: the tunables of one database instance.
 *
 * Everything a caller might reasonably want to change lives here from the first
 * phase on, even where a later phase is the one that starts honouring a field.
 * That is deliberate: the options travel through the C ABI's open() struct as
 * well, so adding a field later would be an ABI change; adding it now (with the
 * documented default) is not.
 *
 * FIELDS
 *   auto_create_schema        implicit label / property-type creation on write.
 *                             true (the container-friendly default): adding a
 *                             label that is not registered creates it. false (the
 *                             GDI-strict posture): an unregistered name is
 *                             error_label / error_property_type.
 *   key_mode                  what create_vertex does with a duplicate
 *                             (label, external_id) key: refuse it
 *                             (error_non_unique_id, the GDI default) or return
 *                             the existing vertex with warning_non_unique_id
 *                             (the SimulateNonUniqueId path).
 *   allow_parallel_edges      applies to create_edge only, never to the legacy
 *                             connect(): the container API keeps its
 *                             one-edge-per-ordered-pair rule.
 *   allow_self_edges          same scoping: create_edge honours it, connect()
 *                             still refuses from == to.
 *   max_journal_bytes         cap on one transaction's undo journal; exceeding
 *                             it yields error_size_limit with the transaction
 *                             still abortable.
 *   cold_map_size             LMDB map size handed to ColdStorage::open.
 *   persist_on_commit         commit() runs GraphSerializer::save_incremental.
 *   graph_name                the name recorded in the store's meta record and
 *                             used by the (later) named-graph catalog.
 *
 * Example:
 *   kimix::graph_db::GraphOptions options;
 *   options.auto_create_schema = false;   // GDI-strict: names must exist
 *   options.key_mode = kimix::graph_db::KeyMode::report_warning;
 */
#pragma once

#include "graph_db_api.h"

#include <core/stl/string.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

// What a duplicate (label, external_id) key does.
enum class KeyMode : uint8_t {
    // Refuse the create: error_non_unique_id (GDI's EnforcesUniqueExternalIds).
    enforce_unique = 0,
    // Return the EXISTING vertex's uid plus warning_non_unique_id (GDI-RMA's
    // under-enforced uniqueness, surfaced as a warning).
    report_warning = 1,
};

struct KIMIX_GRAPH_DB_API GraphOptions {
    bool auto_create_schema{true};
    KeyMode key_mode{KeyMode::enforce_unique};
    bool allow_parallel_edges{true};
    bool allow_self_edges{true};
    // 64 MiB of undo journal per transaction (see max_journal_bytes).
    size_t max_journal_bytes{64ull * 1024ull * 1024ull};
    // ColdStorage::default_map_size (64 GiB); 0 means "keep the storage default".
    size_t cold_map_size{0};
    bool persist_on_commit{false};
    kimix::string graph_name;
};

} // namespace kimix::graph_db
