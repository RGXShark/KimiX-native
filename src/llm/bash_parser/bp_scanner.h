/* bp_scanner.h — the compatibility scanner: port of kimi-agent
 * `shell_compat/bash_core.py::_BashFixScanner` (the live engine reader that
 * `translator.translate` drives).
 *
 * The scan FLOW is a line-for-line port of `_scan_range_inner` and its
 * helpers; every rule DECISION delegates to the rule modules (bp_paths,
 * bp_wrappers, bp_cmd, bp_heredoc) exactly like the reference delegates to
 * paths.py / wrappers.py / cmd_translator.py / heredoc.py.  Rule modules
 * take a `Scanner&` as their "reader" (the reference passes `self`), so the
 * word-boundary primitives (`read_word`, `literal_word_value`,
 * `redirection_after_fd_at`) have ONE implementation shared by the live
 * scan and every rule decision.
 *
 * Depth model: `_scan_range` / `_find_matching` recursion in the reference is
 * bounded by _MAX_NESTING_DEPTH (1024) and catches Python's RecursionError as
 * a whole-command pass-through.  This port keeps the reference's exact
 * synchronous DFS structure (so report discovery order matches byte-for-byte)
 * and the same 1024 budget, but runs the scan on a dedicated thread with a
 * large stack (`scan_all`), so 250-level and 2,000-level nesting cannot
 * overflow the native stack even though the recursion is synchronous.  Beyond
 * the budget `scan_range` returns immediately and `find_matching` reports
 * "unmatched" — the conservative reference outcome.
 */
#pragma once

#include <core/kimix_core.h>

#include "llm/bash_parser/bash_parser.h"
#include "llm/bash_parser/bp_core.h"

namespace kimix::llm::bash_parser {

// Mutable scan reports + edits; the port of the reference scanner's public
// attribute surface (names/path_notes/shell_notes/nul_fixes/unsupported/
// cmd_conditional_notes/edits/heredoc_events).
struct ScanReports {
    struct Edit {
        size_t start = 0;
        size_t end = 0;
        kimix::string replacement;
    };
    kimix::vector<Edit> edits;                      // recording order
    kimix::vector<kimix::string> names;             // fallback registrations (duplicates kept)
    kimix::vector<kimix::string> path_notes;
    kimix::vector<kimix::string> shell_notes;
    kimix::vector<kimix::string> nul_fixes;
    kimix::vector<kimix::string> unsupported;       // first-occurrence dedup
    kimix::vector<kimix::string> cmd_conditional_notes;
    kimix::vector<std::pair<int64_t, int64_t>> heredoc_events; // (redir_line_end, terminator_end)
};

// The compatibility scanner.  Construction never throws; `fix()` returns the
// complete BashFix (never fails — malformed input passes through).
class Scanner {
public:
    explicit Scanner(kimix::string_view command);
    Scanner(kimix::string_view command, kimix::string_view temp_dir);
    ~Scanner();
    Scanner(const Scanner &) = delete;
    Scanner &operator=(const Scanner &) = delete;
    Scanner(Scanner &&) noexcept;
    Scanner &operator=(Scanner &&) noexcept;

    // Port of `_BashFixScanner.fix()`: scan, early-out on no activity,
    // prefix + render + heredoc repair, complete BashFix.
    BashFix fix();

    // `_scan_range(0, n)` with the explicit-frame depth budget.  Runs to
    // completion (the internal worklist drains every queued nested frame).
    void scan_all();

    // Port of `_scan_range(start, end)` as a command context.
    void scan_range(size_t start, size_t end);

    // Port of `_build_source()`: the source with all recorded edits applied
    // (render_splices: sorted one-pass splice).  Returns the original bytes
    // when no edits exist.
    kimix::string build_source() const;

    // Port of `_build_fixed_source(source)`: heredoc trailing-operator repair
    // LAST; `reuse_events` mirrors the reference's `source is self.s` fast
    // path (event positions were collected against this exact string).
    kimix::string build_fixed_source(const kimix::string &source, bool reuse_events) const;

