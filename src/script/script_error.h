/*
 * script_error.h -- INTERNAL: message formatting and the sticky raise helpers.
 *
 * One format, composed in exactly one place (script_error.cpp) so the bytes a
 * script's own error handler sees and the bytes the host receives cannot
 * diverge:
 *
 *     <chunk>:<line>: <PREFIX>: <api> -- <detail>
 *
 * The formatter is POD-only (snprintf into a fixed buffer, no allocation), and
 * the raise helpers (script_sticky.cpp) run it BEFORE they longjmp, so no C++
 * object with a destructor is ever alive when lua_error unwinds (README
 * §"The longjmp rule").  "<line>" is supplied by luaL_where(L, 1) at raise
 * time -- LUA_IDSIZE (60) truncates a long chunk name in the message; the full
 * name is carried in Result::violation.chunk.
 */
#pragma once
#ifndef KIMIX_SCRIPT_ERROR_H
#define KIMIX_SCRIPT_ERROR_H

#include <cstddef>
#include <cstdint>

#include <script/script_status.h>

struct lua_State;

namespace kimix::script {

/* The separator between the api and the detail in a DENIED message
 * (U+2014 EM DASH, written as an escape so the source stays 7-bit clean). */
#define SB_DASH " \xE2\x80\x94 "

/* Compose "<PREFIX><api> -- <detail>" into `out` (NUL-terminated, truncated to
 * `cap`).  `api` may be NULL/empty (memory, timeout, limit, cancelled), in which
 * case the body is just "<PREFIX><detail>".  POD-only: safe to call on the
 * raise path.  Returns the number of bytes written (excluding the NUL). */
size_t sb_compose_body(char *out, size_t cap, Status kind, const char *api, const char *detail) noexcept;

/* Copy a NUL-terminated location prefix ("chunk:12: ", or "" when the running
 * frame has no line info) from luaL_where(L, 1) into `out`. */
size_t sb_where(lua_State *L, char *out, size_t cap) noexcept;

/* Byte length of a leading "<chunk>:<line>: " location, 0 when there is none.
 * Handles chunk names that contain ':' (a Windows path, say) by accepting the
 * LAST ":"-digits-": " group. */
size_t sb_location_prefix_len(const char *msg, size_t len) noexcept;

/* The line number of a message that starts with a location, 0 when absent. */
int32_t sb_line_from_message(const char *msg, size_t len) noexcept;

/* ---- the sticky raise helpers (script_sticky.cpp) ---------------------- */

/* First violation wins: record kind/api/category/detail (+ the composed body),
 * then raise "<where>BODY" with lua_error.  `api` may be NULL/empty for the
 * kinds that have no api (memory / timeout / cancelled / limit / behaviour),
 * in which case the body is "<PREFIX><detail>".  Never returns. */
int sb_raise(lua_State *L, Status kind, const char *api, const char *category, const char *detail);

/* Same as sb_raise, but the raised body is `verbatim` (already prefixed by the
 * caller) -- used by `load` for the LOADERR bytecode refusal, whose text is
 * "LOADERR: ..." rather than "DENIED: load -- ...".  Never returns. */
int sb_raise_verbatim(lua_State *L, Status kind, const char *api, const char *category, const char *detail,
                      const char *verbatim);

/* printf-shaped variant of sb_raise for the kinds without an api: the formatted
 * text becomes the detail (e.g. "allocation of N bytes exceeds ...").  The
 * formatting happens into a fixed POD buffer BEFORE the raise.  Never returns. */
int sb_raisef(lua_State *L, Status kind, const char *category, const char *fmt, ...);

/* Re-raise the recorded sticky message verbatim (what the re-raising wrappers
 * do: pcall/xpcall/coroutine.resume/coroutine.close).  Never returns. */
int sb_sticky_reraise(lua_State *L);

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_ERROR_H */
