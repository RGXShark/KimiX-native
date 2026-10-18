/* bp_paths.h — Windows path, null-device (`nul`) and `cd /d` rules.
 *
 * Ports of kimi-agent `shell_compat/paths.py` (which itself owns the
 * canonical `_BashFixScanner` path methods of bash_core.py).  Pure functions
 * of the raw word text (+ an optional explicit temp-dir spelling); they
 * return the replacement text or nullopt ("leave byte-for-byte").
 *
 * Unicode policy: ordinary UTF-8 is opaque and byte-preserved.  The rules
 * using Python isalpha/isalnum/upper (plausible_path_segments, the
 * multi-segment relative check, the /c/ drive-letter test) decode UTF-8
 * codepoints and consult the generated Python classification tables
 * (unicode_class.inc), so non-ASCII path words rewrite with full parity.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

// `decode_virtual_path_span`: decode Bash unquoted-word backslash escapes in
// a literal path span; nullopt on a trailing lone backslash or an escaped
// glob/brace character (*?[{}).
kimix::optional<kimix::string> decode_virtual_path_span(kimix::string_view span);

// `split_virtual_path_expansions`: split a rewritten virtual path into
// (is_expansion, span) runs; nullopt when the word embeds a command
// substitution, $"...", an unterminated expansion, or a quote character.
struct VirtualPathSpan {
    bool is_expansion = false;
    kimix::string text;
};
kimix::optional<kimix::vector<VirtualPathSpan>> split_virtual_path_expansions(kimix::string_view text);

// `plausible_path_segments`: at least one backslash-separated segment of
// length >= 2 starting with an alpha character.
bool plausible_path_segments(kimix::string_view raw);

// `decode_unquoted_word`: the word value after unquoted-form quote removal
// (every \X becomes X).
kimix::string decode_unquoted_word(kimix::string_view raw);

// `normalize_windows_path`: rewrite backslashes as forward slashes (UNC
// `\\` prefix becomes `//`; escaped literal chars drop the backslash).
kimix::string normalize_windows_path(kimix::string_view raw);

// `quote_path_word`: quote a normalized path only when unquoted emission
// would break it (tilde kept active, glob/brace chars outside quoting,
// metacharacter spans double-quoted with \, ", $, ` escaped).
kimix::string quote_path_word(kimix::string_view normalized);

// `windows_path_replacement`: the Git Bash spelling of a Windows backslash
// path word, or nullopt (quoted/expansion-bearing/ambiguous words).
kimix::optional<kimix::string> windows_path_replacement(kimix::string_view raw);

// `quote_virtual_path`: quote a rewritten virtual path without disabling
// shell expansions; nullopt when undecodable.
kimix::optional<kimix::string> quote_virtual_path(kimix::string_view normalized);

// `git_bash_abs_path_replacement`: the native Windows spelling of a Git Bash
// virtual absolute path (/tmp/... -> temp_dir/..., /c/... -> C:/...).
// `temp_dir` empty = windows_temp_dir() discovery.
kimix::optional<kimix::string> git_bash_abs_path_replacement(kimix::string_view raw, kimix::string_view temp_dir = {});

// `path_replacement`: the combined rule (windows_path_replacement first,
// then git_bash_abs_path_replacement); nullopt when the word is not a
// rewritable path.
kimix::optional<kimix::string> path_replacement(kimix::string_view raw, kimix::string_view temp_dir = {});

// `is_effective_nul_word`: true when a redirection target word resolves to
// `nul` (plain, backslash-escaped, ANSI-C, or empty-quote-concatenated
// spellings; any case).  Whole-token quoted 'nul'/"nul" is an intentional
// filename (false); runtime-expansion words are false.
bool is_effective_nul_word(kimix::string_view raw);

// The fixed null-device replacement text ("/dev/null").
inline constexpr kimix::string_view kNulReplacement = "/dev/null";

// `_NUL_FORBIDDEN_OPERATORS` membership (pure input redirections: the nul
// rewrite never fires for them).  Operator text WITHOUT fd digits.
bool is_nul_forbidden_operator(kimix::string_view op_core);

// `cd_flag_span_after`: the core cd /d decision.  `read_word` mirrors the
// scanner's `_read_word(i, end, scan_substitutions=false)` and
// `redir_after_fd` the `_redirection_after_fd` predicate; both are supplied
// as callbacks so the live scanner and the IR path share one decision.
// Returns the (start, end) span of the deletable /d flag word, or nullopt.
struct WordReader {
    kimix::function<size_t(size_t i, size_t end)> read_word;         // literal (no substitution scan) word end
    kimix::function<bool(size_t i, size_t end)> redirection_after_fd; // fd-prefixed redirection test
};
kimix::optional<std::pair<size_t, size_t>> cd_flag_span_after(const WordReader &reader, kimix::string_view source, size_t start, size_t end);

} // namespace kimix::llm::bash_parser
