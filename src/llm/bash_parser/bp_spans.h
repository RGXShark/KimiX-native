/* bp_spans.h — spans, edits, and diagnostics: the coordinate layer of the
 * lossless shell IR (kimi-agent `shell_compat/spans.py`).
 *
 * Half-open [start, end) UTF-8 byte offsets into the single source string
 * owned by the root ScriptNode.  Python `ValueError` programming-error
 * signaling (negative/disjoint join, overlapping sorted plans) becomes bool /
 * optional returns here (no exceptions in this project); malformed USER input
 * never reaches those paths — the parser is conservative by construction.
 */
#pragma once

#include <core/kimix_core.h>

namespace kimix::llm::bash_parser {

// `SourceSpan`: immutable [start, end) region.
struct SourceSpan {
    size_t start = 0;
    size_t end = 0;

    size_t length() const { return end - start; }
    kimix::string_view text(kimix::string_view source) const { return source.substr(start, end - start); }
    bool empty() const { return start == end; }
    bool adjacent_to(const SourceSpan &other) const { return end == other.start || other.end == start; }
    bool overlaps(const SourceSpan &other) const { return start < other.end && other.start < end; }
    bool contains(const SourceSpan &other) const { return start <= other.start && other.end <= end; }
    SourceSpan union_with(const SourceSpan &other) const {
        return SourceSpan{start < other.start ? start : other.start, end > other.end ? end : other.end};
    }
    // `join`: union of adjacent/overlapping spans; false when disjoint.
    bool join(const SourceSpan &other, SourceSpan &out) const {
        if (end < other.start || other.end < start) {
            return false;
        }
        out = union_with(other);
        return true;
    }
    bool operator==(const SourceSpan &other) const { return start == other.start && end == other.end; }
};

// `Edit`: a replacement splice in original-source coordinates.
struct Edit {
    size_t start = 0;
    size_t end = 0;
    kimix::string replacement;

    SourceSpan span() const { return SourceSpan{start, end}; }
    bool adjacent_to(const Edit &other) const { return end == other.start || other.end == start; }
    bool overlaps(const Edit &other) const { return start < other.end && other.start < end; }
};

// `Edit::sorted_disjoint`: sort by (start, end); false when any edit overlaps
// its predecessor (a malformed plan is never rendered — the reference raises;
// the port declines).
bool sorted_disjoint_edits(kimix::vector<Edit> edits, kimix::vector<Edit> &out);

// `DiagnosticKind`.
enum class DiagnosticKind {
    kUnterminatedQuote,
    kUnterminatedSubst,
    kUnterminatedHeredoc,
    kDepthLimit,
    kAmbiguousRegion,
    kUnexpectedToken,
};

// `Diagnostic`: an internal parse note; data, never an error.
struct Diagnostic {
    DiagnosticKind kind = DiagnosticKind::kAmbiguousRegion;
    SourceSpan span;
    kimix::string message;
};

} // namespace kimix::llm::bash_parser
