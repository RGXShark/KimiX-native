/* bp_core.h — internal foundation of the bash_parser module: boundary
 * oracles, word decoders, and the ported constant tables.
 *
 * Ports of kimi-agent `shell_compat/bash_core.py` module-level helpers
 * (`_find_ansi_c_end`, `_find_backtick_end`, `_find_matching_paren`,
 * `_find_dq_end`, the `_match_*` family), `_BashFixScanner._literal_word_value`
 * / `_ansi_c_literal_span`, and the frozenset/regex constant tables.
 *
 * Byte-oriented coordinate policy: all offsets are UTF-8 byte offsets into the
 * source.  Recognized shell syntax is ASCII; non-ASCII bytes are opaque and
 * copied byte-for-byte.  Python `str.isdigit` on the ASCII paths reduces to
 * ASCII digits; the classification helpers below use explicit ASCII checks
 * (never locale-dependent C isalpha on signed bytes).
 *
 * Internal surface (directly testable, not part of the stable API).
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

// Source text accessor: a string_view plus length in bytes.  All oracle
// functions mirror the Python signatures: an "index AFTER the closer"
// return, or -1 when unterminated.  A `kimix::optional<size_t>` return
// mirrors Python `Optional[int]` (None).

// ── boundary oracles (bash_core module level; byte-for-byte ports) ─────────

// Index AFTER the closing ' of a $'...' region; `start` is the first char
// INSIDE the region (right after `$'`).  -1 when unterminated.  Every `\X`
// pair is an escape.
int64_t find_ansi_c_end(kimix::string_view cmd, int64_t start);

// Index AFTER the closing ` of a backtick region; `start` is right after the
// opening backtick.  -1 when unterminated.  `\X` skips a pair.
int64_t find_backtick_end(kimix::string_view cmd, int64_t start);

// Index of the ) matching the ( at `open_pos`.  -1 when none.  Tracks nested
// $(...), single quotes, double quotes (with their nested regions),
// backticks, $'...' regions and plain subshell parens.  `open_pos` must
// point at '(' (the reference asserts it; here it returns -1 defensively).
int64_t find_matching_paren(kimix::string_view cmd, int64_t open_pos);

// Index AFTER the closing " of a double-quoted region; `start` is right after
// the opening ".  -1 when unterminated.  Recognizes \X escapes (X in
// "$`\), nested $(...) (descends through find_matching_paren), literal $'
// inside quotes, and backtick substitutions.
int64_t find_dq_end(kimix::string_view cmd, int64_t start);

// Scanner-family matchers (the `_match_*` ports).  Python `Optional[int]`.

// Index after the ' closing a $' region opened before `i`; nullopt when
// unterminated.
kimix::optional<size_t> match_ansi_c_quote_span(kimix::string_view text, size_t i);

// Index after the " closing a double-quoted region opened before `i`;
// nullopt when unterminated.  Skips every \X pair, no region descent
// (documented difference from find_dq_end).
kimix::optional<size_t> match_dquote_span(kimix::string_view text, size_t i);

// Index after the } closing ${ (opened at i-2); nullopt when unterminated or
// when a $(...) / backtick is embedded.
kimix::optional<size_t> match_parameter_braces(kimix::string_view text, size_t i);

// Index after the )) closing $(( (opened at i-3); nullopt when unterminated
// or when a $(...) is embedded.
kimix::optional<size_t> match_arithmetic_span(kimix::string_view text, size_t i);

// ── word decoding (bash_core._BashFixScanner static methods) ───────────────

// The word value produced solely by Bash quote removal, or nullopt when a
// parameter/command/arithmetic expansion, glob/brace/tilde expansion, ANSI-C
// escapes the decoder rejects, or malformed quoting contributes.  Port of
// `_literal_word_value`.  Decoded values may contain embedded NUL bytes
// (truncation happens at the C-string boundary in bash; the reference keeps
// full Python strings — the port keeps full byte strings).
kimix::optional<kimix::string> literal_word_value(kimix::string_view raw);

// Decode the $'...' ANSI-C quote starting at `i` in `raw`.  Returns
// (value, index_after_closing_quote) or nullopt when unterminated or holding
// an unsupported escape (\u/\U, multibyte octal/hex > 0x7F, unknown escape).
// The value truncates at the first NUL-producing escape (bash C strings).
// Port of `_ansi_c_literal_span`.
struct AnsiCLiteralSpan {
    kimix::string value;
    size_t index_after;
};
kimix::optional<AnsiCLiteralSpan> ansi_c_literal_span(kimix::string_view raw, size_t i);

// ── predicates ported from the reference regexes ───────────────────────────

// `_ASSIGNMENT_RE` = ^[A-Za-z_][A-Za-z0-9_]*(\+)?=  — true when `raw`
// starts with a NAME[+]= assignment prefix.
bool is_assignment_word(kimix::string_view raw);

// `_NAME_RE` = ^[A-Za-z_][A-Za-z0-9_]*$ — full match.
bool is_name_word(kimix::string_view raw);

// `_GLUED_LABEL_RE` = ^:[A-Za-z_][A-Za-z0-9_]*$ — glued cmd label word.
bool is_glued_label(kimix::string_view raw);

// `_CALL_BATCH_RE` = \.(bat|cmd)$ (case-insensitive).
bool is_call_batch_target(kimix::string_view raw);

// `_FOR_LOOP_TOKEN_RE` = ^(%%?)([A-Za-z_][A-Za-z0-9_]*)$; returns the
// loop-variable name (group 2) when matched, else nullopt.
kimix::optional<kimix::string> for_loop_token_name(kimix::string_view raw);

// `_SHELL_C_OPTIONS` = ^-c$|^-lc$|^-cl$.
bool is_shell_c_option(kimix::string_view word);

// `_SHELL_C_SCRIPT_OPTION_RE` = ^-[A-Za-z]*c$ (last flag is c).
bool is_shell_c_script_option(kimix::string_view word);

// `_ERRORLEVEL_RE` = ^[0-9]{1,9}$.
bool is_errorlevel_operand(kimix::string_view word);

// `_INT_RE` = ^-?[0-9]+$.
bool is_int_operand(kimix::string_view word);

// Python `str.isdigit()` restricted to the contexts used by the reference
// (fd prefixes, redirection tests): ASCII digits only.
inline bool is_ascii_digit(char ch) { return ch >= '0' && ch <= '9'; }

// Python `str.isalpha()` / `str.isalnum()` for path rules: the reference runs
// on Unicode text.  Non-ASCII bytes make these predicates conservative: the
// path rules DECLINE the rewrite for words containing bytes >= 0x80 (documented
// difference; see README).  ASCII-only equivalents below.
inline bool is_path_alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
inline bool is_path_alnum(char ch) { return is_path_alpha(ch) || is_ascii_digit(ch); }

// ── constant tables (bash_core frozensets; membership predicates) ──────────
// All are `inline` functions over sorted static string arrays so there is no
// dynamic-initialization-order dependency.

bool in_command_start_keywords(kimix::string_view w);   // ! { if then elif else while until do
bool in_command_end_keywords(kimix::string_view w);     // fi done esac
bool in_list_keywords(kimix::string_view w);            // for select case
bool in_shell_keywords(kimix::string_view w);           // ! { } if then elif else while until do fi done esac for select case time coproc in function
bool in_command_wrappers(kimix::string_view w);         // builtin command coproc env exec nohup sudo time timeout stdbuf nice xargs
bool in_same_shell_wrappers(kimix::string_view w);      // builtin coproc time watch
bool in_shell_wrappers(kimix::string_view w);           // bash sh dash ash
bool in_exec_operand_commands(kimix::string_view w);    // find xargs eval ssh watch
bool in_pattern_program_commands(kimix::string_view w); // grep fgrep egrep rg sed awk gawk mawk
bool in_grep_pattern_family(kimix::string_view w);      // grep fgrep egrep rg
bool in_grep_value_options(kimix::string_view w);       // -A -B -C -m --after-context ... (see reference)
int32_t wrapper_operand_count(kimix::string_view kind); // timeout -> 1, else 0

// `_REPARSE_BUILTINS`: eval -> -1 (every operand), trap -> 1; nullopt else.
kimix::optional<int32_t> reparse_builtin_count(kimix::string_view word);

// `_WRAPPER_OPTIONS_WITH_VALUE[kind]` membership.
bool wrapper_option_with_value(kimix::string_view kind, kimix::string_view word);
// `_WRAPPER_PATH_OPTIONS[kind]` membership.
bool wrapper_path_option(kimix::string_view kind, kimix::string_view word);
// `_WRAPPER_PATH_OPTION_LONG` membership.
bool wrapper_path_option_long(kimix::string_view word);
// `_SHELL_VALUE_OPTIONS` membership.
bool in_shell_value_options(kimix::string_view word);

// Path-rule character sets (paths.py / bash_core).
bool in_path_safe_chars(char ch);       // _PATH_SAFE_CHARS
bool in_path_glob_chars(char ch);       // _PATH_GLOB_CHARS: *?[]{}
bool in_escaped_literal_chars(char ch); // _ESCAPED_LITERAL_CHARS: " \t&;|()<>'\"$`{}!"
bool in_operator_chars(char ch);        // _OPERATOR_CHARS: ;&|()<>\n
bool in_word_end_chars(char ch);        // _WORD_END_CHARS: operators + space \t \r
bool in_redirection_start(char ch);     // <>
bool in_control_op_starts(char ch);     // ;&|()
bool in_bash_metacharacters(char ch);   // _BASH_METACHARACTERS (unquoted transform)
bool in_dq_escaped(char ch);            // _DQ_ESCAPED: " \ $ `

// `_PATH_DRIVE_RE` fullmatch: [A-Za-z]:[\\/].*
bool fullmatch_path_drive(kimix::string_view raw);
// `_PATH_SEGMENT_RE` fullmatch: [A-Za-z0-9_.~\\-]+
bool fullmatch_path_segment(kimix::string_view value);

// `_MAX_NESTING_DEPTH`
constexpr int32_t kMaxNestingDepth = 1024;

// Operator readers (bash_core `_read_shell_control_operator` /
// `_read_control_operator` / `_read_redirection` semantics).  `op_out`
// receives the operator text ("" when none); the return value is the index
// after the operator (== `i` when nothing matched).
size_t read_shell_control_operator(kimix::string_view s, size_t i, size_t n, kimix::string_view *op_out);
size_t read_control_operator(kimix::string_view s, size_t i, size_t n, kimix::string_view *op_out);
size_t read_redirection_operator(kimix::string_view s, size_t i, size_t n, kimix::string_view *op_out);

// `_redirection_after_fd`: true when the digit run at i is followed by a
// redirection character.
bool redirection_after_fd(kimix::string_view s, size_t i, size_t n);

// `_comment_starts` predicate.
bool comment_starts(kimix::string_view s, size_t i, size_t range_start);

// The Windows temp directory as a forward-slash path (tempfile.gettempdir()
// equivalent): TEMP, then TMP, then `C:/Temp` fallback; backslashes
// normalized; non-throwing; never empty.
kimix::string windows_temp_dir();

// `_single_quote` (fallback_data): wrap text in single quotes, escaping
// embedded ' as '\''.
kimix::string single_quote(kimix::string_view text);

// ── shared inline helpers ────────────────────────────────────────────────────
// Defined inline in the header so every bp_*.cpp translation unit shares ONE
// definition (the kimix-llm unity build concatenates them into a single TU,
// where per-file anonymous-namespace copies would collide).

inline bool bp_starts_with(kimix::string_view s, kimix::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool bp_ends_with(kimix::string_view s, kimix::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ASCII-only lowercase copy (Python str.lower()/casefold() for ASCII bytes;
// non-ASCII bytes pass through unchanged).
inline kimix::string bp_ascii_lower(kimix::string_view s) {
    kimix::string out(s);
    for (char &ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

} // namespace kimix::llm::bash_parser
