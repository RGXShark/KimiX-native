// Test for the native multi-hop traversal (src/graph_db/graph.{h,cpp}
// Graph::traverse_bfs / Graph::shortest_path bounded overload — G3, plan v3
// step 1.2).
//
// This test covers:
// - exact equivalence against a brute-force BFS (over the public allocating
//   adjacency forms) on the known 6-vertex DAG and a 200-vertex layered DAG,
//   over the cap x depth x orientation matrix, filtered and unfiltered;
// - cycle safety (a ring with a chord terminates and still matches the
//   reference);
// - the cap contract: at most cap_per_level vertices per level, a full level
//   stops expanding, depth 0 yields exactly the start;
// - the §5.10 filter contract: a filtered-out vertex is not enqueued, so its
//   neighbourhood is never visited (label and property conditions);
// - argument errors: cap < 1, depth 0 / > kMaxNativeHopDepth, unknown start,
//   a label condition with an ordering op (error_op);
// - determinism: repeated traversals return the identical sequence;
// - the bounded shortest_path: same result as the unbounded form when the
//   bound suffices, empty when it does not, error_vertex on unknown endpoints;
// - the EDGE-filtered shortest_path (the edge-searching form): equivalence
//   against a brute-force walk over the PUBLIC edge reads over the
//   constraint x orientation x depth x endpoint matrix, the reported hop edges
//   (parallel edges included), the zero-length and unreachable answers, the
//   error surface, oriented_links / edge_matches, and the filtered has_path;
// - a 4-reader / 1-writer stress run (no crash, BFS invariants hold, and a
//   post-run quiescent traversal matches the brute force exactly).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/graph.h>
#include <graph_db/graph_db.h>
#include <graph_db/query.h>
#include <graph_db/schema.h>

#include <core/guid.h>
#include <core/stl/unordered_map.h> // kimix::unordered_set lives here
#include <core/stl/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::AdjacencyBuffer;
using graph_db::Constraint;
using graph_db::ConstraintCondition;
using graph_db::ConstraintHandle;
using graph_db::Database;
using graph_db::EdgeId;
using graph_db::EdgeLink;
using graph_db::EdgeOrientation;
using graph_db::EdgeRecord;
using graph_db::GdiOp;
using graph_db::GdiStatus;
using graph_db::Graph;
using graph_db::LabelId;
using graph_db::PropNameId;
using graph_db::PropertyBag;
using graph_db::PropertyList;
using graph_db::PropertyValue;
using graph_db::Vertex;
using PathStep = graph_db::Graph::PathStep;
using TraversalVertex = graph_db::Graph::TraversalVertex;
namespace gtest = kimix::graph_db::test;

namespace {

// Fabricated interned ids (the Graph level has no schema to register them in;
// LabelId / PropNameId are plain uint32_t aliases, property.h).
inline constexpr LabelId kPersonLabel = 7u;
inline constexpr LabelId kRobotLabel = 8u;
inline constexpr PropNameId kAgeName = 5u;
inline constexpr PropNameId kNameName = 6u;

// One vertex of the brute-force reference: same shape as TraversalVertex.
struct RefVertex {
    kimix::Guid guid;
    kimix::Guid parent;
    uint32_t depth;
};

// The oriented adjacency as a fresh owning copy (the public allocating forms).
kimix::vector<kimix::Guid> oriented_adjacency(Graph const &graph, kimix::Guid const &guid,
                                              EdgeOrientation orientation) {
    if (orientation == EdgeOrientation::outgoing) {
        return graph.dependencies(guid);
    }
    if (orientation == EdgeOrientation::incoming) {
        return graph.dependents(guid);
    }
    // Both: out then in, concatenated (the same order _assign_two uses); the
    // global visited set below removes the cross-list duplicates.
    kimix::vector<kimix::Guid> out = graph.dependencies(guid);
    kimix::vector<kimix::Guid> in = graph.dependents(guid);
    out.insert(out.end(), in.begin(), in.end());
    return out;
}

// Does `vertex` satisfy `constraint` (the §5.10 vertex filter)?
bool vertex_matches(Graph const &graph, kimix::Guid const &guid, Constraint const &constraint) {
    Vertex *vertex = graph.find(guid);
    if (vertex == nullptr) {
        return false; // a vanished endpoint is skipped, not an error
    }
    kimix::vector<LabelId> labels;
    vertex->labels(labels);
    PropertyBag bag;
    PropertyList list = vertex->property_list();
    for (auto const &entry : list) {
        bag.emplace(entry.name, entry.value);
    }
    return graph_db::matches(constraint, kimix::span<const LabelId>{labels.data(),
                                                                    labels.size()},
                             bag);
}

// Brute-force BFS with exactly Graph::traverse_bfs's semantics: the start at
// depth 0 unconditionally; per-level discovery in adjacency order; a level
// stops expanding the moment it reaches `cap`; `max_depth` bounds the walk;
// an optional vertex filter gates ENQUEUE (a failed vertex's neighbourhood
// is never visited); duplicates never re-enqueued.
kimix::vector<RefVertex> brute_force_bfs(Graph const &graph, kimix::Guid const &start,
                                         Constraint const *constraint,
                                         EdgeOrientation orientation, int64_t cap,
                                         uint32_t max_depth) {
    kimix::vector<RefVertex> result;
    result.push_back(RefVertex{start, kimix::Guid{}, 0});
    kimix::unordered_set<kimix::Guid> visited;
    visited.insert(start);
    kimix::vector<kimix::Guid> frontier{start};
    for (uint32_t depth = 1; depth <= max_depth && !frontier.empty(); ++depth) {
        kimix::vector<kimix::Guid> next;
        int64_t level_count = 0;
        bool level_full = false;
        for (auto const &current : frontier) {
            if (level_full) {
                break;
            }
            for (auto const &candidate : oriented_adjacency(graph, current, orientation)) {
                if (level_count >= cap) {
                    level_full = true;
                    break;
                }
                if (visited.find(candidate) != visited.end()) {
                    continue;
                }
                if (constraint != nullptr && !vertex_matches(graph, candidate, *constraint)) {
                    continue;
                }
                visited.insert(candidate);
                result.push_back(RefVertex{candidate, current, depth});
                next.push_back(candidate);
                ++level_count;
            }
        }
        frontier = std::move(next);
    }
    return result;
}

// Exact sequence equality (guid, parent, depth — order included).
bool same_walk(kimix::vector<TraversalVertex> const &actual,
               kimix::vector<RefVertex> const &expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < actual.size(); ++i) {
        if (!(actual[i].guid == expected[i].guid &&
              actual[i].parent == expected[i].parent &&
              actual[i].depth == expected[i].depth)) {
            return false;
        }
    }
    return true;
}

// Build a star-of-stars hub graph: `spokes` leaves around one hub, plus a
// second-level hub reached from the first (so 2-hop walks have real work).
struct hub_graph {
    Graph graph;
    Vertex *hub{nullptr};
    Vertex *hub2{nullptr};
    kimix::vector<Vertex *> leaves;
    kimix::vector<Vertex *> outer;

