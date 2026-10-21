// Test for the C ABI (src/graph_db/graph_c_api.h) — GDI §2.13.
//
// This suite includes ONLY the C header (like tests/unit/api/test_kimix_api.cpp):
// it is what a Pentest-style foreign-language binding sees, so nothing of the C++
// surface is allowed to leak into it.
//
// This test covers:
// - the ABI/layout contract: abi_version matches the macro, every struct size is
//   pinned, sizeof(size_t) is the pointer width;
// - the session lifecycle: open/close, a second close, NULL-argument rejection;
// - status names: every code round-trips, and an unknown one answers
//   "status_unknown" instead of NULL;
// - a full command walk: schema -> keyed vertices -> edges -> labels/properties ->
//   constraint-filtered paged reads through the TWO-PHASE buffer protocol (NULL
//   first to learn the size, then the real fetch, then a deliberately too-small
//   buffer that must report ERR_BUFFER with the needed size) -> a transaction
//   abort and commit -> persist -> reopen and read the same data back;
// - a create/free loop, to prove the session owns no per-call allocation it
//   forgets (the only owned object is the session itself).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

// The ONLY include from the graph_db library: the C header.
#include <graph_db/graph_c_api.h>

#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
namespace gtest = kimix::graph_db::test;

namespace {

// Small helpers that make the C surface readable in a test.
kimix_gdb_bytes bytes_of(char const *text) {
    kimix_gdb_bytes out;
    out.data = reinterpret_cast<uint8_t const *>(text);
    out.len = std::strlen(text);
    return out;
}

kimix_gdb_property int_property(int64_t value) {
    kimix_gdb_property property{};
    property.kind = KIMIX_GDB_KIND_INT64;
    property.dtype = KIMIX_GDB_DTYPE_INT64;
    property.count = 1;
    property.i64 = value;
    return property;
}

kimix_gdb_property text_property(char const *text) {
    kimix_gdb_property property{};
    property.kind = KIMIX_GDB_KIND_STRING;
    property.dtype = KIMIX_GDB_DTYPE_CHAR;
    property.count = 1;
    property.payload = bytes_of(text);
    return property;
}

// One open in-memory session; the guard removes it even on an early return.
struct session_guard {
    kimix_gdb_session *session{nullptr};

    session_guard() {
        kimix_gdb_open_options options{};
        options.directory = nullptr; // in memory
        options.auto_create_schema = 1;
        options.allow_parallel_edges = 1;
        options.allow_self_edges = 1;
        options.max_journal_bytes = 64ull * 1024ull * 1024ull;
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
    }
    session_guard(session_guard const &) = delete;
    session_guard &operator=(session_guard const &) = delete;
    ~session_guard() {
        if (session != nullptr) {
            kimix_gdb_close(session);
        }
    }

