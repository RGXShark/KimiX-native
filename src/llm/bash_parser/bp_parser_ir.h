/* bp_parser_ir.h — tolerant command-context parser for the lossless shell IR
 * (port of kimi-agent `shell_compat/parser.py`).
 *
 * Builds the bp_models IR from the flat token stream of bp_lexer.  This is
 * deliberately NOT a POSIX grammar: structure is recognized only where
 * recognition is safe — command word vs arguments vs assignments, redirections,
 * heredocs, comments, nested executable substitutions, wrapper hints.  Anything
 * ambiguous stays a RawNode and the parser NEVER raises on malformed input.
 *
 * Depth model: nested command contexts are queued as worklist tasks drained by
 * a WHILE loop (`_drain_tasks`), so descent adds no native frames — arbitrarily
 * deep input cannot overflow the stack.  Beyond `kMaxNestingDepth` the region is
 * not parsed (a `kDepthLimit` diagnostic, characters owned by a RawNode).
 *
 * Tiling invariant: `render_source(parse(src)) == src` byte-for-byte for every
 * input, valid or malformed.
 *
 * Public surface (the reference `__all__`).
 */
#pragma once

#include <core/kimix_core.h>

#include "llm/bash_parser/bp_lexer.h"
#include "llm/bash_parser/bp_models.h"

namespace kimix::llm::bash_parser {

// COMMAND_POSITION_OPERATORS = {"|", "|&", "&&", "||", ";", ";&", ";;&", "("}.
bool is_command_position_operator(kimix::string_view op);

// DECLARATION_BUILTINS = {declare, typeset, local, export, readonly}.
bool is_declaration_builtin(kimix::string_view word);

// PIPELINE_OPERATORS = {"|", "|&"}.
bool is_pipeline_operator(kimix::string_view op);

// classify_assignment(raw_word): the variable name when raw_word is a
// `NAME[+]=…` assignment (the same predicate as the legacy `_ASSIGNMENT_RE`,
// with the captured name — the `+` kept), else nullopt.
kimix::optional<kimix::string> classify_assignment(kimix::string_view raw_word);

// word_to_node(token, source): build a WORD Node (anatomy + static literal).
Node word_to_node(const Token &token, kimix::string_view source);

// is_effective_nul_target(word, source): the legacy null-device predicate
// lifted to a parsed redirection target.  True/False when statically decidable,
// nullopt when runtime expansions keep the value open.
kimix::optional<bool> is_effective_nul_target(const Node &word, kimix::string_view source);

// keyword_of(raw, literal): the reserved-WORD value of a token, or nullopt when
// it is not plain.  A quoted or expansion-bearing word is data; bare
// operator-shaped keywords ({ } in then do else elif fi done esac if while
// until for case select time coproc function) are reserved words when spelled
// bare even though the static decoder maps them to nullopt.
kimix::optional<kimix::string> keyword_of(kimix::string_view raw, const kimix::optional<kimix::string> &literal);

// parse(source): the lossless tolerant shell IR (root ScriptNode).  Never
// raises; unrecognized structure stays byte-for-byte and nesting beyond
// `kMaxNestingDepth` stops descending.  The returned node owns the whole tree
// (move-only); its `source` field carries the single string every span indexes
// into.
Node parse(kimix::string_view source);

// parse_inner(source): parse a standalone (runtime-reparsed) script into its
// own IR; the returned ScriptNode owns `source` with local coordinates.
Node parse_inner(kimix::string_view source);

// parse_command_context == parse (the reference alias).
Node parse_command_context(kimix::string_view source);

} // namespace kimix::llm::bash_parser
