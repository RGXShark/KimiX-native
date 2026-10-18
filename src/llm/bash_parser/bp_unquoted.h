/* bp_unquoted.h — the `_process_unquoted` emitter (kimi-agent
 * `shell_compat/unquoted.py`): the lexer-driven unquoted-backslash
 * transform, driven by the shared boundary oracles of bp_core.h.
 *
 * Behavior (bit-for-bit identical to the reference):
 * - a backslash before a non-metacharacter becomes `/`;
 * - a backslash before a bash metacharacter is preserved as the escape pair;
 * - single/double/ANSI-C quote regions are copied verbatim (inside a closed
 *   double-quote region, `$(...)` and backtick substitutions recurse through
 *   the slice contract);
 * - top-level `$(...)` is consumed by the flat walk (no top-level
 *   command-substitution case); a top-level backtick recurses on its slice;
 * - escaped `nul` spellings (_ESCAPED_NUL_RE word spans) keep their
 *   backslashes so fix_bash_command can rewrite the whole word;
 * - unterminated quotes/regions copy the remainder verbatim.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

// `_process_unquoted(cmd)`.  Recursive on substitution slices exactly like
// the reference (slice depth is bounded by source nesting; the recursion is
// the reference's own contract — each recursion recomputes its own
// null-device spans on its slice).
kimix::string process_unquoted_impl(kimix::string_view cmd);

} // namespace kimix::llm::bash_parser
