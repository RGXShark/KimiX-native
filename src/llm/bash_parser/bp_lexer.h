/* bp_lexer.h — lossless shell LEXER: one flat O(n)-per-slice tokenizer for the
 * shell IR (port of kimi-agent `shell_compat/lexer.py`).
 *
 * The lexer drives every region boundary through the boundary oracles in
 * bp_core.h (find_ansi_c_end / find_backtick_end / find_dq_end /
 * find_matching_paren / match_* / read_control_operator /
 * read_redirection_operator / redirection_after_fd / comment_starts); it never
 * re-implements a boundary, so a token edge can only ever be a legacy boundary
 * decision.  `lex` performs one pass over the range plus one pass per region
 * entry; it is non-recursive, so pathologically nested input
 * (`$( $( $( … ) ) )`) terminates in bounded stack space.
 *
 * Lossless invariant: token spans tile the [start, end) range exactly —
 * whitespace runs become RAW tokens, a `\`+newline pair becomes a RAW token, a
 * newline a NEWLINE token, comments end at (exclusive) the newline,
 * redirections (with optional fd prefixes) are REDIRECTION tokens carrying the
 * full text and the parsed fd, control operators are OPERATOR tokens, and
 * words are WORD tokens whose `parts` cover the word span (literal runs, quote
 * / escape / substitution regions, and the sub-regions discovered *directly*
 * inside a double quote recorded as sibling parts immediately after the
 * enclosing quote part).  Unterminated regions never throw: they extend to the
 * range end and record a diagnostic.
 *
 * Coordinate policy: every offset is a UTF-8 byte offset into `source`
 * (ASCII-only shell syntax; non-ASCII bytes are opaque and copied verbatim),
 * identical to the rest of the bash_parser module.
 *
 * Internal surface (directly testable, not part of the stable API).
 */
#pragma once

#include <core/kimix_core.h>

#include "llm/bash_parser/bp_spans.h"

namespace kimix::llm::bash_parser {

// WordPartKind: lexical anatomy of one region inside a word.
//
// NOTE: bp_models.h does not exist yet.  This enum is declared here so the
// lexer and its unit test compile standalone; the IR (models) task MUST reuse
// this exact declaration (include bp_lexer.h) rather than re-declare a
// competing `WordPartKind`.  The 13 members mirror `models.py WordPartKind`
// (LITERAL, SINGLE_QUOTE, DOUBLE_QUOTE, ANSI_C_QUOTE, LOCALE_QUOTE,
// PARAMETER_EXPANSION, ARITHMETIC_EXPANSION, COMMAND_SUBSTITUTION,
// BACKQUOTE_SUBSTITUTION, PROCESS_SUBSTITUTION, ESCAPED_CHARACTER,
// LINE_CONTINUATION, RAW).
enum class WordPartKind {
    kLiteral,
    kSingleQuote,
    kDoubleQuote,
    kAnsiCQuote,
    kLocaleQuote,
    kParameterExpansion,
    kArithmeticExpansion,
    kCommandSubstitution,
    kBackquoteSubstitution,
    kProcessSubstitution,
    kEscapedCharacter,
    kLineContinuation,
    kRaw,
};

// TokenKind: category of a lexed token (`lexer.py TokenKind`).
enum class TokenKind {
    kWord,
    kOperator,
    kRedirection,
    kComment,
    kNewline,
    kRaw,
};

// The ``(content_start, content_end)`` range of a region's content, stripped of
// its opener/closer.  `nullopt` for unterminated or non-addressable regions.
using WordPartInner = kimix::optional<std::pair<size_t, size_t>>;

// WordPart: a lexical region inside a word, the ``(kind, span, inner)`` triple.
struct WordPart {
    WordPartKind kind = WordPartKind::kRaw;
    SourceSpan span;
    WordPartInner inner;
};

// Token: one lexed source span.  WORD tokens carry `parts` in source order;
// OPERATOR/REDIRECTION tokens carry their operator text in `text`; REDIRECTION
// tokens with an explicit fd prefix (``2>file``) carry that fd in `fd`.
//
// LIFETIME: `text` is a zero-copy `string_view` INTO the `source` passed to
// `lex`.  A Token (and the LexResult holding it) therefore only stays valid for
// as long as its `source` outlives it.  In this module's usage the source is
// owned by the caller (ultimately the root ScriptNode) and outlives every token,
// so the view is safe; callers that need to detach tokens from their source must
// copy `text` before the source goes away.  (Deviation from the Python, which
// stored an owning `str`; the port chooses string_view for zero-copy.)
struct Token {
    TokenKind kind = TokenKind::kRaw;
    SourceSpan span;
    kimix::vector<WordPart> parts;
    // operator / token text (a view into source); empty for WORD tokens (which
    // own no separate text — read source[span] instead) and for NEWLINE tokens.
    kimix::string_view text;
    // fd number for redirection tokens with an explicit fd prefix, else nullopt.
    kimix::optional<int32_t> fd;
};

// LexResult: the tokens of one range plus the lexer's internal diagnostics.
struct LexResult {
    kimix::vector<Token> tokens;
    kimix::vector<Diagnostic> diagnostics;
};

// Sentinel `end` for `lex`: resolved to source.size() when the caller omits it.
inline constexpr size_t kLexUnbounded = SIZE_MAX;

// Tokenize source[start:end] into a flat, source-ordered token list.
//
// Every region boundary comes from an oracle helper (see bp_core.h); no
// recursion is used at any depth.  Token spans tile the range exactly —
// whitespace runs become RAW tokens — the base of the lossless invariant.
// Diagnostics (unterminated quotes/regions) are collected, never raised:
// malformed input still yields a tiling stream whose spans cover the range.
//
// DEVIATION (range validation): the reference raises ValueError for a range
// outside ``0 <= start <= end <= len(source)``.  This port does not throw (the
// project builds without exceptions); an invalid range instead yields an EMPTY
// LexResult carrying a single diagnostic (kind kAmbiguousRegion).
LexResult lex(kimix::string_view source, size_t start = 0, size_t end = kLexUnbounded);

// ── module-level constant sets, ported as predicate functions ──────────────

// TRUNCATING_REDIRECT_OPERATORS: {">", ">|", ">>", "<>", "&>", "&>>"}.
bool is_truncating_redirect_operator(kimix::string_view op);

// INPUT_ONLY_REDIRECT_OPERATORS: {"<", "<<", "<<-", "<<<", "<&"}.
bool is_input_only_redirect_operator(kimix::string_view op);

// HEREDOC_OPERATORS: {"<<", "<<-"} (consume a heredoc delimiter as the word).
bool is_heredoc_operator(kimix::string_view op);

// EXECUTABLE_REGIONS: {COMMAND_SUBSTITUTION, BACKQUOTE_SUBSTITUTION,
// PROCESS_SUBSTITUTION} — region kinds whose content is an executable context.
bool is_executable_region(WordPartKind kind);

} // namespace kimix::llm::bash_parser
