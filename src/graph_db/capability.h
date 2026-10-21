/*
 * capability.h — the honest capability advertisement of this backend.
 *
 * Semevia.Gdi's BufferValidator turns each flag into an accept/degrade/reject
 * decision when a command buffer is validated, so a wrong `true` is worse than a
 * missing feature: it makes the adapter emit a command the store cannot honour.
 * The flags below are therefore set from what the implementation ACTUALLY does,
 * phase by phase, and `capability_name_diff()` lets a test (or the C# side)
 * assert the difference between two advertisements instead of eyeballing a
 * struct.
 *
 * Current state (see reports/gdi_alignment.md for the phase table):
 *   supports_global_scan            false  — no vertex iteration API (P9)
 *   supports_edge_by_stable_uid     true   (P3: edge ids survive save/load)
 *   supports_edge_property_read     true   (P3: edges carry properties)
 *   supports_native_multi_hop       false  — composed BFS only (P9)
 *   max_native_hop_depth            0
 *   supports_global_aggregates      false  (P9)
 *   supports_collective_transactions true  (P6: read-only scope, no snapshot)
 *   enforces_unique_external_ids    true   (P2: KeyIndex refuses a duplicate)
 *
 * Example:
 *   auto cap = kimix::graph_db::current_capability();
 *   if (!cap.supports_global_scan) { fall back to a key lookup; }
 */
#pragma once

#include "graph_db_api.h"

#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstdint>

namespace kimix::graph_db {

/*
 * The per-backend capability advertisement — a field-for-field mirror of
 * Semevia.Gdi.Capability (same names, same meaning, snake_case spelling).
 */
struct KIMIX_GRAPH_DB_API Capability {
    // G1: can a command scan/filter the whole vertex set?
    bool supports_global_scan{false};
    // G2: can an edge created in an earlier batch/transaction be addressed by
    // its stable uid (GDI_AssociateEdge semantics)?
    bool supports_edge_by_stable_uid{false};
    // G2: is edge property read-back available (GDI_GetPropertiesOfEdge)?
    bool supports_edge_property_read{false};
    // G3: does the store evaluate multi-hop traversals natively?
    bool supports_native_multi_hop{false};
    // G3: native traversal depth limit (meaningful only with the flag above).
    int32_t max_native_hop_depth{0};
    // G4: are graph-wide aggregates available?
    bool supports_global_aggregates{false};
    // GDI_StartCollectiveTransaction equivalent.
    bool supports_collective_transactions{false};
    // Does the backend itself enforce (label, external_id) uniqueness?
    bool enforces_unique_external_ids{false};

    [[nodiscard]] bool operator==(Capability const &other) const noexcept {
        return supports_global_scan == other.supports_global_scan &&
               supports_edge_by_stable_uid == other.supports_edge_by_stable_uid &&
               supports_edge_property_read == other.supports_edge_property_read &&
               supports_native_multi_hop == other.supports_native_multi_hop &&
               max_native_hop_depth == other.max_native_hop_depth &&
               supports_global_aggregates == other.supports_global_aggregates &&
               supports_collective_transactions == other.supports_collective_transactions &&
               enforces_unique_external_ids == other.enforces_unique_external_ids;
    }
    [[nodiscard]] bool operator!=(Capability const &other) const noexcept {
        return !(*this == other);
    }
};

// What THIS build advertises. The value moves forward with the phases and is
// the single place the advertisement is decided (the C ABI forwards it).
[[nodiscard]] KIMIX_GRAPH_DB_API Capability current_capability() noexcept;

// The names of the flags that differ between `lhs` and `rhs` — the enumerable
// difference between two capabilities (empty when they are equivalent), i.e. the
// mirror of Semevia.Gdi.Capability.Diff. Names are the struct member names, so a
// failing comparison reads as the list of gaps.
[[nodiscard]] KIMIX_GRAPH_DB_API kimix::vector<kimix::string>
capability_name_diff(Capability const &lhs, Capability const &rhs);

} // namespace kimix::graph_db
