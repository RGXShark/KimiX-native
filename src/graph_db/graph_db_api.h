/*
 * graph_db_api.h — linkage macro family for the kimix-graph-db library.
 *
 * Three states, the same shape as the KIMIX_CORE_API family in
 * <core/dll_export.h>:
 *
 *   KIMIX_GRAPH_DB_STATIC      -> plain (no decoration). The target propagates
 *                                 this define publicly (src/graph_db/xmake.lua),
 *                                 so the library and every consumer agree.
 *   KIMIX_GRAPH_DB_EXPORT_DLL  -> dllexport (Windows) / default visibility (ELF,
 *                                 Mach-O). For a future shared build of the same
 *                                 sources.
 *   neither                    -> also plain.
 *
 * Deviation from <core/dll_export.h>, deliberate and documented: the "neither"
 * state is plain rather than __declspec(dllimport). kimix-graph-db is only ever
 * produced as a static archive today (target kind "static"), so the import state
 * could only ever manufacture link errors against an archive; the pattern is
 * kept three-state so a shared flavour needs no header change later. Nothing in
 * this project defines KIMIX_GRAPH_DB_EXPORT_DLL today.
 *
 * Every exported declaration of src/graph_db carries KIMIX_GRAPH_DB_API.
 */
#pragma once

#if defined(KIMIX_GRAPH_DB_STATIC)
    #define KIMIX_GRAPH_DB_API
#elif defined(KIMIX_GRAPH_DB_EXPORT_DLL)
    #ifdef _WIN32
        #define KIMIX_GRAPH_DB_API __declspec(dllexport)
    #else
        #define KIMIX_GRAPH_DB_API __attribute__((visibility("default")))
    #endif
#else
    #define KIMIX_GRAPH_DB_API
#endif