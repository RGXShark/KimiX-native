/*
 * capability.cpp — capability advertisement + the name diff.
 *
 * `current_capability()` is the one place the advertisement is decided; the
 * C ABI forwards it and reports/gdi_alignment.md documents which phase flipped
 * which flag. Keep it honest: the flags mirror what Database actually
 * implements (see the table in capability.h).
 */
#include "capability.h"

namespace kimix::graph_db {

Capability current_capability() noexcept {
    Capability cap;
    // P2: the (label, external_id) KeyIndex is authoritative and refuses a
    // duplicate key with error_non_unique_id (GraphOptions::key_mode can relax
    // it to the warning path, which is what WarningNonUniqueId exists for).
    cap.enforces_unique_external_ids = true;
    // P3: edges are first-class records with a stable uid that survives
    // save/load, and they carry labels and properties.
    cap.supports_edge_by_stable_uid = true;
    cap.supports_edge_property_read = true;
    // P6: begin_collective/close_collective install a read-only scope.
    cap.supports_collective_transactions = true;
    // P9 (deferred): global scan, native multi-hop, global aggregates.
    cap.supports_global_scan = false;
    cap.supports_native_multi_hop = false;
    cap.max_native_hop_depth = 0;
    cap.supports_global_aggregates = false;
    return cap;
}

kimix::vector<kimix::string> capability_name_diff(Capability const &lhs,
                                                  Capability const &rhs) {
    kimix::vector<kimix::string> out;
    if (lhs.supports_global_scan != rhs.supports_global_scan) {
        out.push_back(kimix::string{"supports_global_scan"});
    }
    if (lhs.supports_edge_by_stable_uid != rhs.supports_edge_by_stable_uid) {
        out.push_back(kimix::string{"supports_edge_by_stable_uid"});
    }
    if (lhs.supports_edge_property_read != rhs.supports_edge_property_read) {
        out.push_back(kimix::string{"supports_edge_property_read"});
    }
    if (lhs.supports_native_multi_hop != rhs.supports_native_multi_hop) {
        out.push_back(kimix::string{"supports_native_multi_hop"});
    }
    if (lhs.max_native_hop_depth != rhs.max_native_hop_depth) {
        out.push_back(kimix::string{"max_native_hop_depth"});
    }
    if (lhs.supports_global_aggregates != rhs.supports_global_aggregates) {
        out.push_back(kimix::string{"supports_global_aggregates"});
    }
    if (lhs.supports_collective_transactions != rhs.supports_collective_transactions) {
        out.push_back(kimix::string{"supports_collective_transactions"});
    }
    if (lhs.enforces_unique_external_ids != rhs.enforces_unique_external_ids) {
        out.push_back(kimix::string{"enforces_unique_external_ids"});
    }
    return out;
}

} // namespace kimix::graph_db
