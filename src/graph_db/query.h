/*
 * query.h — constraint evaluation and the two-phase page contract (GDI §2.6).
 *
 * Two things live here, both pure functions over already-copied data (no locks,
 * no graph access — the Database does the locking):
 *
 *   matches(...)  the AND-ed condition evaluation of Semevia.Gdi's
 *                 InMemoryStore.Matches, over a label set and a property bag.
 *   Page<T>       the GDI page shape: `items` (at most `cap` rows) plus
 *                 `total_count` (the FULL match count). cap == 0 yields no items
 *                 and the real count — that is how CountNeighbors counts without
 *                 pulling a neighbourhood, and how the C ABI's two-phase buffer
 *                 protocol learns how big a buffer it needs.
 *
 * EVALUATION RULES (copied from the reference, not invented):
 *   - a LABEL condition tests membership; only equal/not_equal are meaningful and
 *     any other op is rejected when the condition is ADDED (Schema: error_op),
 *     because a filter that silently evaluated `greater` on a label would lie;
 *   - a PROPERTY condition needs the property to be present and then compares via
 *     property.h's evaluate() (numeric cross-kind ordering, ordinal strings,
 *     equality-only for blob/arrays);
 *   - the conditions are AND-ed; an empty constraint matches everything.
 *
 * Example:
 *   kimix::graph_db::Constraint constraint = kimix::graph_db::Constraint{}.with(
 *       kimix::graph_db::ConstraintCondition::on_label(person, kimix::graph_db::GdiOp::equal));
 *   kimix::graph_db::EdgePage page;
 *   db.get_edges_of_vertex(uid, constraint, kimix::graph_db::EdgeOrientation::outgoing,
 *                          0, page);          // page.total_count is the match count
 */
#pragma once

#include "graph_db_api.h"
#include "property.h"
#include "schema.h"

#include <core/guid.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

// One page of a capped read. `total_count` is always the FULL match count, so a
// caller can size its buffer from a cap-0 call and then fetch with cap ==
// total_count (GDI's own size-query/fetch idiom).
template<typename T>
struct Page {
    kimix::vector<T> items;
    size_t total_count{0};

    [[nodiscard]] bool empty() const noexcept { return items.empty(); }
};

using VertexPage = Page<kimix::Guid>;
using EdgePage = Page<EdgeRecord>;

/*
 * Fill one page from an already collected match list: `items` gets the first
 * min(cap, matches) entries, `total_count` the full size. `matches` is consumed
 * (moved from) so a large neighbourhood is not copied twice.
 */
template<typename T>
[[nodiscard]] Page<T> make_page(kimix::vector<T> &&matches, int64_t cap) {
    Page<T> page;
    page.total_count = matches.size();
    if (cap < 0) {
        return page; // the caller validates the cap; a negative one yields nothing
    }
    size_t const take = static_cast<size_t>(cap);
    if (take == 0) {
        return page; // count probe: no items, the real total
    }
    if (matches.size() <= take) {
        page.items = std::move(matches);
        return page;
    }
    page.items.reserve(take);
    for (size_t i = 0; i < take; ++i) {
        page.items.push_back(std::move(matches[i]));
    }
    return page;
}

// Does an entity with these labels/properties satisfy the constraint?
// `properties` must be sorted by name for the binary search used below... it is
// not: the bag is small and a linear scan is what the reference does, so a linear
// scan it is (documented: a condition list is short, a property bag is short).
[[nodiscard]] KIMIX_GRAPH_DB_API bool
matches(Constraint const &constraint, kimix::span<const LabelId> labels,
        PropertyList const &properties) noexcept;

// The same test over a PropertyBag (the vertex-side storage form).
[[nodiscard]] KIMIX_GRAPH_DB_API bool
matches(Constraint const &constraint, kimix::span<const LabelId> labels,
        PropertyBag const &properties) noexcept;

// Convenience: does the label set contain `label`? (linear; a label set is tiny)
[[nodiscard]] KIMIX_GRAPH_DB_API bool
has_label(kimix::span<const LabelId> labels, LabelId label) noexcept;

} // namespace kimix::graph_db
