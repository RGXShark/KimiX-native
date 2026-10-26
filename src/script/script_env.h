/*
 * script_env.h -- INTERNAL: the run environment (L2 of the sandbox layers).
 *
 * `sb_env_build` creates the fresh _ENV table of one run and fills it with
 * exactly three kinds of name:
 *
 *   ALLOW   the safe base functions and the string/table/math/utf8 tables,
 *   REPLACE a vetted C wrapper in place of a stock function (load, pcall,
 *           xpcall, print, warn, tostring, collectgarbage, string.format, and
 *           coroutine.resume/close when coroutines are on),
 *   DENY    a LOUD tripwire: a table or function that exists, is callable, and
 *           raises "DENIED: <api> -- <why>" naming the exact member (io.open,
 *           os.exit, package.loadlib, ...).
 *
 * A tripwire beats an absent global on purpose: "attempt to index a nil value"
 * is silent and unhelpful, while the tripwire tells the script (and the host)
 * what it tried to do and why it is not allowed.
 *
 * `sb_env_verify` is the integrity post-check: it re-reads every primitive from
 * the environment and proves it is still the sandbox's own C closure (same
 * lua_CFunction AND same upvalue-1 tag), so a chunk that does
 * `pcall = function() return true end` is reported as Status::behaviour instead
 * of being silently obeyed.  `sb_push_text` is the redacting converter behind
 * tostring/print/error-to-string: it never prints an address.
 */
#pragma once
#ifndef KIMIX_SCRIPT_ENV_H
#define KIMIX_SCRIPT_ENV_H

#include <cstdint>

#include <core/stl/vector.h>

struct lua_State;

namespace kimix::script {

struct SandboxNative;
struct SandboxState;

/* Build the environment and leave it on the top of the stack (the caller owns
 * that stack slot).  `natives` are the host functions from
 * Sandbox::add_native.  May raise (LIMIT / MEMORY) -- never returns normally in
 * that case. */
void sb_env_build(lua_State *L, SandboxState *s, const kimix::vector<SandboxNative> &natives, bool gc_count);

/* true when every sandbox primitive in the environment is still ours.  Records
 * the BEHAVIOUR violation itself on failure (no raise). */
bool sb_env_verify(lua_State *L, SandboxState *s);

/* Push the redacted text of the value at `idx` ("nil", "true", "1.5",
 * "table: #1", "function: #2", or the value's own __tostring result).  Returns
 * false only when the interpreter cannot allocate. */
bool sb_push_text(lua_State *L, SandboxState *s, int idx);

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_ENV_H */