    ScanReports &reports() { return reports_; }
    const ScanReports &reports() const { return reports_; }
    kimix::string_view source() const { return s_; }
    size_t length() const { return n_; }
    const kimix::string &temp_dir() const { return temp_dir_; }
    // Current nesting depth (the reference's `nest_depth` budget counter).
    int32_t nest_depth() const;

    // ── reader primitives (byte-for-byte ports of the scanner methods) ────

    // `_read_word(start, end, scan_substitutions)`: end index of the word at
    // `start` (== start when no word).  scan_substitutions=true recursively
    // scans $()/backtick contents as command contexts (recording their edits
    // and reports); false only walks boundaries.
    size_t read_word(size_t start, size_t end, bool scan_substitutions = true);

    // `_literal_word_value`: static quote-removal decode (ANSI-C aware).
    static kimix::optional<kimix::string> literal_word_value(kimix::string_view raw);

    // `_redirection_after_fd(i, end)`.
    bool redirection_after_fd_at(size_t i, size_t end) const;

    // `_find_matching(i, end, ')')`: the matching paren position, or `end`
    // when unmatched (depth budget respected).
    size_t find_matching(size_t i, size_t end);

    // `_heredoc_delimiter(raw)`.  The outer optional is nullopt only for an
    // empty raw word; a malformed ANSI-C delimiter yields a nullopt
    // `delimiter` inside (unmatchable: no body is claimed).
    struct HeredocDelimiter {
        kimix::optional<kimix::string> delimiter;
        bool expands = true;
    };
    static kimix::optional<HeredocDelimiter> heredoc_delimiter(kimix::string_view raw);

    // `_skip_heredoc_bodies(i, end, docs, scan_expansions)`: claim pending
    // heredoc bodies starting at i; returns the new index and appends the
    // (redir_line_end, terminator_end) event.
    struct PendingHeredoc {
        kimix::optional<kimix::string> delimiter;
        bool strip_tabs = false;
        bool expands = true;
    };
    size_t skip_heredoc_bodies(size_t i, size_t end, const kimix::vector<PendingHeredoc> &documents, bool scan_expansions = true);

    // `_read_control_operator` (scanner flavor; no parens).
    std::pair<kimix::string_view, size_t> read_shell_control_operator_at(size_t i, size_t end) const;

    // `_comment_starts`.
    bool comment_starts_at(size_t i, size_t range_start) const { return comment_starts(s_, i, range_start); }

    // `_empty_parentheses_end(i, end)`: index after "()" or nullopt.
    kimix::optional<size_t> empty_parentheses_end(size_t i, size_t end) const;

    // `_function_declaration_end(raw, i, end)`.
    kimix::optional<size_t> function_declaration_end(kimix::string_view raw, size_t i, size_t end) const;

    // `_coproc_name_before_compound(raw, i, end)`.
    bool coproc_name_before_compound(kimix::string_view raw, size_t i, size_t end) const;

    // `_path_replacement(raw)` with this scanner's temp dir.
    kimix::optional<kimix::string> path_replacement_for(kimix::string_view raw) const;

    // ── report recorders (exact reference semantics) ───────────────────────

    void record_edit(size_t start, size_t end, kimix::string replacement);
    void record_name(const kimix::string &name); // duplicates kept (prefix dedups)
    void record_path(const kimix::string &raw);
    void record_shell(const kimix::string &name);
    void record_nul(const kimix::string &raw);
    void record_cmd_conditional(const kimix::string &note);
    // `_record_unsupported`: first-occurrence dedup; returns true when the
    // name is an unsupported table key (`decoded` may carry the already
    // decoded literal; nullopt-with-flag decodes `raw` — see bp_scanner.cpp
    // `UndecodedTag`).
    bool record_unsupported(kimix::string_view raw, const kimix::optional<kimix::string> &decoded, bool decoded_valid);

    // Merge an inner scanner's reports with the LIVE rules (names/shell/
    // unsupported dedup against the OUTER list at merge time; path/nul/cmd
    // notes extend verbatim).
    void merge_inner(const Scanner &inner);

    // ── nested-script repair entry points (bash_core methods) ─────────────

    // `_handle_shell_wrapper`: returns (scan_index, keep_wrapper) or nullopt
    // when the wrapper is left untouched.
    std::optional<std::pair<size_t, bool>> handle_shell_wrapper(const kimix::string &shell_name, size_t word_start, size_t i, size_t end, bool assignment_prefix, bool wrapped);