    // A registered label by name.
    uint32_t label(char const *name) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_label_create(session, name, std::strlen(name), &id), KIMIX_GDB_OK));
        return id;
    }

    uint32_t property_type(char const *name, uint8_t dtype) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_property_type_create(session, name, std::strlen(name),
                                                 KIMIX_GDB_CONTAINER_SINGLE, dtype, 1, &id),
                  KIMIX_GDB_OK));
        return id;
    }

    kimix_gdb_guid vertex(uint32_t label_id, char const *external) {
        kimix_gdb_vertex_spec spec{};
        spec.key.label = label_id;
        spec.key.external_id = bytes_of(external);
        kimix_gdb_guid uid{};
        uint8_t warning = 0;
        expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, &warning), KIMIX_GDB_OK));
        return uid;
    }

    kimix_gdb_guid edge(kimix_gdb_guid origin, kimix_gdb_guid target) {
        kimix_gdb_guid id{};
        expect(eq(kimix_gdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                        0, nullptr, 0, &id),
                  KIMIX_GDB_OK));
        return id;
    }
};

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "cabi_version_and_layout"_test = [] {
        expect(eq(kimix_gdb_abi_version(), KIMIX_GDB_ABI_VERSION));
        expect(kimix_gdb_version_string() != nullptr);
        expect(std::strlen(kimix_gdb_version_string()) > 0);

        kimix_gdb_layout layout{};
        expect(eq(kimix_gdb_layout_info(&layout), KIMIX_GDB_OK));
        expect(eq(layout.abi_version, KIMIX_GDB_ABI_VERSION));
        expect(eq(layout.sizeof_property, static_cast<uint32_t>(sizeof(kimix_gdb_property))));
        expect(eq(layout.sizeof_property_record,
                  static_cast<uint32_t>(sizeof(kimix_gdb_property_record))));
        expect(eq(layout.sizeof_vertex_key, static_cast<uint32_t>(sizeof(kimix_gdb_vertex_key))));
        expect(eq(layout.sizeof_edge_record,
                  static_cast<uint32_t>(sizeof(kimix_gdb_edge_record))));
        expect(eq(layout.sizeof_property_type,
                  static_cast<uint32_t>(sizeof(kimix_gdb_property_type))));
        expect(eq(layout.sizeof_capability, static_cast<uint32_t>(sizeof(kimix_gdb_capability))));
        expect(eq(layout.sizeof_vertex_spec,
                  static_cast<uint32_t>(sizeof(kimix_gdb_vertex_spec))));
        expect(eq(layout.sizeof_size_t, static_cast<uint32_t>(sizeof(size_t))));
        expect(eq(layout.alignof_guid, static_cast<uint32_t>(alignof(kimix_gdb_guid))));
        expect(eq(layout.sizeof_size_t, uint32_t{8})) << "the ABI is 64-bit only";

        // NULL out-parameters are argument errors, never crashes.
        expect(eq(kimix_gdb_layout_info(nullptr), KIMIX_GDB_ERR_ARGUMENT));
    };

    "cabi_status_names"_test = [] {
        // Every code the header defines round-trips through status_name.
        int32_t const codes[] = {KIMIX_GDB_OK,
                                 KIMIX_GDB_WARNING_NON_UNIQUE_ID,
                                 KIMIX_GDB_WARNING_OTHER,
                                 KIMIX_GDB_ERR_ARGUMENT,
                                 KIMIX_GDB_ERR_BUFFER,
                                 KIMIX_GDB_ERR_EDGE,
                                 KIMIX_GDB_ERR_EDGE_ORIENTATION,
                                 KIMIX_GDB_ERR_EMPTY_NAME,
                                 KIMIX_GDB_ERR_LABEL,
                                 KIMIX_GDB_ERR_NAME_EXISTS,
                                 KIMIX_GDB_ERR_NO_PROPERTY,
                                 KIMIX_GDB_ERR_NON_UNIQUE_ID,
                                 KIMIX_GDB_ERR_OP,
                                 KIMIX_GDB_ERR_PROPERTY_EXISTS,
                                 KIMIX_GDB_ERR_PROPERTY_TYPE,
                                 KIMIX_GDB_ERR_READ_ONLY_TRANSACTION,
                                 KIMIX_GDB_ERR_SIZE_LIMIT,
                                 KIMIX_GDB_ERR_TRANSACTION,
                                 KIMIX_GDB_ERR_UID,
                                 KIMIX_GDB_ERR_VERTEX,
                                 KIMIX_GDB_ERR_WRONG_TYPE,
                                 KIMIX_GDB_ERR_IO,
                                 KIMIX_GDB_ERR_UNKNOWN};
        for (auto code : codes) {
            char const *name = kimix_gdb_status_name(code);
            expect(name != nullptr);
            expect(std::strlen(name) > 0) << "code " << code;
        }
        expect(std::strcmp(kimix_gdb_status_name(KIMIX_GDB_OK), "ok") == 0);
        expect(std::strcmp(kimix_gdb_status_name(KIMIX_GDB_ERR_VERTEX), "error_vertex") == 0);
        expect(std::strcmp(kimix_gdb_status_name(999), "status_unknown") == 0);
    };

    "cabi_session_lifecycle"_test = [] {
        kimix_gdb_session *session = nullptr;
        kimix_gdb_open_options options{};
        options.max_journal_bytes = 1024 * 1024;
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
        expect(session != nullptr);

        kimix_gdb_capability capability{};
        expect(eq(kimix_gdb_capabilities(session, &capability), KIMIX_GDB_OK));
        expect(capability.enforces_unique_external_ids == 1);
        expect(capability.supports_edge_by_stable_uid == 1);
        expect(capability.supports_edge_property_read == 1);
        expect(capability.supports_collective_transactions == 1);
        expect(capability.supports_global_scan == 0) << "the phase-2 gap is still honest";
        expect(capability.supports_native_multi_hop == 0);
        expect(capability.supports_global_aggregates == 0);

        kimix_gdb_close(session);
        // A NULL session is tolerated by close() and rejected by everything else.
        kimix_gdb_close(nullptr);
        kimix_gdb_guid uid{};
        expect(eq(kimix_gdb_vertex_free(nullptr, uid), KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_capabilities(nullptr, &capability), KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_open(nullptr, nullptr), KIMIX_GDB_ERR_ARGUMENT));
    };

    "cabi_command_walk"_test = [] {
        session_guard guard;
        kimix_gdb_session *session = guard.session;
        uint32_t const node = guard.label("node");
        uint32_t const link = guard.label("link");
        uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);

        kimix_gdb_guid const a = guard.vertex(node, "a");
        kimix_gdb_guid const b = guard.vertex(node, "b");
        kimix_gdb_guid const c = guard.vertex(node, "c");

        // Key -> uid translation.
        kimix_gdb_vertex_key key{};
        key.label = node;
        key.external_id = bytes_of("b");
        kimix_gdb_guid resolved{};
        uint8_t found = 0;
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 1);
        expect(resolved.lo == b.lo && resolved.hi == b.hi);
        key.external_id = bytes_of("nobody");
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 0) << "a miss is not an error";

        // Edges, a label and an int property.
        kimix_gdb_guid const ab = guard.edge(a, b);
        kimix_gdb_guid const ac = guard.edge(a, c);
        kimix_gdb_property const value = int_property(42);
        expect(eq(kimix_gdb_vertex_property_add(session, a, age, &value), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_vertex_property_add(session, a, age, &value),
                  KIMIX_GDB_ERR_PROPERTY_EXISTS));
        expect(eq(kimix_gdb_vertex_label_add(session, a, link), KIMIX_GDB_OK));

        // Property read-back, TWO-PHASE: the size probe first.
        size_t count = 0;
        size_t scratch_bytes = 0;
        expect(eq(kimix_gdb_vertex_properties(session, a, nullptr, 0, nullptr, 0, &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        kimix::vector<kimix_gdb_property_record> items(count + 1);
        kimix::vector<uint8_t> scratch(scratch_bytes + 8);
        expect(eq(kimix_gdb_vertex_properties(session, a, items.data(), items.size(),
                                              scratch.data(), scratch.size(), &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        expect(eq(items[0].name_id, age));
        expect(items[0].kind == KIMIX_GDB_KIND_INT64);
        expect(eq(items[0].i64, int64_t{42}));

        // A deliberately too-small item array reports the need, not a crash.
        expect(eq(kimix_gdb_vertex_properties(session, a, nullptr, 0, nullptr, 0, &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));

        // Labels: the two-phase shape, with the key label first.
        size_t label_count = 0;
        expect(eq(kimix_gdb_vertex_labels(session, a, nullptr, 0, &label_count), KIMIX_GDB_OK));
        expect(eq(label_count, size_t{2})) << "the key label `node` and the added `link`";
        kimix::vector<uint32_t> labels(label_count);
        expect(eq(kimix_gdb_vertex_labels(session, a, labels.data(), labels.size(), &label_count),
                  KIMIX_GDB_OK));
        expect(eq(labels[0], node)) << "the key label comes first";

        // Edge read: count probe, then a page of two.
        size_t total = 0;
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0,
                                            nullptr, 0, &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(total, size_t{2}));
        kimix::vector<kimix_gdb_edge_record> edges(2);
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 2,
                                            edges.data(), edges.size(), &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{2}));
        expect(eq(total, size_t{2}));
        for (auto const &record : edges) {
            expect(record.origin.lo == a.lo && record.origin.hi == a.hi);
        }
        // cap 1 of 2: one item, still the full total.
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1,
                                            edges.data(), edges.size(), &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        expect(eq(total, size_t{2}));

        // A constraint that filters the NEIGHBOUR VERTICES by property.
        uint32_t constraint = 0;
        expect(eq(kimix_gdb_constraint_create(session, &constraint), KIMIX_GDB_OK));
        kimix_gdb_property const expected = int_property(42);
        expect(eq(kimix_gdb_constraint_add_property(session, constraint, age,
                                                    KIMIX_GDB_OP_EQUAL, &expected, 1),
                  KIMIX_GDB_OK));
        expect(eq(kimix_gdb_count_neighbors(session, a, constraint, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            &count),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{0})) << "b and c carry no `age` property";
        uint8_t exists = 1;
        expect(eq(kimix_gdb_exists_neighbor(session, a, constraint,
                                            KIMIX_GDB_ORIENTATION_OUTGOING, &exists),
                  KIMIX_GDB_OK));
        expect(exists == 0);
        expect(eq(kimix_gdb_constraint_free(session, constraint), KIMIX_GDB_OK));

        // Endpoints of one edge.
        kimix_gdb_guid origin{};
        kimix_gdb_guid target{};
        expect(eq(kimix_gdb_edge_endpoints(session, ab, &origin, &target), KIMIX_GDB_OK));
        expect(origin.lo == a.lo);
        expect(target.lo == b.lo);
        expect(eq(kimix_gdb_edge_free(session, ac), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_edge_free(session, ac), KIMIX_GDB_ERR_UID));

        // Guarded delete: `a` still has the ab edge.
        expect(eq(kimix_gdb_vertex_free_guarded(session, a), KIMIX_GDB_ERR_EDGE));
        expect(eq(kimix_gdb_edge_free(session, ab), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_vertex_free_guarded(session, a), KIMIX_GDB_OK));
    };

    "cabi_transaction_and_persistence"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        kimix::string const path{dir.path().string().c_str()};
        kimix_gdb_guid kept{};
        kimix_gdb_guid dropped{};
        {
            kimix_gdb_session *session = nullptr;
            kimix_gdb_open_options options{};
            options.directory = path.c_str();
            options.max_journal_bytes = 1024 * 1024;
            expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
            uint32_t label = 0;
            expect(eq(kimix_gdb_label_create(session, "node", 4, &label), KIMIX_GDB_OK));

            // An aborted transaction leaves nothing behind...
            expect(eq(kimix_gdb_tx_begin(session), KIMIX_GDB_OK));
            kimix_gdb_vertex_spec spec{};
            spec.key.label = label;
            spec.key.external_id = bytes_of("dropped");
            expect(eq(kimix_gdb_vertex_create(session, &spec, &dropped, nullptr), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_tx_abort(session), KIMIX_GDB_OK));
            kimix_gdb_vertex_key key{};
            key.label = label;
            key.external_id = bytes_of("dropped");
            uint8_t found = 1;
            expect(eq(kimix_gdb_vertex_translate(session, &key, &dropped, &found), KIMIX_GDB_OK));
            expect(found == 0) << "the aborted create is gone";

            // ...and a committed one survives a save.
            expect(eq(kimix_gdb_tx_begin(session), KIMIX_GDB_OK));
            spec.key.external_id = bytes_of("kept");
            expect(eq(kimix_gdb_vertex_create(session, &spec, &kept, nullptr), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_tx_commit(session), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_persist(session, 1), KIMIX_GDB_OK));
            kimix_gdb_close(session);
        }
        // Reopen: the committed vertex is in the store, the aborted one is not.
        kimix_gdb_session *session = nullptr;
        kimix_gdb_open_options options{};
        options.directory = path.c_str();
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
        uint32_t label = 0;
        uint8_t found = 0;
        expect(eq(kimix_gdb_label_find(session, "node", 4, &label, &found), KIMIX_GDB_OK));
        expect(found == 1) << "the schema survived the reopen";
        kimix_gdb_vertex_key key{};
        key.label = label;
        key.external_id = bytes_of("kept");
        kimix_gdb_guid resolved{};
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 1);
        expect(resolved.lo == kept.lo && resolved.hi == kept.hi);
        key.external_id = bytes_of("dropped");
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 0);
        kimix_gdb_close(session);
    };

    "cabi_churn_leaves_no_state"_test = [] {
        session_guard guard;
        kimix_gdb_session *session = guard.session;
        uint32_t const node = guard.label("node");
        for (int i = 0; i < 200; ++i) {
            kimix::string const name{kimix::format("churn{}", i)};
            kimix_gdb_vertex_spec spec{};
            spec.key.label = node;
            spec.key.external_id = kimix_gdb_bytes{
                reinterpret_cast<uint8_t const *>(name.data()), name.size()};
            kimix_gdb_guid uid{};
            expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, nullptr), KIMIX_GDB_OK));
            kimix_gdb_guid const target = guard.vertex(node, kimix::format("fixed{}", i).c_str());
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, uid, target, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                            0, nullptr, 0, &edge),
                      KIMIX_GDB_OK));
            expect(eq(kimix_gdb_edge_free(session, edge), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_vertex_free(session, uid), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_vertex_free(session, target), KIMIX_GDB_OK));
        }
        // Everything is gone; the session is still usable and the close in the
        // guard is the only owned object's release.
        size_t count = 0;
        kimix_gdb_guid nobody{};
        expect(eq(kimix_gdb_vertex_free(session, nobody), KIMIX_GDB_ERR_VERTEX));
        expect(eq(kimix_gdb_vertex_labels(session, nobody, nullptr, 0, &count),
                  KIMIX_GDB_ERR_VERTEX));
    };
}
