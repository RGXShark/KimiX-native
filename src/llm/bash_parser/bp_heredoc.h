/* bp_heredoc.h — heredoc trailing-operator repair (kimi-agent
 * `shell_compat/heredoc.py` LIVE path: `_apply_heredoc_operator_move` and
 * `_fix_heredoc_trailing_operators`).
 *
 * Applied LAST in the fix pipeline.  On the unchanged fast path the events
 * collected by the scan are reused (their positions index the original
 * string); when the source was edited the repair rescans (events = nullopt)
 * exactly like the reference.  The IR-side event model
 * (collect_heredoc_events / fix_heredoc_trailing_operators_ir) is part of the
 * IR layer port; see the module README.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

class Scanner;

// `_HEREDOC_TRAILING_OPERATORS` membership: && || | |& ; &
bool is_heredoc_trailing_operator(kimix::string_view op);

// `_apply_heredoc_operator_move(source, redir_line_end, terminator_end)`:
// move a control-operator line following a heredoc terminator up onto the
// redirection line (multi-line operator runs, backslash continuations,
// comment lines and the construct-closer "stay" guards included).  Pure
// function; returns the source unchanged when the pattern does not apply.
kimix::string apply_heredoc_operator_move(kimix::string_view source, int64_t redir_line_end, int64_t terminator_end);

// `_fix_heredoc_trailing_operators(source, events)`: apply the repair to
// every event in reverse order.  `events` nullopt = rescan the source with a
// fresh Scanner (legacy fallback path; a fresh scanner's reports are
// discarded — only its heredoc_events are used).
kimix::string fix_heredoc_trailing_operators(kimix::string_view source, const kimix::optional<kimix::vector<std::pair<int64_t, int64_t>>> &events);

// `_BashFixScanner._heredoc_line_continues`: odd number of trailing
// backslashes.
bool heredoc_line_continues(kimix::string_view line);

} // namespace kimix::llm::bash_parser