    // `_fix_shell_c_script_in_place(i, end)`: index after the repaired script
    // word, or nullopt.
    kimix::optional<size_t> fix_shell_c_script_in_place(size_t i, size_t end);

    // `_inline_script_operand(word_start, word_end, raw)`: true when the word
    // was consumed as an inline script.
    bool inline_script_operand(size_t word_start, size_t word_end, kimix::string_view raw);

    // `_harvest_nonliteral_inline_script(raw)`.
    void harvest_nonliteral_inline_script(kimix::string_view raw);

    // `_try_at_prefix(word_start, word_end, end)`: (index, mode) or nullopt;
    // mode is "statement" or "strip" (see cmd_translator.at_prefix_rule).
    struct AtPrefixResult {
        size_t index = 0;
        bool statement = false;
    };
    kimix::optional<AtPrefixResult> try_at_prefix(size_t word_start, size_t word_end, size_t end);

    // `_try_cmd_if(if_start, if_end, end)` / `_try_cmd_for(...)`: index past
    // the translated construct, or nullopt (no edits recorded).
    kimix::optional<size_t> try_cmd_if(size_t if_start, size_t if_end, size_t end);
    kimix::optional<size_t> try_cmd_for(size_t for_start, size_t for_end, size_t end);

    // `_record_cmd_jump(raw, literal, word_end, end)`: true when a jump was
    // refused through the unsupported channel.  `literal_valid` says whether
    // `literal` holds a decoded value (nullopt literal is allowed).
    bool record_cmd_jump(kimix::string_view raw, const kimix::optional<kimix::string> &literal, bool literal_valid, size_t word_end, size_t end);

    // `_peek_literal_word(pos, end)`: decoded literal of the word at pos
    // (horizontal whitespace only), or nullopt.
    kimix::optional<kimix::string> peek_literal_word(size_t pos, size_t end);

    // `_drop_cmd_cd_flag(i, end)`.
    void drop_cmd_cd_flag(size_t i, size_t end);

private:
    struct Impl;

    // ── internal word/region skip helpers (ports of the scanner methods) ──
    size_t skip_single_quote(size_t i, size_t end) const;
    size_t skip_ansi_quote(size_t i, size_t end) const;
    size_t skip_double_quote_scan(size_t i, size_t end);       // `_skip_double_quote` (scans substitutions)
    size_t skip_double_quote_for_matching(size_t i, size_t end); // `_skip_double_quote_for_matching`
    size_t skip_parameter(size_t i, size_t end);               // `_skip_parameter`
    size_t skip_parameter_literal(size_t i, size_t end) const; // `_skip_parameter_literal`
    size_t skip_arithmetic(size_t i, size_t end);              // `_skip_arithmetic`
    size_t skip_conditional(size_t i, size_t end);             // `_skip_conditional` ([[ ... ]])
    size_t scanner_find_backtick_end(size_t i, size_t end) const; // `_find_backtick_end`
    void scan_expansions(size_t i, size_t end);                // `_scan_expansions`
    void scan_heredoc_expansions(size_t i, size_t end);        // `_scan_heredoc_expansions`
    void scan_array_words(size_t i, size_t end);               // `_scan_array_words`
    size_t find_matching_impl(size_t i, size_t end);           // `_find_matching` (depth-budgeted)
    size_t find_matching_inner(size_t i, size_t end);          // `_find_matching_inner`
    void scan_range_inner(size_t start, size_t end);           // `_scan_range_inner` (the scan flow)
    void read_redirection_at(size_t i, size_t end, kimix::string_view *op_out, size_t *after_out) const; // `_read_redirection`

    // `_classify_pattern_option` state step used by the argument-position scan.
    static kimix::optional<kimix::string_view> classify_pattern_option_for(kimix::string_view family, kimix::string_view word);

    kimix::unique_ptr<Impl> impl_;
    kimix::string owned_; // owns the command bytes when constructed from a view
    kimix::string_view s_;
    size_t n_ = 0;
    kimix::string temp_dir_;
    ScanReports reports_;
};

} // namespace kimix::llm::bash_parser
