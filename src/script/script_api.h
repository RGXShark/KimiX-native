/*
 * script_api.h -- the KIMIX_SCRIPT_API linkage macro family (kimix-script).
 *
 * Same three states as KIMIX_GRAPH_DB_API / KIMIX_LLM_API (src/core/dll_export.h):
 *
 *   KIMIX_SCRIPT_STATIC      -> nothing (a static build/archive: the target
 *                               kimix-script is static and defines this
 *                               publicly, so both the library and its
 *                               consumers see plain declarations).
 *   KIMIX_SCRIPT_EXPORT_DLL  -> dllexport / default visibility (reserved for
 *                               a future shared variant).
 *   neither (default)        -> dllimport on Windows, nothing elsewhere.
 */
#pragma once
#ifndef KIMIX_SCRIPT_API_H
#define KIMIX_SCRIPT_API_H

#include <core/dll_export.h>

#if defined(KIMIX_SCRIPT_STATIC)
    #define KIMIX_SCRIPT_API
#elif defined(KIMIX_SCRIPT_EXPORT_DLL)
    #ifdef _WIN32
        #define KIMIX_SCRIPT_API __declspec(dllexport)
    #else
        #define KIMIX_SCRIPT_API __attribute__((visibility("default")))
    #endif
#else
    #ifdef _WIN32
        #define KIMIX_SCRIPT_API __declspec(dllimport)
    #else
        #define KIMIX_SCRIPT_API
    #endif
#endif

#endif /* KIMIX_SCRIPT_API_H */
