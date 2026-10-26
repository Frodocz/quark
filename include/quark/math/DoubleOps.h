#pragma once

#include "quark/Attributes.h"
#include "quark/math/MathOps.h"

#include <concepts>
#include <limits>

// Tolerance-based comparison for floating point values (model outputs, analytics).
// Do NOT use these for prices or quantities: keep those in ScaledInt / tick indices
// and compare integers exactly.
//
// Semantics (for non-NaN inputs exactly one of greater / less_than / equals is true):
//   equals(a, b)           a == b, or |a - b| <= max(tol.abs, tol.rel * max(|a|, |b|))
//   greater(a, b)          a > b  and not equals(a, b)
//   less_than(a, b)        a < b  and not equals(a, b)
//   greater_or_equal(a, b) a > b  or  equals(a, b)
//   less_or_equal(a, b)    a < b  or  equals(a, b)
//   is_zero(x)             |x| <= tol.abs      (tol.rel is meaningless against zero)
//
// Properties:
//   * constexpr and noexcept; built on the helpers in MathOps.h, no <cmath> needed
//   * never uses == or != on floating point, so it builds under -Wfloat-equal -Werror
//   * every predicate is false if either argument is NaN
//   * infinities compare equal only to the same infinity, never to a finite value
//   * -0.0 == +0.0
//   * the relative part scales with magnitude (unlike a fixed absolute epsilon)
//   * like every tolerance-based equality, "equals" is NOT transitive
//
// Do not compile with -ffast-math / -Ofast: it lets the compiler assume no NaN or inf,
// which removes the guards below.

QUARK_BEGIN_NAMESPACE

namespace math {

template <std::floating_point T>
struct Tolerance {
    // Relative tolerance: a few ulps by default.
    T rel = T{4} * std::numeric_limits<T>::epsilon();
    // Absolute tolerance: 0 by default. Set it explicitly (domain-specific) when values
    // can be close to zero, e.g. results of subtractions: Tolerance<double>{.abs = 1e-13}
    T abs = T{0};
};

template <std::floating_point T>
[[nodiscard]] constexpr bool is_zero(T x, Tolerance<T> tol = {}) noexcept {
    return math::abs(x) <= tol.abs;
}

template <std::floating_point T>
[[nodiscard]] constexpr bool equals(T a, T b, Tolerance<T> tol = {}) noexcept {
    if (!math::is_finite(a) || !math::is_finite(b)) {
        // NaN, or at least one infinity: equal only if both are the same infinity.
        // (NaN fails every ordering comparison, so it is never equal to anything.)
        constexpr T kMax = std::numeric_limits<T>::max();
        return (a > kMax && b > kMax) || (a < -kMax && b < -kMax);
    }
    // Never compute a - b directly: it can overflow (e.g. 1e308 - -1e308), which is
    // harmless at run time but not a constant expression. Work on magnitudes instead.
    const T abs_a = math::abs(a);
    const T abs_b = math::abs(b);
    const T hi = math::max(abs_a, abs_b);
    const T lo = math::min(abs_a, abs_b);
    const T threshold = math::max(tol.abs, tol.rel * hi);
    if ((a < T{0}) == (b < T{0})) {
        return hi - lo <= threshold;  // same sign (+-0 included): |a - b| == ||a| - |b||
    }
    return abs_a <= threshold - abs_b;  // opposite signs: |a - b| == |a| + |b|
}

template <std::floating_point T>
[[nodiscard]] constexpr bool greater(T a, T b, Tolerance<T> tol = {}) noexcept {
    return a > b && !equals(a, b, tol);
}
 
template <std::floating_point T>
[[nodiscard]] constexpr bool less_than(T a, T b, Tolerance<T> tol = {}) noexcept {
    return a < b && !equals(a, b, tol);
}
 
template <std::floating_point T>
[[nodiscard]] constexpr bool greater_or_equal(T a, T b, Tolerance<T> tol = {}) noexcept {
    return a > b || equals(a, b, tol);
}
 
template <std::floating_point T>
[[nodiscard]] constexpr bool less_or_equal(T a, T b, Tolerance<T> tol = {}) noexcept {
    return a < b || equals(a, b, tol);
}

} // namespace math

QUARK_END_NAMESPACE
