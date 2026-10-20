/*
 * compare.h — kimix::compare<T> three-way comparison functor.
 *
 * A strcmp-style ordering functor: returns a value < 0, == 0, or > 0
 * depending on whether a orders before, equal to, or after b. Used as the
 * default ordering for kimix::HashMap's per-bucket trees and available for
 * any container that needs a three-way ordering callback.
 *
 *   kimix::compare<int> cmp;
 *   cmp(1, 2);   // < 0
 *   cmp(2, 2);   // == 0
 *   cmp(3, 2);   // > 0
 *
 * Specialize kimix::compare<T> for types that need a custom ordering
 * (see guid.h for an example).
 */
#pragma once

#include <cstdint>
#include <type_traits>

namespace kimix {

// Three-way comparison functor: returns < 0, 0, or > 0, mirroring
// strcmp semantics. Arithmetic and pointer types order by value/address;
// enums order by underlying value; everything else uses operator== /
// operator< (so those must exist for T).
template <typename T>
struct compare {
    template <typename V>
    int32_t operator()(T const &a, V const &b) const {
        if constexpr (std::is_enum_v<T>) {
            if (a == b) return 0;
            return static_cast<int32_t>(static_cast<std::underlying_type_t<T>>(a) < static_cast<std::underlying_type_t<T>>(b) ? -1 : 1);
        } else if constexpr (std::is_arithmetic_v<T> || std::is_pointer_v<T>) {
            return static_cast<int32_t>((a > b) - (a < b));
        } else {
            if (a == b) return 0;
            return a < b ? -1 : 1;
        }
    }
};

} // namespace kimix