    explicit hub_graph(size_t spokes) {
        hub = graph.create_vertex();
        hub2 = graph.create_vertex();
        leaves.reserve(spokes);
        outer.reserve(spokes);
        graph.connect(hub->guid(), hub2->guid());
        for (size_t i = 0; i < spokes; ++i) {
            Vertex *leaf = graph.create_vertex();
            graph.connect(hub->guid(), leaf->guid()); // hub -> leaf (outgoing)
            leaves.push_back(leaf);
            Vertex *tip = graph.create_vertex();
            graph.connect(leaf->guid(), tip->guid()); // leaf -> far (2-hop)
            outer.push_back(tip);
        }
    }
};

// A ring of `count` vertices plus one chord — cycles everywhere, but a
// bounded BFS over it must terminate and match the reference.
void build_ring(Graph &graph, size_t count, kimix::vector<Vertex *> &out) {
    out.clear();
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(graph.create_vertex());
    }
    for (size_t i = 0; i < count; ++i) {
        graph.connect(out[i]->guid(), out[(i + 1) % count]->guid());
    }
    if (count > 3) {
        graph.connect(out[0]->guid(), out[count / 2]->guid()); // the chord
    }
}

// The BFS invariants every traverse_bfs result must satisfy DURING a
// concurrent-writer stress run.  Deliberately time-local only: start first at
// depth 0; no duplicates; per-level caps; depths increase by at most one per
// step.  Edge existence is NOT checked here — a parent edge may legitimately
// be removed by the writer between the discovery and this check (the walk was
// consistent at discovery time; the graph need not be consistent with it
// afterwards).  Quiescent consistency is verified separately by the
// brute-force comparison after the stress.
bool walk_is_consistent(kimix::vector<TraversalVertex> const &walk, kimix::Guid const &start,
                        int64_t cap_per_level) {
    if (walk.empty() || !(walk[0].guid == start) || walk[0].depth != 0) {
        return false;
    }
    kimix::unordered_set<kimix::Guid> seen;
    seen.insert(start);
    kimix::vector<int64_t> level_count(walk.back().depth + 1, 0);
    for (size_t i = 1; i < walk.size(); ++i) {
        TraversalVertex const &v = walk[i];
        if (v.depth == 0 || v.depth > walk[i - 1].depth + 1) {
            return false;
        }
        if (seen.find(v.guid) != seen.end()) {
            return false; // duplicate
        }
        seen.insert(v.guid);
        if (level_count[v.depth]++ >= cap_per_level) {
            return false; // per-level cap violated
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The EDGE-filtered path fixtures (the edge-searching form of shortest_path)
// ---------------------------------------------------------------------------

// Fabricated ids for the edge filters. The Graph level has no schema to register
// them in (LabelId / PropNameId are plain uint32_t aliases, property.h).
inline constexpr LabelId kOpenEdgeLabel = 21u;
inline constexpr LabelId kBlockedEdgeLabel = 22u;
inline constexpr PropNameId kCostName = 11u;

// The (neighbour, edge) pairs a brute force steps over from `current`, computed
// through the PUBLIC edge reads only — incident_edges + edge_record + matches —
// so the reference below shares no code with the engine's link-level walk.
void ref_hops(Graph const &graph, kimix::Guid const &current, EdgeOrientation orientation,
              Constraint const &constraint,
              kimix::vector<std::pair<kimix::Guid, EdgeId>> &out) {
    out.clear();
    kimix::vector<EdgeId> ids;
    graph.incident_edges(current, orientation, ids);
    for (auto const &id : ids) {
        EdgeRecord record;
        if (!graph.edge_record(id, record)) {
            continue;
        }
        if (!graph_db::matches(constraint, record.labels, record.properties)) {
            continue;
        }
        // The other end: an out link points at the target, an in link at the
        // origin; a self loop is both, and either expression gives `current`.
        kimix::Guid const neighbour = record.origin == current ? record.target : record.origin;
        out.push_back({neighbour, id});
    }
}

// A brute-force edge-filtered bounded shortest path: `vertices` is the path with
// its endpoints included, `edges[i]` the edge that reaches `vertices[i]`, both
// empty when `to` is unreachable within the bound.
struct RefPath {
    kimix::vector<kimix::Guid> vertices;
    kimix::vector<EdgeId> edges;
};

RefPath ref_filtered_path(Graph const &graph, kimix::Guid const &from, kimix::Guid const &to,
                          Constraint const &constraint, EdgeOrientation orientation,
                          uint32_t max_depth) {
    RefPath path;
    if (from == to) {
        path.vertices.push_back(from);
        path.edges.push_back(EdgeId{});
        return path;
    }
    kimix::unordered_map<kimix::Guid, kimix::Guid> parent;
    kimix::unordered_map<kimix::Guid, EdgeId> via;
    parent.emplace(from, from);
    via.emplace(from, EdgeId{});
    kimix::vector<kimix::Guid> frontier;
    kimix::vector<kimix::Guid> next_frontier;
    kimix::vector<std::pair<kimix::Guid, EdgeId>> hops;
    frontier.push_back(from);
    uint32_t depth = 0;
    bool found = false;
    while (!frontier.empty() && !found && depth < max_depth) {
        ++depth;
        next_frontier.clear();
        for (auto const &current : frontier) {
            ref_hops(graph, current, orientation, constraint, hops);
            for (auto const &hop : hops) {
                if (parent.find(hop.first) != parent.end()) {
                    continue;
                }
                parent.emplace(hop.first, current);
                via.emplace(hop.first, hop.second);
                if (hop.first == to) {
                    found = true;
                    break;
                }
                next_frontier.push_back(hop.first);
            }
            if (found) {
                break;
            }
        }
        frontier.swap(next_frontier);
    }
    if (!found) {
        return path;
    }
    kimix::vector<kimix::Guid> reversed;
    kimix::vector<EdgeId> reversed_edges;
    for (kimix::Guid cursor = to;; cursor = parent.find(cursor)->second) {
        reversed.push_back(cursor);
        reversed_edges.push_back(via.find(cursor)->second);
        if (cursor == from) {
            break;
        }
    }
    std::reverse(reversed.begin(), reversed.end());
    std::reverse(reversed_edges.begin(), reversed_edges.end());
    path.vertices = std::move(reversed);
    path.edges = std::move(reversed_edges);
    return path;
}

// The engine's path equals the reference: same vertices, same edges (so a
// parallel-edge choice is pinned too), and depth == index.
void expect_same_path(kimix::vector<Graph::PathStep> const &actual, RefPath const &expected,
                      char const *what) {
    expect(eq(actual.size(), expected.vertices.size())) << what;
    if (actual.size() != expected.vertices.size()) {
        return;
    }
    for (size_t i = 0; i < actual.size(); ++i) {
        expect(actual[i].vertex == expected.vertices[i]) << what;
        expect(actual[i].edge == expected.edges[i]) << what;
        expect(eq(actual[i].depth, static_cast<uint32_t>(i))) << what;
    }
}

// Every hop of a returned path must be REAL: the reported edge exists, joins the
// two consecutive vertices in the walked orientation, and satisfies the filter.
void expect_path_is_sound(Graph const &graph, kimix::vector<Graph::PathStep> const &path,
                          Constraint const &constraint, EdgeOrientation orientation,
                          kimix::Guid const &from, kimix::Guid const &to, char const *what) {
    expect(!path.empty()) << what;
    if (path.empty()) {
        return;
    }
    expect(path.front().vertex == from) << what;
    expect(path.back().vertex == to) << what;
    expect(!static_cast<bool>(path.front().edge)) << what << ": the start is reached by no edge";
    kimix::unordered_set<kimix::Guid> seen;
    for (size_t i = 0; i < path.size(); ++i) {
        expect(seen.insert(path[i].vertex).second) << what << ": the path is simple";
        if (i == 0) {
            continue;
        }
        EdgeRecord record;
        expect(graph.edge_record(path[i].edge, record)) << what << ": the hop's edge exists";
        expect(graph.edge_matches(path[i].edge, constraint)) << what << ": the hop's edge matches";
        bool const forward =
            record.origin == path[i - 1].vertex && record.target == path[i].vertex;
        bool const backward =
            record.origin == path[i].vertex && record.target == path[i - 1].vertex;
        bool const connects = orientation == EdgeOrientation::outgoing
                                  ? forward
                                  : orientation == EdgeOrientation::incoming ? backward
                                                                             : (forward || backward);
        expect(connects) << what << ": the hop's edge joins the two steps";
    }
}



} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    // -----------------------------------------------------------------------
    // Equivalence vs the brute force, over the whole cap x depth x orientation
    // matrix, on the known 6-vertex DAG.
    // -----------------------------------------------------------------------
    "traverse_bfs_matches_brute_force_small_dag"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        int64_t const caps[] = {1, 2, 3, 100};
        uint32_t const depths[] = {1u, 2u, 3u, Graph::kMaxNativeHopDepth};
        EdgeOrientation const orientations[] = {EdgeOrientation::outgoing,
                                                EdgeOrientation::incoming,
                                                EdgeOrientation::both};
        for (EdgeOrientation orientation : orientations) {
            for (int64_t cap : caps) {
                for (uint32_t depth : depths) {
                    for (size_t s = 0; s < dag.vertices.size(); ++s) {
                        kimix::Guid const start = dag.vertices[s]->guid();
                        kimix::vector<TraversalVertex> actual;
                        expect(eq(graph.traverse_bfs(start, {}, orientation, cap, depth,
                                                     actual),
                                  GdiStatus::ok));
                        auto expected =
                            brute_force_bfs(graph, start, nullptr, orientation, cap, depth);
                        expect(same_walk(actual, expected))
                            << "traverse_bfs matches the brute force (cap/depth/orientation matrix)";
                    }
                }
            }
        }
    };

    // The same, on a 200-vertex layered DAG with a sparse random structure.
    "traverse_bfs_matches_brute_force_layered"_test = [] {
        Graph graph;
        auto vertices = gtest::build_layered_dag(graph, 200, 777u);
        expect(eq(graph.size(), size_t{200}));
        std::mt19937 rng(9u);
        for (int round = 0; round < 40; ++round) {
            kimix::Guid const start = vertices[rng() % vertices.size()]->guid();
            int64_t const cap = 1 + static_cast<int64_t>(rng() % 12);
            uint32_t const depth = 1 + rng() % 4;
            EdgeOrientation const orientation =
                static_cast<EdgeOrientation>(rng() % 3u);
            kimix::vector<TraversalVertex> actual;
            expect(eq(graph.traverse_bfs(start, {}, orientation, cap, depth, actual),
                      GdiStatus::ok));
            auto expected =
                brute_force_bfs(graph, start, nullptr, orientation, cap, depth);
            expect(same_walk(actual, expected))
                << "traverse_bfs matches the brute force on the layered DAG";
        }
    };

    // Cycles: a ring with a chord must terminate and match the reference.
    "traverse_bfs_cycles_terminate_and_match"_test = [] {
        Graph graph;
        kimix::vector<Vertex *> ring;
        build_ring(graph, 24, ring);
        for (int64_t cap : {1, 3, 7, 100}) {
            for (uint32_t depth : {1u, 2u, 4u, 12u, Graph::kMaxNativeHopDepth}) {
                kimix::vector<TraversalVertex> actual;
                expect(eq(graph.traverse_bfs(ring[3]->guid(), {},
                                             EdgeOrientation::outgoing, cap, depth,
                                             actual),
                          GdiStatus::ok));
                auto expected = brute_force_bfs(graph, ring[3]->guid(), nullptr,
                                                EdgeOrientation::outgoing, cap, depth);
                expect(same_walk(actual, expected))
                    << "a cyclic graph terminates and matches the brute force";
            }
        }
    };

    // The cap and depth contract on a hub graph.
    "traverse_bfs_cap_and_depth_contract"_test = [] {
        hub_graph hubs(48);
        // Depth 1 from the hub: exactly the hub's out-adjacency (48 leaves + hub2),
        // capped at 10 -> 10 vertices, all at depth 1.
        {
            kimix::vector<TraversalVertex> walk;
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 10, 1, walk),
                      GdiStatus::ok));
            expect(eq(walk.size(), size_t{11})) << "start + 10 capped level-1 vertices";
            expect(walk.back().depth == 1_u);
        }
        // Depth 0 request is an error; so is a 0 or negative cap.
        {
            kimix::vector<TraversalVertex> walk;
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 10, 0, walk),
                      GdiStatus::error_argument));
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 0, 1, walk),
                      GdiStatus::error_argument));
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, -5, 1, walk),
                      GdiStatus::error_argument));
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 10,
                                              Graph::kMaxNativeHopDepth + 1, walk),
                      GdiStatus::error_argument))
                << "beyond the advertised max native hop depth";
        }
        // Depth 2 from the hub reaches outer vertices (depth 2) via leaves.
        {
            kimix::vector<TraversalVertex> walk;
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 100, 2, walk),
                      GdiStatus::ok));
            expect(eq(walk.back().depth, 2u));
            expect(eq(walk.size(), size_t{1 + 49 + 48})) // hub + leaves/hub2 + outer
                << "the 2-hop walk covers every reachable vertex";
        }
        // Unknown start.
        {
            kimix::vector<TraversalVertex> walk;
            kimix::Guid unknown = kimix::Guid(true);
            expect(eq(hubs.graph.traverse_bfs(unknown, {}, EdgeOrientation::outgoing, 10,
                                              1, walk),
                      GdiStatus::error_vertex));
            expect(walk.empty());
        }
    };

    // The §5.10 filter: a filtered-out vertex is not enqueued, so its own
    // neighbourhood is never visited — checked against the brute force.
    // Graph: hub (person, age 50) -> 8 leaves (alternating robot/age 70 and
    // person/age 20); each leaf -> one far vertex carrying the leaf's label
    // and age.
    "traverse_bfs_filter_gates_discovery"_test = [] {
        Graph filtered;
        auto *hub = filtered.create_vertex();
        hub->add_label(kPersonLabel);
        hub->put_property(kAgeName, PropertyValue::make_int64(50));
        size_t robot_leaves = 0;
        for (size_t i = 0; i < 8; ++i) {
            bool const is_robot = (i % 2u) == 0u;
            robot_leaves += is_robot ? 1u : 0u;
            LabelId const leaf_label = is_robot ? kRobotLabel : kPersonLabel;
            int64_t const leaf_age = is_robot ? 70 : 20;
            auto *leaf = filtered.create_vertex();
            leaf->add_label(leaf_label);
            leaf->put_property(kAgeName, PropertyValue::make_int64(leaf_age));
            filtered.connect(hub->guid(), leaf->guid());
            auto *tip = filtered.create_vertex();
            tip->add_label(leaf_label);
            tip->put_property(kAgeName, PropertyValue::make_int64(leaf_age));
            filtered.connect(leaf->guid(), tip->guid());
        }
        // Label filter: only robots pass, so the walk is hub + robot leaves +
        // robot fars — every discovered vertex at any depth is a robot.
        {
            Constraint robots;
            robots.conditions.push_back(
                ConstraintCondition::on_label(kRobotLabel, GdiOp::equal));
            kimix::vector<TraversalVertex> walk;
            expect(eq(filtered.traverse_bfs(hub->guid(), robots,
                                            EdgeOrientation::outgoing, 100, 2, walk),
                      GdiStatus::ok));
            auto expected = brute_force_bfs(filtered, hub->guid(), &robots,
                                            EdgeOrientation::outgoing, 100, 2);
            expect(same_walk(walk, expected))
                << "a label-filtered 2-hop walk matches the brute force";
            expect(eq(walk.size(), size_t{1 + robot_leaves * 2}));
            for (auto const &v : walk) {
                if (v.depth == 0) {
                    continue; // the start is returned unconditionally
                }
                Vertex *vertex = filtered.find(v.guid);
                expect(vertex->has_label(kRobotLabel)) << "only robots discovered";
            }
        }
        // Property filter: age >= 60 keeps the robots (70) and drops the person
        // leaves (20) — and the fars off the dropped leaves are never visited.
        {
            Constraint seniors;
            seniors.conditions.push_back(ConstraintCondition::on_property(
                kAgeName, GdiOp::greater_or_equal, PropertyValue::make_int64(60)));
            kimix::vector<TraversalVertex> walk;
            expect(eq(filtered.traverse_bfs(hub->guid(), seniors,
                                            EdgeOrientation::outgoing, 100, 2, walk),
                      GdiStatus::ok));
            auto expected = brute_force_bfs(filtered, hub->guid(), &seniors,
                                            EdgeOrientation::outgoing, 100, 2);
            expect(same_walk(walk, expected))
                << "a property-filtered 2-hop walk matches the brute force";
            expect(eq(walk.size(), size_t{1 + robot_leaves * 2}))
                << "hub + robot leaves + their fars; the person branch is gated";
        }
    };

    // Argument validation: the error surface is exact.
    "traverse_bfs_argument_errors"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // A label condition with an ordering op is error_op, validated up front.
        {
            Constraint bad;
            bad.conditions.push_back(
                ConstraintCondition::on_label(kPersonLabel, GdiOp::greater));
            kimix::vector<TraversalVertex> walk;
            expect(eq(graph.traverse_bfs(dag.a->guid(), bad, EdgeOrientation::outgoing,
                                         10, 2, walk),
                      GdiStatus::error_op));
            expect(walk.empty());
        }
        // An unknown START with a bad cap still reports the cap first
        // (validation order: caps, then constraint, then existence).
        {
            kimix::vector<TraversalVertex> walk;
            kimix::Guid unknown = kimix::Guid(true);
            expect(eq(graph.traverse_bfs(unknown, {}, EdgeOrientation::outgoing, 0, 1,
                                         walk),
                      GdiStatus::error_argument));
        }
    };

    // Determinism: the same traversal twice is byte-identical.
    "traverse_bfs_determinism"_test = [] {
        Graph graph;
        auto vertices = gtest::build_layered_dag(graph, 120, 4242u);
        kimix::Guid const start = vertices[7]->guid();
        kimix::vector<TraversalVertex> first;
        kimix::vector<TraversalVertex> second;
        expect(eq(graph.traverse_bfs(start, {}, EdgeOrientation::both, 9, 3, first),
                  GdiStatus::ok));
        expect(eq(graph.traverse_bfs(start, {}, EdgeOrientation::both, 9, 3, second),
                  GdiStatus::ok));
        expect(eq(first.size(), second.size()));
        for (size_t i = 0; i < first.size(); ++i) {
            expect(first[i].guid == second[i].guid && first[i].parent == second[i].parent &&
                   first[i].depth == second[i].depth)
                << "repeated traversals return the identical sequence";
        }
    };

    // The bounded shortest_path: equivalence with the unbounded form, the
    // bound's effect, and the error surface.
    "shortest_path_bounded"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // A bound at the true distance returns the same path as the unbounded form.
        {
            auto unbounded = graph.shortest_path(dag.a->guid(), dag.d->guid());
            expect(ge(unbounded.size(), size_t{2}));
            kimix::vector<kimix::Guid> bounded;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(),
                                          static_cast<uint32_t>(unbounded.size() - 1),
                                          bounded),
                      GdiStatus::ok));
            expect(eq(bounded.size(), unbounded.size()));
            for (size_t i = 0; i < bounded.size(); ++i) {
                expect(bounded[i] == unbounded[i]) << "bounded == unbounded within the bound";
            }
        }
        // A bound one below the true distance finds nothing (ok, empty).
        {
            auto unbounded = graph.shortest_path(dag.a->guid(), dag.d->guid());
            kimix::vector<kimix::Guid> bounded;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(),
                                          static_cast<uint32_t>(unbounded.size() - 2),
                                          bounded),
                      GdiStatus::ok));
            expect(bounded.empty()) << "beyond the bound is an empty result, not an error";
        }
        // from == to on an existing vertex is {from}; on an unknown one it is
        // error_vertex.
        {
            kimix::vector<kimix::Guid> path;
            expect(eq(graph.shortest_path(dag.b->guid(), dag.b->guid(), 3, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{1}));
            kimix::Guid unknown = kimix::Guid(true);
            expect(eq(graph.shortest_path(unknown, unknown, 3, path),
                      GdiStatus::error_vertex));
            expect(eq(graph.shortest_path(dag.a->guid(), unknown, 3, path),
                      GdiStatus::error_vertex));
        }
        // Depth bound validation.
        {
            kimix::vector<kimix::Guid> path;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), 0, path),
                      GdiStatus::error_argument));
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(),
                                          Graph::kMaxNativeHopDepth + 1, path),
                      GdiStatus::error_argument));
        }
        // On a cyclic graph the bounded form still matches the unbounded one.
        {
            Graph ring_graph;
            kimix::vector<Vertex *> ring;
            build_ring(ring_graph, 16, ring);
            auto unbounded = ring_graph.shortest_path(ring[0]->guid(), ring[7]->guid());
            kimix::vector<kimix::Guid> bounded;
            expect(eq(ring_graph.shortest_path(ring[0]->guid(), ring[7]->guid(),
                                               Graph::kMaxNativeHopDepth, bounded),
                      GdiStatus::ok));
            expect(eq(bounded.size(), unbounded.size()));
            for (size_t i = 0; i < bounded.size(); ++i) {
                expect(bounded[i] == unbounded[i]) << "cyclic graph: bounded == unbounded";
            }
        }
    };

    // -----------------------------------------------------------------------
    // EDGE-filtered shortest path (the edge-searching form): the walk keeps a hop
    // only when the EDGE it steps over matches, so the filter selects a ROUTE,
    // not a set of vertices. Equivalence against the brute force over the
    // constraint x orientation x depth matrix, then the contract details.
    // -----------------------------------------------------------------------
    "edge_filtered_shortest_path_matches_brute_force"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // Label every edge by its ORIGIN: b's and f's edges are `blocked`, the
        // rest `open`, and each carries a `cost` (blocked 9.0, open 1.0) so a
        // label filter and a property filter select the same routes from
        // different conditions.
        for (auto const &id : graph.edge_ids()) {
            EdgeRecord record;
            expect(graph.edge_record(id, record));
            bool const blocked = record.origin == dag.b->guid() || record.origin == dag.f->guid();
            expect(eq(graph.edge_add_label(id, blocked ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
            expect(eq(graph.edge_put_property(id, kCostName,
                                              PropertyValue::make_double(blocked ? 9.0 : 1.0)),
                      GdiStatus::ok));
        }
        kimix::vector<Constraint> constraints;
        constraints.push_back(Constraint{}); // no filter: every edge
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal)));
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal)));
        constraints.push_back(Constraint{}.with(ConstraintCondition::on_property(
            kCostName, GdiOp::less_or_equal, PropertyValue::make_double(5.0))));
        constraints.push_back(Constraint{}.with(ConstraintCondition::on_property(
            kCostName, GdiOp::greater, PropertyValue::make_double(5.0))));
        constraints.push_back(
            Constraint{}
                .with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal))
                .with(ConstraintCondition::on_property(
                    kCostName, GdiOp::greater, PropertyValue::make_double(5.0))));
        EdgeOrientation const orientations[] = {EdgeOrientation::outgoing,
                                                EdgeOrientation::incoming,
                                                EdgeOrientation::both};
        uint32_t const depths[] = {1u, 2u, 3u, Graph::kMaxNativeHopDepth};
        size_t compared = 0;
        for (auto const &constraint : constraints) {
            for (EdgeOrientation orientation : orientations) {
                for (uint32_t depth : depths) {
                    for (size_t s = 0; s < dag.vertices.size(); ++s) {
                        for (size_t t = 0; t < dag.vertices.size(); ++t) {
                            kimix::Guid const from = dag.vertices[s]->guid();
                            kimix::Guid const to = dag.vertices[t]->guid();
                            kimix::vector<PathStep> actual;
                            expect(eq(graph.shortest_path(from, to, constraint, orientation,
                                                          depth, actual),
                                      GdiStatus::ok));
                            RefPath const expected = ref_filtered_path(
                                graph, from, to, constraint, orientation, depth);
                            expect_same_path(actual, expected,
                                             "edge-filtered path matches the brute force");
                            if (!actual.empty()) {
                                expect_path_is_sound(graph, actual, constraint, orientation, from,
                                                     to, "every reported hop is real");
                            }
                            ++compared;
                        }
                    }
                }
            }
        }
        expect(gt(compared, size_t{1000})) << "the matrix really ran";
    };

    // The same equivalence at scale: a 120-vertex layered DAG with every edge
    // labeled and costed, and random endpoint / orientation / depth / filter
    // draws. A DAG is the shape that makes "no matching chain" the common answer,
    // which is exactly the case a length-only comparison would hide.
    "edge_filtered_shortest_path_layered"_test = [] {
        Graph graph;
        auto vertices = gtest::build_layered_dag(graph, 120, 4242u);
        // Label by the POSITIONS of the endpoints, not by edge_ids() order: that
        // order is shard order (a hash of a random guid), so keying off it would
        // make every run filter a different subset. This keeps the fixture
        // reproducible while still spreading the labels unevenly over the graph.
        kimix::unordered_map<kimix::Guid, size_t> rank;
        rank.reserve(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            rank.emplace(vertices[i]->guid(), i);
        }
        for (auto const &id : graph.edge_ids()) {
            EdgeRecord record;
            expect(graph.edge_record(id, record));
            bool const blocked =
                ((rank.find(record.origin)->second + rank.find(record.target)->second) % 3u) ==
                0u;
            expect(eq(graph.edge_add_label(id, blocked ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
            expect(eq(graph.edge_put_property(id, kCostName,
                                              PropertyValue::make_double(blocked ? 9.0 : 1.0)),
                      GdiStatus::ok));
        }
        kimix::vector<Constraint> constraints;
        constraints.push_back(Constraint{});
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal)));
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal)));
        constraints.push_back(Constraint{}.with(ConstraintCondition::on_property(
            kCostName, GdiOp::less, PropertyValue::make_double(5.0))));
        std::mt19937 rng(17u);
        size_t found = 0;
        for (int round = 0; round < 160; ++round) {
            kimix::Guid const from = vertices[rng() % vertices.size()]->guid();
            kimix::Guid const to = vertices[rng() % vertices.size()]->guid();
            Constraint const &constraint = constraints[rng() % constraints.size()];
            EdgeOrientation const orientation =
                static_cast<EdgeOrientation>(rng() % 3u);
            uint32_t const depth = 1 + static_cast<uint32_t>(rng() % 6);
            kimix::vector<PathStep> actual;
            expect(eq(graph.shortest_path(from, to, constraint, orientation, depth, actual),
                      GdiStatus::ok));
            expect_same_path(actual,
                             ref_filtered_path(graph, from, to, constraint, orientation, depth),
                             "layered DAG: the edge-filtered path matches the brute force");
            if (!actual.empty()) {
                ++found;
                expect_path_is_sound(graph, actual, constraint, orientation, from, to,
                                     "every reported hop is real");
            }
        }
        expect(gt(found, size_t{0})) << "the draws did produce reachable pairs";
    };

    // The contract: which edge a hop reports (parallel edges included), the
    // zero-length path, the unreachable answer and the error surface.
    "edge_filtered_shortest_path_contract"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // a -> b already exists (connect); add a PARALLEL a -> b that only the
        // filter can pick, which is the case a vertex-only path cannot express.
        EdgeId const first = [&] {
            kimix::vector<EdgeId> ids;
            graph.incident_edges(dag.a->guid(), EdgeOrientation::outgoing, ids);
            expect(eq(ids.size(), size_t{2})); // a -> b, a -> c
            return ids[0];
        }();
        EdgeId parallel;
        LabelId const open_labels[]{kOpenEdgeLabel};
        expect(eq(graph.edge_create(dag.a->guid(), dag.b->guid(),
                                    graph_db::EdgeDirectionType::directed,
                                    kimix::span<const LabelId>{open_labels, 1}, nullptr, parallel),
                  GdiStatus::ok));
        expect(parallel != first) << "a parallel edge is its own id";
        expect(eq(graph.edge_add_label(first, kBlockedEdgeLabel), GdiStatus::ok));

        // Unfiltered: the FIRST a -> b edge in link order carries the hop.
        {
            kimix::vector<PathStep> path;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.b->guid(), Constraint{},
                                          EdgeOrientation::outgoing, 4, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{2}));
            expect(path[1].edge == first) << "the unfiltered walk reports the first parallel edge";
        }
        // Refusing that edge reroutes the SAME hop onto the parallel one.
        {
            Constraint open_only =
                Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
            kimix::vector<PathStep> path;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.b->guid(), open_only,
                                          EdgeOrientation::outgoing, 4, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{2}));
            expect(path[1].edge == parallel)
                << "the filter selects WHICH parallel edge carried the hop";
        }
        // A zero-length path traverses no edge, so no edge filter can exclude it.
        {
            Constraint blocked =
                Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal));
            kimix::vector<PathStep> path;
            expect(eq(graph.shortest_path(dag.b->guid(), dag.b->guid(), blocked,
                                          EdgeOrientation::outgoing, 3, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{1}));
            expect(!static_cast<bool>(path[0].edge)) << "the start is reached by no edge";
        }
        // Unreachable within the bound, and unreachable over the matched edges:
        // both are an empty result with status ok (GDI's nullable-list answer).
        {
            Constraint blocked =
                Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal));
            kimix::vector<PathStep> path;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), Constraint{},
                                          EdgeOrientation::outgoing, 1, path),
                      GdiStatus::ok));
            expect(path.empty()) << "a -> d needs two hops, the bound is one";
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), blocked,
                                          EdgeOrientation::outgoing, 8, path),
                      GdiStatus::ok));
            expect(path.empty())
                << "a -> b is blocked, but b -> d carries no label, so d stays unreachable";
        }
        // Error surface: bound, unknown endpoints, and the label-op rule.
        {
            kimix::vector<PathStep> path;
            kimix::Guid const unknown(true);
            Constraint const none;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), none,
                                          EdgeOrientation::outgoing, 0, path),
                      GdiStatus::error_argument));
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), none,
                                          EdgeOrientation::outgoing,
                                          Graph::kMaxNativeHopDepth + 1, path),
                      GdiStatus::error_argument));
            expect(eq(graph.shortest_path(unknown, dag.d->guid(), none, EdgeOrientation::outgoing,
                                          4, path),
                      GdiStatus::error_vertex));
            expect(eq(graph.shortest_path(dag.a->guid(), unknown, none, EdgeOrientation::outgoing,
                                          4, path),
                      GdiStatus::error_vertex));
            Constraint ordering =
                Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::greater));
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), ordering,
                                          EdgeOrientation::outgoing, 4, path),
                      GdiStatus::error_op))
                << "a label condition can only test membership";
        }
        // A self loop is a hop in both orientations but never shortens a path.
        {
            Vertex *loop = graph.create_vertex();
            kimix::vector<EdgeId> ids;
            graph.incident_edges(loop->guid(), EdgeOrientation::both, ids);
            expect(ids.empty());
            EdgeId loop_edge;
            LabelId const open_labels2[]{kOpenEdgeLabel};
            expect(eq(graph.edge_create(loop->guid(), loop->guid(),
                                        graph_db::EdgeDirectionType::directed,
                                        kimix::span<const LabelId>{open_labels2, 1}, nullptr,
                                        loop_edge),
                      GdiStatus::ok));
            kimix::vector<PathStep> path;
            Constraint open_only =
                Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
            expect(eq(graph.shortest_path(loop->guid(), loop->guid(), open_only,
                                          EdgeOrientation::outgoing, 4, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{1})) << "from == to stays the zero-length path";
        }
    };

    // The two primitives the walk is built on, plus the edge-filtered
    // reachability probe.
    "edge_filtered_primitives"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // oriented_links: (edge, neighbour) pairs, one shard snapshot per call.
        kimix::vector<EdgeLink> links;
        expect(graph.oriented_links(dag.a->guid(), EdgeOrientation::outgoing, links));
        expect(eq(links.size(), size_t{2}));
        expect(links[0].neighbour == dag.b->guid()) << "insertion order: a -> b first";
        expect(links[1].neighbour == dag.c->guid());
        EdgeRecord record;
        expect(graph.edge_record(links[0].edge, record));
        expect(record.origin == dag.a->guid() && record.target == dag.b->guid())
            << "the link carries the EDGE that makes the hop";
        expect(graph.oriented_links(dag.a->guid(), EdgeOrientation::incoming, links));
        expect(links.empty()) << "a has no incoming edge";
        expect(graph.oriented_links(dag.d->guid(), EdgeOrientation::both, links));
        expect(eq(links.size(), size_t{3})) << "b -> d, c -> d and f -> d are all incident";
        kimix::Guid const unknown(true);
        expect(!graph.oriented_links(unknown, EdgeOrientation::both, links));
        expect(links.empty()) << "a failed read leaves the buffer empty, not stale";
        // edge_matches: an empty constraint is "the edge exists", a label filter
        // reads the EDGE's own payload.
        kimix::vector<EdgeId> a_out;
        graph.incident_edges(dag.a->guid(), EdgeOrientation::outgoing, a_out);
        expect(eq(graph.edge_add_label(a_out[1], kBlockedEdgeLabel), GdiStatus::ok));
        expect(graph.edge_matches(a_out[0], Constraint{}));
        expect(!graph.edge_matches(unknown, Constraint{}))
            << "an unknown edge does not match, not even the empty constraint";
        Constraint blocked =
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal));
        expect(graph.edge_matches(a_out[1], blocked));
        expect(!graph.edge_matches(a_out[0], blocked));
        // has_path: the reachability probe honours the same filter.
        expect(graph.has_path(dag.a->guid(), dag.d->guid(), Constraint{},
                              EdgeOrientation::outgoing))
            << "a -> b -> d exists over every edge";
        expect(!graph.has_path(dag.a->guid(), dag.d->guid(), blocked, EdgeOrientation::outgoing))
            << "a -> c matches but c -> d does not, so d is unreachable over blocked edges";
        expect(graph.has_path(dag.a->guid(), dag.c->guid(), blocked, EdgeOrientation::outgoing))
            << "the blocked edge itself is reachable";
        expect(!graph.has_path(unknown, dag.d->guid(), Constraint{}, EdgeOrientation::outgoing))
            << "an unknown endpoint is unreachable, not an error";
        expect(graph.has_path(dag.b->guid(), dag.b->guid(), blocked, EdgeOrientation::outgoing))
            << "from == to on an existing vertex is a zero-length path";
        expect(!graph.has_path(unknown, unknown, Constraint{}, EdgeOrientation::outgoing));
    };

    // Database facade: the edge filter is a registered constraint like every
    // other read filter, so the facade must resolve it, forward the same status
    // codes and agree with the raw Graph API.
    // Database facade: the edge filter is a registered constraint like every
    // other read filter, so the facade must resolve it, forward the same status
    // codes and agree with the raw Graph API.
    "database_edge_filtered_shortest_path"_test = [] {
        Database db;
        auto dag = gtest::small_dag(db.graph());
        // Every edge gets a label, and the b -> d hinge gets the blocking one, so
        // the two-hop route a -> c -> d is the only one an open-only filter
        // leaves (a -> b would be unlabeled and therefore unmatched).
        for (auto const &id : db.graph().edge_ids()) {
            EdgeRecord record;
            expect(db.graph().edge_record(id, record));
            bool const hinge = record.origin == dag.b->guid() && record.target == dag.d->guid();
            expect(eq(db.graph().edge_add_label(
                          id, hinge ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
        }
        Constraint const open_only =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
        kimix::vector<PathStep> via_facade;
        expect(eq(db.shortest_path(dag.a->guid(), dag.d->guid(), open_only,
                                   EdgeOrientation::outgoing, 8, via_facade),
                  GdiStatus::ok));
        expect(eq(via_facade.size(), size_t{3})) << "a -> c -> d";
        if (via_facade.size() == 3) {
            expect(via_facade[1].vertex == dag.c->guid() && via_facade[2].vertex == dag.d->guid())
                << "the blocked hinge is routed around";
            expect(via_facade[1].edge != via_facade[2].edge)
                << "the two hops are two distinct edges";
        }
        // The facade agrees with the raw Graph API on the same definition.
        kimix::vector<PathStep> via_graph;
        expect(eq(db.graph().shortest_path(dag.a->guid(), dag.d->guid(), open_only,
                                           EdgeOrientation::outgoing, 8, via_graph),
                  GdiStatus::ok));
        expect(eq(via_graph.size(), via_facade.size()));
        for (size_t i = 0; i < via_graph.size() && i < via_facade.size(); ++i) {
            expect(via_graph[i].vertex == via_facade[i].vertex &&
                   via_graph[i].edge == via_facade[i].edge)
                << "the facade forwards the Graph result verbatim";
        }
        // A registered HANDLE resolves to the same definition (the C ABI shape).
        graph_db::ConstraintHandle handle{};
        expect(eq(db.create_constraint(open_only, handle), GdiStatus::ok));
        Constraint resolved;
        expect(db.schema().constraint(handle, resolved));
        kimix::vector<PathStep> via_handle;
        expect(eq(db.shortest_path(dag.a->guid(), dag.d->guid(), resolved,
                                   EdgeOrientation::outgoing, 8, via_handle),
                  GdiStatus::ok));
        expect(eq(via_handle.size(), via_facade.size()));
        // The facade validates the definition BEFORE walking (_resolve_constraint).
        Constraint const ordering =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::less));
        kimix::vector<PathStep> rejected;
        expect(eq(db.shortest_path(dag.a->guid(), dag.d->guid(), ordering,
                                   EdgeOrientation::outgoing, 8, rejected),
                  GdiStatus::error_op));
        expect(rejected.empty());
        // Reachability through the facade, over matching edges only.
        bool reachable = true;
        expect(eq(db.has_path(dag.a->guid(), dag.d->guid(), open_only,
                              EdgeOrientation::outgoing, reachable),
                  GdiStatus::ok));
        expect(reachable);
        Constraint const blocked =
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal));
        expect(eq(db.has_path(dag.a->guid(), dag.d->guid(), blocked,
                              EdgeOrientation::outgoing, reachable),
                  GdiStatus::ok));
        expect(!reachable) << "a -> b is open, so the blocked-only walk leaves a and dies at b";
        expect(eq(db.has_path(dag.a->guid(), dag.d->guid(), ordering,
                              EdgeOrientation::outgoing, reachable),
                  GdiStatus::error_op))
            << "the reachability probe validates the definition too";
    };

    // 4 readers traverse (filtered and unfiltered) while one writer mutates a
    // disjoint tail region: no crash, invariants hold on every observed walk,
    // and a post-run quiescent traversal matches the brute force.
    // -----------------------------------------------------------------------
    // Database facade: the GDI-shaped entry points resolve a registered
    // constraint handle exactly like the read commands, forward the exact
    // status codes, and agree with the raw Graph API.
    // -----------------------------------------------------------------------
    "database_traverse_bfs_facade"_test = [] {
        Database db;
        LabelId person{};
        PropNameId age{};
        expect(eq(db.create_label("person", person), GdiStatus::ok));
        graph_db::PropertyTypeInfo age_info;
        age_info.name = "age";
        age_info.data = graph_db::GdiDataType::int64_;
        age_info.count = 1;
        age_info.container = graph_db::GdiContainerType::single;
        expect(eq(db.create_property_type(age_info, age), GdiStatus::ok));

        // hub -> leaves (alternating ages), each leaf -> one far vertex.
        kimix::vector<std::byte> hub_key = gtest::to_bytes("hub");
        graph_db::VertexCreateResult hub;
        expect(eq(db.create_vertex(graph_db::VertexKeyView{
                      person, kimix::span<const std::byte>{hub_key.data(), hub_key.size()}},
                                  {}, nullptr, hub),
                  GdiStatus::ok));
        kimix::vector<kimix::Guid> leaves;
        kimix::vector<kimix::Guid> fars;
        for (size_t i = 0; i < 8; ++i) {
            kimix::vector<std::byte> key = gtest::to_bytes(kimix::format("leaf-{}", i));
            graph_db::PropertyList props;
            props.push_back({age, PropertyValue::make_int64(i % 2 == 0 ? 70 : 20)});
            graph_db::LabelId labels[]{person};
            graph_db::VertexCreateResult created;
            expect(eq(db.create_vertex(graph_db::VertexKeyView{
                          person, kimix::span<const std::byte>{key.data(), key.size()}},
                                      kimix::span<const graph_db::LabelId>{labels, 1}, &props,
                                      created),
                      GdiStatus::ok));
            leaves.push_back(created.uid);
            kimix::Guid edge;
            expect(eq(db.edge_create(hub.uid, created.uid, graph_db::EdgeDirectionType::directed,
                                     {}, nullptr, edge),
                      GdiStatus::ok));
            kimix::vector<std::byte> far_key = gtest::to_bytes(kimix::format("far-{}", i));
            graph_db::VertexCreateResult far_created;
            expect(eq(db.create_vertex(graph_db::VertexKeyView{
                          person, kimix::span<const std::byte>{far_key.data(), far_key.size()}},
                                      {}, nullptr, far_created),
                      GdiStatus::ok));
            fars.push_back(far_created.uid);
            expect(eq(db.edge_create(created.uid, far_created.uid,
                                     graph_db::EdgeDirectionType::directed, {}, nullptr, edge),
                      GdiStatus::ok));
        }

        // Unfiltered facade traversal == the raw Graph API.
        {
            kimix::vector<TraversalVertex> facade;
            expect(eq(db.traverse_bfs(hub.uid, {}, EdgeOrientation::outgoing, 100, 2, facade),
                      GdiStatus::ok));
            kimix::vector<TraversalVertex> raw;
            expect(eq(db.graph().traverse_bfs(hub.uid, {}, EdgeOrientation::outgoing, 100, 2,
                                              raw),
                      GdiStatus::ok));
            expect(eq(facade.size(), raw.size()));
            for (size_t i = 0; i < facade.size(); ++i) {
                expect(facade[i].guid == raw[i].guid && facade[i].parent == raw[i].parent &&
                       facade[i].depth == raw[i].depth)
                    << "the facade forwards the Graph result verbatim";
            }
            expect(eq(facade.size(), size_t{1 + 8 + 8}));
        }
        // Registered-handle constraint (the C ABI's shape): seniors only.
        {
            Constraint definition;
            definition.conditions.push_back(ConstraintCondition::on_property(
                age, GdiOp::greater_or_equal, PropertyValue::make_int64(60)));
            graph_db::ConstraintHandle handle{};
            expect(eq(db.create_constraint(definition, handle), GdiStatus::ok));
            expect(handle.valid());
            Constraint resolved;
            expect(db.schema().constraint(handle, resolved));
            kimix::vector<TraversalVertex> walk;
            expect(eq(db.traverse_bfs(hub.uid, resolved, EdgeOrientation::outgoing, 100, 2,
                                      walk),
                      GdiStatus::ok));
            expect(eq(walk.size(), size_t{1 + 4}))
                << "the handle-resolved filter gates the discovery";
        }
        // Facade error surface: unknown start, bad cap, bad bound.
        {
            kimix::vector<TraversalVertex> walk;
            kimix::Guid unknown(true);
            expect(eq(db.traverse_bfs(unknown, {}, EdgeOrientation::outgoing, 10, 2, walk),
                      GdiStatus::error_vertex));
            expect(eq(db.traverse_bfs(hub.uid, {}, EdgeOrientation::outgoing, 0, 2, walk),
                      GdiStatus::error_argument));
            kimix::vector<kimix::Guid> path;
            expect(eq(db.shortest_path(hub.uid, leaves[0], 0, path),
                      GdiStatus::error_argument));
            expect(eq(db.shortest_path(unknown, leaves[0], 2, path),
                      GdiStatus::error_vertex));
            expect(eq(db.shortest_path(hub.uid, fars[0], 4, path), GdiStatus::ok));
            expect(eq(path.size(), size_t{3})) << "hub -> leaf -> far";
        }
    };

    "traverse_bfs_reader_writer_stress"_test = [] {
        Graph graph;
        auto core = gtest::build_layered_dag(graph, 400, 5150u);
        // Label the core so the filtered readers have something to match.
        for (auto *vertex : core) {
            vertex->add_label(kPersonLabel);
            vertex->put_property(kAgeName, PropertyValue::make_int64(42));
        }
        Constraint persons;
        persons.conditions.push_back(
            ConstraintCondition::on_label(kPersonLabel, GdiOp::equal));

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> walks{0};
        std::atomic<uint64_t> invariant_violations{0};

        // The writer owns a disjoint vertex set it creates itself (the
        // documented reset: the bench thread built the core).
        std::thread writer([&] {
            graph.reset_writer_thread();
            kimix::vector<Vertex *> mine;
            uint64_t round = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                Vertex *vertex = graph.create_vertex();
                if (vertex == nullptr) {
                    break;
                }
                vertex->add_label(kRobotLabel);
                mine.push_back(vertex);
                if (mine.size() > 1) {
                    graph.connect(mine[mine.size() - 2]->guid(), vertex->guid());
                }
                // Cascade-remove every other batch from the tail: exercises the
                // concurrent-cascade memory-safety argument mid-traversal.
                if (mine.size() >= 64) {
                    if ((round++ % 2u) == 0u) {
                        for (auto *v : mine) {
                            graph.remove_vertex(v->guid());
                        }
                        mine.clear();
                    }
                }
            }
        });

        auto reader = [&] {
            std::mt19937 rng(static_cast<unsigned>(std::hash<std::thread::id>{}(
                                 std::this_thread::get_id())));
            while (!stop.load(std::memory_order_relaxed)) {
                kimix::Guid const start =
                    core[rng() % core.size()]->guid();
                kimix::vector<TraversalVertex> walk;
                GdiStatus const status =
                    rng() % 2u
                        ? graph.traverse_bfs(start, {}, EdgeOrientation::outgoing, 12, 3,
                                             walk)
                        : graph.traverse_bfs(start, persons, EdgeOrientation::both, 12, 2,
                                             walk);
                if (status != GdiStatus::ok) {
                    continue; // the start may have been removed? core is never removed
                }
                if (!walk_is_consistent(walk, start, 12)) {
                    invariant_violations.fetch_add(1, std::memory_order_relaxed);
                }
                walks.fetch_add(1, std::memory_order_relaxed);
            }
        };
        kimix::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back(reader);
        }
        // Let the mix run briefly.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        stop.store(true, std::memory_order_relaxed);
        for (auto &thread : readers) {
            thread.join();
        }
        writer.join();

        expect(gt(walks.load(), uint64_t{0})) << "the readers did work";
        expect(eq(invariant_violations.load(), uint64_t{0}))
            << "no observed walk violated the BFS invariants under concurrent writes";

        // Quiescent: the traversal matches the brute force on the final state.
        kimix::vector<TraversalVertex> walk;
        expect(eq(graph.traverse_bfs(core[0]->guid(), {}, EdgeOrientation::outgoing, 16,
                                     3, walk),
                  GdiStatus::ok));
        auto expected = brute_force_bfs(graph, core[0]->guid(), nullptr,
                                        EdgeOrientation::outgoing, 16, 3);
        expect(same_walk(walk, expected)) << "post-stress traversal matches the brute force";
    };
}
