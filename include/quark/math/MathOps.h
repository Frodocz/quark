#pragma once

#include "quark/Attributes.h"

#include <cassert>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

// constexpr numeric building blocks shared by DoubleOps, ScaleInt and anything else.
//
//   * no <cmath> needed (std::abs etc. are only constexpr in C++23)
//   * floating point helpers use ordering comparisons only, clean under -Wfloat-equal
//   * integer helpers have no undefined behaviour anywhere in the range of T, including
//     T's minimum (whose negation does not fit): see uabs, abs and the *_overflow helpers
//   * the division helpers take a positive divisor; when it is a compile time constant (or
//     becomes one after inlining) the compiler replaces the ~40 cycle 64-bit idiv with a
//     multiply and shift, or a mask/shift for powers of two

QUARK_BEGIN_NAMESPACE

namespace math {

// 2^63 == -static_cast<double>(INT64_MIN). Note static_cast<double>(INT64_MAX) rounds up to this
// value as well. Every double in the open interval (-2^63, 2^63) truncates to a valid int64_t.
inline constexpr double TWO_POW_63 = 9223372036854775808.0;

// ---- generic --------------------------------------------------------------------------------

template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T max(T a, T b) noexcept {
    return a < b ? b : a;
}

template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T min(T a, T b) noexcept {
    return b < a ? b : a;
}

// ---- floating point ---------------------------------------------------------------------------

template <std::floating_point T>
[[nodiscard]] constexpr T abs(T x) noexcept {
    return x < T{0} ? -x : x;
}

// False for NaN and +-infinity. Uses only ordering comparisons (no ==, !=), so it is clean
// under -Wfloat-equal, and no arithmetic on inf/NaN, so it is valid in constant evaluation
// (except for a NaN argument: GCC does not treat comparisons with NaN as constant).
template <std::floating_point T>
[[nodiscard]] constexpr bool is_finite(T x) noexcept {
    return x >= std::numeric_limits<T>::lowest() && x <= std::numeric_limits<T>::max();
}

// ---- integer: overflow aware arithmetic -------------------------------------------------------
// Return true on overflow. The result is always stored, wrapped modulo 2^N, so it is well defined
// either way; with the return value unused they compile to the plain instruction.

template <std::integral T>
[[nodiscard]] constexpr bool add_overflow(T a, T b, T &out) noexcept {
    return __builtin_add_overflow(a, b, &out);
}

template <std::integral T>
[[nodiscard]] constexpr bool sub_overflow(T a, T b, T &out) noexcept {
    return __builtin_sub_overflow(a, b, &out);
}

template <std::integral T>
[[nodiscard]] constexpr bool mul_overflow(T a, T b, T &out) noexcept {
    return __builtin_mul_overflow(a, b, &out);
}

// |x| as the unsigned type: defined for every x, uabs(INT64_MIN) == 2^63.
template <std::signed_integral T>
[[nodiscard]] constexpr std::make_unsigned_t<T> uabs(T x) noexcept {
    using U = std::make_unsigned_t<T>;
    return x < 0 ? static_cast<U>(U{0} - static_cast<U>(x)) : static_cast<U>(x);
}

// |x|. abs(min) is not representable: asserts in debug builds, returns min (wraps) in release.
template <std::signed_integral T>
[[nodiscard]] constexpr T abs(T x) noexcept {
    assert(x != std::numeric_limits<T>::min() && "abs(min) overflows");
    T out{};
    (void)sub_overflow(T{0}, x, out);
    return x < 0 ? out : x;
}

// ---- integer: powers ------------------------------------------------------------------------

// 10^e, e must be in [0, digits10] (e.g. [0, 18] for int64_t).
template <std::integral T = int64_t>
[[nodiscard]] constexpr T ipow10(int e) noexcept {
    assert(e >= 0 && e <= std::numeric_limits<T>::digits10);
    T r = 1;
    for (int i = 0; i < e; ++i) {
        r *= 10;
    }
    return r;
}

// k if v == 10^k, otherwise -1.
template <std::integral T>
[[nodiscard]] constexpr int exact_log10(T v) noexcept {
    if (v <= 0) {
        return -1;
    }
    int k = 0;
    for (; v % 10 == 0; v /= 10) {
        ++k;
    }
    return v == 1 ? k : -1;
}

template <std::integral T>
[[nodiscard]] constexpr bool is_pow2(T v) noexcept {
    return v > 0 && (v & (v - 1)) == 0;
}

// ---- integer: division by a positive divisor ----------------------------------------------------
// All are exact and overflow free for every v (d > 0). q and r come from a single division, and
// the adjustments are branch free (setcc / sign mask), so random signs cost no mispredictions.

// floor(v / d)
template <std::signed_integral T>
[[nodiscard]] constexpr T floor_div(T v, T d) noexcept {
    assert(d > 0);
    return static_cast<T>(v / d - static_cast<T>(v % d < 0));
}

// ceil(v / d)
template <std::signed_integral T>
[[nodiscard]] constexpr T ceil_div(T v, T d) noexcept {
    assert(d > 0);
    return static_cast<T>(v / d + static_cast<T>(v % d > 0));
}

// v / d rounded to nearest, ties away from zero. No v +- d/2 offset (which overflows near
// the limits) and no negation of v.
template <std::signed_integral T>
[[nodiscard]] constexpr T div_round_half_away(T v, T d) noexcept {
    assert(d > 0);
    const T q = v / d;
    const T r = v % d;
    const T abs_r = r < 0 ? -r : r;                                          // |r| < d, cannot overflow
    const T away = static_cast<T>((v >> (std::numeric_limits<T>::digits)) | 1);  // -1 or +1 (C++20 shift)
    // 2 * |r| >= d, written so it cannot overflow
    return static_cast<T>(q + static_cast<T>(abs_r >= d - abs_r) * away);
}

namespace detail {

// q * d, or the next multiple toward zero when q * d does not fit in T. That only happens when
// rounding a value within one d of T's limits, and (q -+ 1) * d then always fits.
template <std::signed_integral T>
[[nodiscard]] constexpr T mul_or_step_toward_zero(T q, T d) noexcept {
    T out{};
    if (mul_overflow(q, d, out)) [[unlikely]] {
        return (q < 0 ? q + 1 : q - 1) * d;
    }
    return out;
}

}  // namespace detail

// Largest multiple of d that is <= v (see detail::mul_or_step_toward_zero near the limits).
template <std::signed_integral T>
[[nodiscard]] constexpr T floor_to_multiple(T v, T d) noexcept {
    return detail::mul_or_step_toward_zero(floor_div(v, d), d);
}

// Smallest multiple of d that is >= v (see detail::mul_or_step_toward_zero near the limits).
template <std::signed_integral T>
[[nodiscard]] constexpr T ceil_to_multiple(T v, T d) noexcept {
    return detail::mul_or_step_toward_zero(ceil_div(v, d), d);
}

// Nearest multiple of d, ties away from zero (see detail::mul_or_step_toward_zero near the limits).
template <std::signed_integral T>
[[nodiscard]] constexpr T round_to_multiple(T v, T d) noexcept {
    return detail::mul_or_step_toward_zero(div_round_half_away(v, d), d);
}

template <std::signed_integral T>
[[nodiscard]] constexpr bool is_multiple_of(T v, T d) noexcept {
    assert(d > 0);
    return v % d == 0;
}

}  // namespace math

QUARK_END_NAMESPACE
