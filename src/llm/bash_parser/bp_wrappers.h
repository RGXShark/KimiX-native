/* bp_wrappers.h — compatibility classification, wrapper state machine, and
 * redundant-shell unwrap rules.  Ports of kimi-agent
 * `shell_compat/wrappers.py` (the LIVE-path decisions; the IR-dispatcher-only
 * wrappers of that module are covered by the scanner flow itself — see the
 * module README's exclusion list).
 *
 * Rule functions take a `Scanner&` as their "reader" where the reference
 * takes the duck-typed `reader` (the live scanner passes `self`), so word
 * boundaries are byte-identical.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

class Scanner;

// ── wrapper state machine (`_BashWrapper` / `wrapper_step`) ────────────────

// Port of `_BashWrapper` (same field names/semantics).
struct WrapperState {
    kimix::string kind;
    bool skip_next = false;
    bool opaque = false;
    bool path_value = false;
    int32_t operands = 0;
};

// `wrapper_step` actions (identical contract to `_consume_wrapper_word`).
enum class WrapperAction {
    kSkip,    // word consumed as wrapper data
    kInspect, // wrapper state ends; the word is still worth decoding (command -v)
    kCommand, // the wrapped command word
};

// Byte-for-byte port of `wrappers.wrapper_step`.
WrapperAction wrapper_step(WrapperState &state, kimix::string_view raw);

// `wrappers.wrapper_path_inline_replacement`: rewrite an inline
// `--chdir=PATH`-style option value of a path wrapper.  Returns the
// replacement text, empty string when the arm fired with no path change, or
// nullopt when the arm does not apply.
kimix::optional<kimix::string> wrapper_path_inline_replacement(WrapperState &state, kimix::string_view raw, const Scanner &scanner);

// ── pattern/program operand suppression (grep/sed/awk/rg) ──────────────────

// `classify_pattern_option`: "data-pending" / "path-pending" /
// "value-pending" / "data" / "path" / nullopt.
kimix::optional<kimix::string_view> classify_pattern_option(kimix::string_view family, kimix::string_view word);

// ── runtime-reparsed scripts ────────────────────────────────────────────────

// `is_inline_script_word`: a quoted inline-script operand (leading quote).
bool is_inline_script_word(kimix::string_view raw);

// `shell_c_script_range`: (script_start, script_end) of an option-validated
// `-c`-family inline script following a shell word, or nullopt.
kimix::optional<std::pair<size_t, size_t>> shell_c_script_range(Scanner &reader, kimix::string_view source, size_t start, size_t end);

// ── redundant-shell unwrap decision (`unwrap_decision`) ────────────────────

enum class UnwrapAction {
    kKeep,      // leave the wrapper byte-for-byte
    kUnwrap,    // drop the redundant shell word
    kFixScript, // [-l] -c script shape: wrapper stays, script repaired in place
};

// Port of `wrappers.UnwrapDecision`.
struct UnwrapDecision {
    UnwrapAction action = UnwrapAction::kKeep;
    bool keep_wrapper = false;
    std::optional<std::pair<size_t, size_t>> script_span; // (start, end)
    bool harvest = false;
    kimix::optional<size_t> scan_index;
};

// `plausible_script_file`: byte-for-byte port.
bool plausible_script_file(kimix::string_view raw);

// `unwrap_decision`: byte-for-byte port (the edit/note/merge tail stays in
// the scanner's `handle_shell_wrapper`).
UnwrapDecision unwrap_decision(Scanner &reader, kimix::string_view source, size_t start, size_t end, bool assignment_prefix, bool wrapped);

} // namespace kimix::llm::bash_parser
