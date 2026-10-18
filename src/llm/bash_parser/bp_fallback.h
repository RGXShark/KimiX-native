/* bp_fallback.h — generated fallback data + definition/prefix helpers.
 *
 * Ports of kimi-agent `shell_compat/fallback_data.py` (tables generated into
 * fallback_data.inc by scripts/gen_bash_fix_data.py --bash-parser-data; never
 * hand-transcribed) and the `wrappers.py` prefix generators.
 *
 * Static data is immutable tables/string views with no dynamic-initialization
 * dependency; runtime strings are composed with kimix containers.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

// `_FALLBACK_BODIES` lookup: the raw fallback body of a registered name
// (including the g* GNU aliases and the netcat/htop shares), or nullopt.
kimix::optional<kimix::string_view> fallback_body(kimix::string_view name);

// `_FALLBACKS` lookup: the runtime-guarded definition of a registered name,
// or nullopt.  (Generated: `_fallback_definition(name)`.)
kimix::optional<kimix::string_view> fallback_definition(kimix::string_view name);

// True when `name in _FALLBACKS` (the scanner's fallback-registration test).
bool is_fallback_name(kimix::string_view name);

// `_UNSUPPORTED_BODIES` lookup: the refusal reason, or nullopt.
kimix::optional<kimix::string_view> unsupported_reason(kimix::string_view name);

// `_FALLBACK_COMMAND_WRAPPERS` lookup (gtimeout->timeout, watch->watch,
// sudo->sudo), or nullopt.
kimix::optional<kimix::string_view> fallback_command_wrapper_kind(kimix::string_view name);

// `_wrapper_runner(name)`: the standalone executable runner rewrite
// ("/usr/bin/bash -c '<definition>; name "$@"' --").
kimix::string wrapper_runner(kimix::string_view name);

// `wrappers.fallback_prefix(names)`: dedup in discovery order, one guarded
// definition + one conditional `export -f` line per unique name, terminated
// by a newline; empty when no names.
kimix::string fallback_prefix(const kimix::vector<kimix::string> &names);

// `wrappers.export_lines(names)`: the conditional export lines (deduped).
kimix::vector<kimix::string> export_lines(const kimix::vector<kimix::string> &names);

// `bash_compatibility_prelude()` body (all definitions + all exports; empty
// on non-Windows hosts, matching the reference sys.platform gate).
kimix::string compatibility_prelude();

} // namespace kimix::llm::bash_parser
