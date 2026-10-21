-- Kimix graph database (src/graph_db)
--
-- Target: kimix-graph-db — a STATIC library holding the in-memory graph layer
-- and its cold-storage glue, on top of kimix-core only:
--
--   graph_db_api.h  the KIMIX_GRAPH_DB_API linkage-macro family (three states,
--                   same shape as KIMIX_CORE_API in src/core/dll_export.h).
--                   The target is static, so it defines KIMIX_GRAPH_DB_STATIC
--                   publicly and the macro expands to nothing for the library
--                   and for every consumer.
--   utils.h/.cpp    key scheme ("v/" + 16-byte binary guid, "meta/root"
--                   reserved), encode/decode_vertex_record (the little-endian
--                   [GDB1][guid][MD5][n_dep][n_depended][guids][content]
--                   record), and ColdStorage — a thin RAII front end over
--                   kimix::LMDB (src/core/lmdb.h) with batched transactional
--                   writes and copy_to() checkpoints.
--   vertex.h/.cpp   one vertex: guid, opaque content blob + cached MD5, the
--                   dependency links in both forms (guids and Vertex* sets), a
--                   64-byte-separated kimix::shared_atomic_mutex and the dirty
--                   flag.
--   graph.h/.cpp    the vertex container: structural CRUD with a cascading
--                   remove, connect/disconnect, roots, neighbors/dependencies,
--                   cycle-safe BFS has_path/shortest_path, dirty bookkeeping,
--                   and the two-level (graph shared_mutex / vertex
--                   shared_atomic_mutex) concurrency contract.
--   graph_db.h      umbrella include + KIMIX_GRAPH_DB_VERSION.
--
-- Dependency rule: kimix-core only. Third-party code is reached through it
-- (LMDB's C sources are already compiled into kimix-core, mimalloc backs
-- kimix::allocator), so this target never add_deps() a src/ext target — the
-- same rule kimix-llm and kimix_api follow.
--
-- Later subtasks add serialize.cpp (parallel incremental save + dual-stage
-- load) and the benchmark target; this file needs no change for them
-- (`add_files("*.cpp")` picks them up), only the test registrations in
-- tests/xmake.lua.
--
-- Build:   xmake build kimix-graph-db   ->  bin/<mode>/libkimix-graph-db.a
target("kimix-graph-db")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    -- `src/` on the (public) include path: this module's own sources include
    -- its headers as "graph.h"/"utils.h" (same directory) and core as
    -- <core/lmdb.h>, and a dependent target gets the identical spelling —
    -- #include <graph_db/graph_db.h>.
    add_includedirs("..", { public = true })
    -- `.` public as well, so a consumer may also include the flat spelling
    -- "graph_db.h" (documented in graph_db.h's own example).
    add_includedirs(".", { public = true })
    add_deps("kimix-core")
    -- No KIMIX_CORE_STATIC (and no public propagation of it): KIMIX_CORE_API
    -- resolves as dllimport against kimix-core.dll, both here and for this
    -- archive's dependents (the tests).
    -- Static linkage of this library's own symbols: KIMIX_GRAPH_DB_API
    -- expands to nothing (src/graph_db/graph_db_api.h).
    add_defines("KIMIX_GRAPH_DB_STATIC", { public = true })
    on_load(function(target)
        -- KIMIX_GRAPH_DB_VERSION comes from version.txt, the single source of
        -- truth for the project version (same file src/xmake.lua turns into
        -- kimix_version.h). A missing/unreadable file falls back to "0.0.0",
        -- which is also the header's #ifndef fallback.
        local version_txt = path.join(os.projectdir(), "version.txt")
        local text = os.isfile(version_txt) and io.readfile(version_txt) or nil
        local version = text and text:gsub("^%s*(.-)%s*$", "%1") or "0.0.0"
        target:add("defines", 'KIMIX_GRAPH_DB_VERSION="' .. version .. '"', { public = true })
        if target:is_plat("windows") then
            target:add("defines", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", { public = true })
        end
    end)
    _config_project({ batch_size = 8, project_kind = "static" })
target_end()