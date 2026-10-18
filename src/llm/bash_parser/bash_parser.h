/* bash_parser.h — Windows Git Bash compatibility kernel (kimix::llm::bash_parser).
 *
 * Native C++ port of the authoritative Python reference
 * (kimi-agent `bin/kimix_native/shell_compat/*`, behind the `_shell_compat`
 * facade).  It rewrites selected native POSIX / cmd.exe command shapes for
 * Windows Git Bash: guarded fallback command definitions, Windows path
 * spellings (backslashes, `C:\`, UNC, `/c/...`, `/tmp/...`), the `nul`
 * null-device redirection leak, redundant `bash`/`sh` wrapper unwrapping,
 * runtime-reparsed inline scripts (`eval`/`trap`/`watch`/`-c`/`find -exec`),
 * cmd.exe `@echo`/`if`/`for` control-flow translation, heredoc trailing
 * operator repair, and unsupported-command refusals.
 *
 * Public surface (the only stable API of this module):
 *
 *   BashFix fix_bash_command(kimix::string_view command);
 *   BashFix fix_bash_command(kimix::string_view command, const FixOptions&);
 *   kimix::string bash_compatibility_prelude();
 *   kimix::string process_unquoted(kimix::string_view command);
 *
 * The kernel always applies the Windows Git Bash transformation (no
 * app-layer OS gate) — exactly like the reference `bash_core.fix_bash_command`.
 * Byte contract: an unchanged result returns the ORIGINAL bytes verbatim
 * (embedded NUL and non-ASCII bytes are never truncated or normalized);
 * report lists keep source-discovery order; `unsupported` alone does not make
 * `changed()` true; warning text matches the reference wording byte-for-byte.
 * Malformed, ambiguous, or over-budget input always takes the conservative
 * pass-through path — this API never fails on user input.
 */
#pragma once

#include <core/dll_export.h>
#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

// Explicit fix-time configuration.  The reference resolves the Windows temp
// directory through `tempfile.gettempdir()`; `temp_dir` overrides that
// spelling (used for deterministic `/tmp` tests and by callers that already
// know the host temp dir).  Empty `temp_dir` = host discovery.
struct KIMIX_LLM_API FixOptions {
    // Forward-slash Windows spelling of the host temp directory that Git
    // Bash's `/tmp` maps to (e.g. `C:/Users/x/AppData/Local/Temp`).  Backslash
    // spellings are normalized internally.  Empty: discover from the
    // environment (TEMP/TMP), non-throwing.
    kimix::string temp_dir;
};

// Result of fix_bash_command — the value-type equivalent of the reference's
// frozen `BashFix` dataclass.  Read-only accessors, value equality; the
// internal builder lives in bash_parser.cpp.
class KIMIX_LLM_API BashFix {
public:
    BashFix() = default;
    explicit BashFix(kimix::string command);
    BashFix(kimix::string command, kimix::vector<kimix::string> replacements, kimix::vector<kimix::string> path_changes, kimix::vector<kimix::string> shell_wrappers, kimix::vector<kimix::string> nul_fixes, kimix::vector<kimix::string> unsupported, kimix::vector<kimix::string> cmd_conditional_fixes);

    // The (possibly rewritten) command text.  Unchanged results carry the
    // original bytes verbatim.
    const kimix::string &command() const { return command_; }

    // Each original fallback command NAME in source-discovery order
    // (repeated discoveries are retained; the prefix dedups separately).
    const kimix::vector<kimix::string> &replacements() const { return replacements_; }

    // Each original argument/command word whose Windows path spelling was
    // rewritten, in source-discovery order.
    const kimix::vector<kimix::string> &path_changes() const { return path_changes_; }

    // Each redundant bash/sh invocation that was unwrapped.
    const kimix::vector<kimix::string> &shell_wrappers() const { return shell_wrappers_; }

    // Each redirection target whose effective value was `nul`, rewritten to
    // `/dev/null`.
    const kimix::vector<kimix::string> &nul_fixes() const { return nul_fixes_; }

    // Command names with no faithful Windows Git Bash equivalent
    // (first-occurrence deduplicated; alone they do NOT make changed() true).
    const kimix::vector<kimix::string> &unsupported() const { return unsupported_; }

    // Original keyword region of each translated cmd.exe control-flow
    // construct.
    const kimix::vector<kimix::string> &cmd_conditional_fixes() const { return cmd_conditional_fixes_; }

    // True when any compatibility replacement was made: fallback
    // replacements, path changes, shell unwrapping, nul fixes, or cmd
    // conditional fixes (NOT unsupported alone — matches the reference).
    bool changed() const;

    // Concise compatibility-change description; sections ordered:
    // unsupported, fallbacks, paths, shell wrappers, nul, cmd control flow.
    // Empty when nothing was reported.
    kimix::string warning() const;

    bool operator==(const BashFix &other) const;
    bool operator!=(const BashFix &other) const { return !(*this == other); }

private:
    kimix::string command_;
    kimix::vector<kimix::string> replacements_;
    kimix::vector<kimix::string> path_changes_;
    kimix::vector<kimix::string> shell_wrappers_;
    kimix::vector<kimix::string> nul_fixes_;
    kimix::vector<kimix::string> unsupported_;
    kimix::vector<kimix::string> cmd_conditional_fixes_;
};

// Full Git Bash transformation of `command` (no app-layer OS gate).
// Never throws; malformed input passes through unchanged.
KIMIX_LLM_API BashFix fix_bash_command(kimix::string_view command);
KIMIX_LLM_API BashFix fix_bash_command(kimix::string_view command, const FixOptions &options);

// All fallback definitions plus conditional `export -f` lines for a
// persistent interactive Git Bash shell (empty on non-Windows hosts,
// matching the reference platform behavior).
KIMIX_LLM_API kimix::string bash_compatibility_prelude();

// The separate Bash unquoted-backslash transform (`_process_unquoted`):
// converts unquoted backslashes to forward slashes while preserving
// metacharacter escapes and quoted regions.
KIMIX_LLM_API kimix::string process_unquoted(kimix::string_view command);

} // namespace kimix::llm::bash_parser
