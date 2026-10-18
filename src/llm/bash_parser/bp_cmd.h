/* bp_cmd.h — cmd.exe translations: leading-@ rules, `if`/`for` control-flow
 * translation, and the untranslatable jump refusals.
 *
 * Ports of kimi-agent `shell_compat/cmd_translator.py` (the LIVE-path
 * functions the scanner delegates to: at_prefix_rule, at_boundary_ok,
 * split_top_eq, cmd_cmp_side, build_cmd_cmp, cmd_if_branch_ok,
 * validate_cmd_if/commit_cmd_if, validate_cmd_for/commit_cmd_for,
 * peek_literal_word, cmd_jump_record/record_cmd_jump).  The IR-dispatcher-only
 * rule wrappers (rule_cmd_at/rule_cmd_control/rule_cmd_jump and the
 * CmdIfNode/CmdForNode statements) are covered by the scanner flow — see the
 * module README exclusions.
 *
 * Validation passes are strictly read-only (they record NO edits); committing
 * a validated match appends disjoint edits + path notes to the reader and
 * scans branch bodies recursively through `Scanner::scan_range`.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

class Scanner;

// ── leading-@ rule ──────────────────────────────────────────────────────────

// `AtPrefixDecision`: index to continue the dispatch loop at; mode
// ("statement"/"strip"); the (start, end, replacement) splice; the
// cmd_conditional note (statement mode only).
struct AtPrefixDecision {
    size_t index = 0;
    bool statement = false; // mode == "statement"
    size_t edit_start = 0;
    size_t edit_end = 0;
    kimix::string edit_replacement;
    kimix::optional<kimix::string> note;
};

// `at_prefix_rule`: decide the leading-@ command-word form; strictly
// read-only.  nullopt on every documented bail.
kimix::optional<AtPrefixDecision> at_prefix_rule(Scanner &reader, kimix::string_view source, size_t word_start, size_t word_end, size_t end);

// `at_boundary_ok`: pure source predicate.
bool at_boundary_ok(kimix::string_view source, size_t pos, size_t end);

// ── if-comparison primitives ────────────────────────────────────────────────

// `split_top_eq`: split a glued cmd LHS==RHS word on the first top-level ==.
kimix::optional<std::pair<kimix::string, kimix::string>> split_top_eq(kimix::string_view raw);

// `cmd_cmp_side`: (bash_operand, was_cmd_quoted) for one compare side.
struct CmpSide {
    kimix::string operand;
    bool was_cmd_quoted = false;
};
kimix::optional<CmpSide> cmd_cmp_side(kimix::string_view raw, bool folded, bool numeric);

// `build_cmd_cmp`: the bash condition text for a compare/arithmetic pair.
kimix::optional<kimix::string> build_cmd_cmp(kimix::string_view lhs_raw, kimix::string_view rhs_raw, kimix::string_view op, bool folded, bool numeric);

// `cmd_if_branch_ok`: whether the branch body [start, stop) is translatable.
bool cmd_if_branch_ok(kimix::string_view source, size_t start, size_t stop);

// ── cmd `if` validate + commit ──────────────────────────────────────────────

// `_CmdIfSegment` port.  kind: 0=exist 1=defined 2=errorlevel 3=compare
// 4=arithmetic.
enum class CmdIfKind { kExist = 0, kDefined, kErrorlevel, kCompare, kArithmetic };

struct CmdIfSegment {
    size_t header_start = 0;
    bool negated = false;
    size_t op_start = 0;
    size_t op_end = 0;
    size_t open_pos = 0;
    size_t close_pos = 0;
    CmdIfKind kind = CmdIfKind::kExist;
    bool folded = false;
    kimix::optional<kimix::string> cmp_text; // compare/arithmetic only
};

// `_CmdIfMatch` port.
struct CmdIfMatch {
    kimix::vector<CmdIfSegment> segments;
    kimix::optional<size_t> else_open;
    kimix::optional<size_t> else_close;
};

// `validate_cmd_if`: read-only shape validation; nullopt on any deviation.
kimix::optional<CmdIfMatch> validate_cmd_if(Scanner &reader, kimix::string_view source, size_t if_start, size_t pos, size_t end);

// `commit_cmd_if`: emit the disjoint edits, scan branch bodies; returns the
// index just past the whole conditional.
size_t commit_cmd_if(Scanner &reader, kimix::string_view source, const CmdIfMatch &match);

// ── cmd `for` validate + commit ─────────────────────────────────────────────

// `cmd_for_item_ok`.
bool cmd_for_item_ok(kimix::string_view item);

// `cmd_body_has_bare_ampersand`.
bool cmd_body_has_bare_ampersand(kimix::string_view body);

struct CmdForListEdits {
    kimix::vector<std::pair<size_t, size_t>> comma_spans;
    struct PathEdit {
        size_t start = 0;
        size_t end = 0;
        kimix::string replacement;
    };
    kimix::vector<PathEdit> list_path_edits;
};

// `validate_cmd_for_list`: nullopt bails the whole loop.
kimix::optional<CmdForListEdits> validate_cmd_for_list(Scanner &reader, kimix::string_view source, size_t i, size_t close);

// `validate_cmd_for_body`: whole-word %name/%%name spans, or nullopt.
kimix::optional<kimix::vector<std::pair<size_t, size_t>>> validate_cmd_for_body(Scanner &reader, kimix::string_view source, size_t start, size_t stop, kimix::string_view name);

// `_CmdForMatch` port.
struct CmdForMatch {
    size_t for_start = 0;
    kimix::string name;
    size_t tok_start = 0;
    size_t tok_end = 0;
    size_t open_pos = 0;
    size_t close_pos = 0;
    size_t do_end = 0;
    size_t body_start = 0;
    size_t body_end = 0;
    kimix::vector<std::pair<size_t, size_t>> comma_spans;
    kimix::vector<CmdForListEdits::PathEdit> list_path_edits;
    kimix::vector<std::pair<size_t, size_t>> var_edits;
};

// `validate_cmd_for`: read-only; nullopt on any deviation.
kimix::optional<CmdForMatch> validate_cmd_for(Scanner &reader, kimix::string_view source, size_t for_start, size_t pos, size_t end);

// `commit_cmd_for`: returns the index just past the whole loop.
size_t commit_cmd_for(Scanner &reader, kimix::string_view source, const CmdForMatch &match);

// ── jump refusals ───────────────────────────────────────────────────────────

// `peek_literal_word`: decoded literal of the word at pos, or nullopt.
kimix::optional<kimix::string> peek_literal_word(Scanner &reader, kimix::string_view source, size_t pos, size_t end);

// `cmd_jump_record`: the refusal key ("goto"/"call :label"/"exit /b") or
// nullopt.  `literal` is the already-decoded command-word value (may be
// nullopt — then `raw` is used, matching the reference isinstance test).
kimix::optional<kimix::string_view> cmd_jump_record(Scanner &reader, kimix::string_view source, kimix::string_view raw, const kimix::optional<kimix::string> &literal, bool literal_valid, size_t word_end, size_t end);

} // namespace kimix::llm::bash_parser
