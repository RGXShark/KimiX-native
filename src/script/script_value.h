/*
 * script_value.h -- the one value type that may cross the sandbox boundary.
 *
 * A `Value` is a PURE DATA snapshot of a Lua value: no pointer, no lua_State,
 * no reference into the interpreter heap.  That is the outbound half of the
 * pointer-control rule in src/script/README.md:
 *
 *   nil / booleans / integers / numbers -> copied by value,
 *   strings                            -> copied bytes,
 *   tables                             -> canonical JSON text (minified,
 *                                         object keys sorted), produced by
 *                                         the marshaller in script_value.cpp,
 *   function / thread / userdata       -> REFUSED (Status::limit); a script
 *                                         can therefore never hand the native
 *                                         side something callable or
 *                                         addressable.
 *
 * The type is deliberately flat (a Kind tag plus the three payload fields) so
 * a C caller can mirror it, and so a Result can hold a vector of them.
 */
#pragma once
#ifndef KIMIX_SCRIPT_VALUE_H
#define KIMIX_SCRIPT_VALUE_H

#include <cstdint>

#include <core/stl/string.h>

struct lua_State;

namespace kimix::script {

struct SandboxState;

struct Value {
    enum class Kind : uint8_t {
        nil = 0,
        boolean,
        integer,
        number,
        string, /* Kind::string: the raw bytes (s) */
        table,  /* Kind::table:  canonical JSON text (s) */
    };

    Kind kind = Kind::nil;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    kimix::string s;

    /* "nil" / "boolean" / "integer" / "number" / "string" / "table" -- the
     * same words the sandbox's LIMIT messages use. */
    static const char *kind_name(Kind k) noexcept;

    void clear() noexcept;
};

/* INTERNAL (script_value.cpp): marshal `count` Lua values starting at the
 * absolute stack index `first` into state->results.  Raises a Status::limit
 * error (and never returns) when a value cannot cross the boundary: a function,
 * a thread, a userdata, a table over the depth/byte cap, or a cycle. */
bool sb_marshal_results(lua_State *L, SandboxState *s, int first, int count);

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_VALUE_H */
