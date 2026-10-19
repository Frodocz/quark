#pragma once

// Fixed-point decimal stored as an int64_t "raw" count of 10^-SCALE units.
//
// Conventions
//   * double -> ScaleInt rounds to the nearest unit, ties away from zero (symmetric).
//   * Inputs must be finite and |v| < MAX_ABS_DOUBLE. Violations assert in debug builds and
//     are undefined (float->int overflow) in release; use try_from_double() on untrusted input.
//   * A double carries ~15 significant decimal digits, so double <-> ScaleInt round trips are
//     exact only while |raw| < 1e15. Keep hot-path state in raw integers and convert at the edges.
//   * Integer arithmetic (+ - * unary minus, abs, rescale) has no undefined behaviour: overflow,
//     including negating INT64_MIN, asserts in debug builds and wraps (two's complement) in
//     release, at the cost of a plain add/sub/imul.
//   * Tick rounding never overflows: within one tick of the int64 limits the result is the
//     nearest representable multiple toward zero.
//   * Formatting is integer only (std::to_chars), independent of fmt / std::format.
//
// Tick rounding cost: a 64-bit division by a runtime tick is ~40 cycles. Use, from fastest:
//   * floor_to_tick<TICK_RAW>()  tick known at compile time: multiply + shift (any constant)
//   * floor_to_tick(TickSize)    tick converted once per instrument; power of ten ticks (0.01,
//                                1e-8, ...) dispatch to a compile-time divisor, others use idiv
//   * floor_to_tick_raw(int64_t) runtime integer tick: one idiv
//   * floor_to_tick(double)      converts the tick on every call

#include "quark/Attributes.h"
#include "quark/math/DoubleOps.h"
#include "quark/math/MathOps.h"

#include <cassert>
#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

QUARK_BEGIN_NAMESPACE

namespace math {

struct SmallStrBuf {
    constexpr SmallStrBuf() { data_[0] = '\0'; }
    const char *c_str() const noexcept { return data_; }
    char data_[64];
};

template <int SCALE>
struct ScaleInt;

// A positive tick size in raw units of ScaleInt<SCALE>, converted once (e.g. per instrument).
// Remembers whether the tick is a power of ten in raw units, which covers most exchange ticks
// (0.01 at E8 is 10^6, 1e-8 at E8 is 1), so the rounding can use a compile-time divisor.
template <int SCALE>
class TickSize {
public:
    // tick must be at least one unit of ScaleInt<SCALE> (asserts in debug builds)
    constexpr explicit TickSize(double tick) noexcept : TickSize(ScaleInt<SCALE>::tick_to_raw(tick), Raw{}) {}

    [[nodiscard]] static constexpr TickSize from_raw(int64_t tick_raw) noexcept { return TickSize(tick_raw, Raw{}); }

    [[nodiscard]] constexpr int64_t raw() const noexcept { return raw_; }

    // k if raw() == 10^k, otherwise -1
    [[nodiscard]] constexpr int pow10_exponent() const noexcept { return pow10_exponent_; }

    // Calls f(d) where d is the raw tick: a std::integral_constant<int64_t, 10^k> when the tick
    // is a power of ten (the divisor is then a compile time constant once f is inlined), the
    // int64_t value otherwise. Branches only on pow10_exponent(), which is perfectly predictable.
    template <class F>
    QUARK_ALWAYS_INLINE constexpr auto visit(F&& f) const noexcept {
        return visit_pow10(f, std::make_index_sequence<std::numeric_limits<int64_t>::digits10 + 1>{});
    }

private:
    struct Raw {};

    constexpr TickSize(int64_t tick_raw, Raw) noexcept : raw_(tick_raw), pow10_exponent_(exact_log10(tick_raw)) {
        assert(tick_raw > 0 && "tick must be positive and at least one unit of this scale");
    }

    template <class F, std::size_t... K>
    QUARK_ALWAYS_INLINE constexpr auto visit_pow10(F& f, std::index_sequence<K...>) const noexcept {
        decltype(f(int64_t{})) out{};
        const bool is_pow10 =
            ((pow10_exponent_ == static_cast<int>(K) &&
              (out = f(std::integral_constant<int64_t, ipow10<int64_t>(static_cast<int>(K))>{}), true)) || ...);
        return is_pow10 ? out : f(raw_);
    }

    int64_t raw_;
    int pow10_exponent_;
};

template <int SCALE>
struct ScaleInt {
public:
    static_assert(SCALE >= 1 && SCALE <= 13, "ScaleInt only support SCALE in range [1, 13]");

    using SelfT = ScaleInt<SCALE>;

    static constexpr int64_t SCALE_AS_INT = ipow10<int64_t>(SCALE);
    static constexpr uint64_t SCALE_AS_UINT = static_cast<uint64_t>(SCALE_AS_INT);
    static constexpr double SCALE_AS_DOUBLE = static_cast<double>(SCALE_AS_INT);  // exact: 10^13 < 2^53
    static constexpr double TICK = 1.0 / SCALE_AS_DOUBLE;
    // Every v with |v| < MAX_ABS_DOUBLE has |v * SCALE_AS_DOUBLE| < 2^63 after rounding, so the
    // conversion fits in int64_t (checked for every SCALE in the tests). The bound itself may scale
    // to exactly 2^63 and is excluded, which is why every check is a strict "<".
    static constexpr double MAX_ABS_DOUBLE = TWO_POW_63 / SCALE_AS_DOUBLE;
    // sign + up to 19 integer digits + '.' + SCALE fraction digits
    static constexpr std::size_t MAX_CHARS = 1 + 19 + 1 + SCALE;
    static_assert(MAX_CHARS < sizeof(SmallStrBuf::data_));

    static constexpr int get_scale() noexcept { return SCALE; }

    // ---- construction / conversion -------------------------------------------------------

    ScaleInt() = default;

    constexpr explicit ScaleInt(double v) noexcept : v_(to_raw(v)) {}

    [[nodiscard]] static constexpr SelfT from_raw(int64_t raw) noexcept {
        SelfT ret;
        ret.v_ = raw;
        return ret;
    }

    // Round to nearest unit, ties away from zero. Branch-free (compare + select); no tolerance
    // check is needed, anything closer to zero than half a unit rounds to 0 anyway.
    [[nodiscard]] static constexpr int64_t to_raw(double v) noexcept {
        assert(math::abs(v) < MAX_ABS_DOUBLE && "not finite or out of range");  // NaN fails too
        return static_cast<int64_t>(v * SCALE_AS_DOUBLE + (v < 0.0 ? -0.5 : 0.5));
    }

    // Checked conversion for untrusted input: NaN, inf and out of range values return false.
    [[nodiscard]] static constexpr bool try_from_double(double v, SelfT& out) noexcept {
        if (!(math::abs(v) < MAX_ABS_DOUBLE)) {  // written so NaN fails
            return false;
        }
        out.v_ = to_raw(v);
        return true;
    }

    // Division (not multiplication by TICK) so the result is the double nearest to the decimal value.
    [[nodiscard]] constexpr double as_double() const noexcept { return static_cast<double>(v_) / SCALE_AS_DOUBLE; }

    [[nodiscard]] constexpr int64_t get_raw() const noexcept { return v_; }

    constexpr void set_raw(int64_t raw) noexcept { v_ = raw; }

    constexpr SelfT& operator=(double v) noexcept {
        v_ = to_raw(v);
        return *this;
    }

    // Widening is exact (overflow asserts / wraps, see the conventions); narrowing rounds half
    // away from zero.
    template <int NEW_SCALE>
    [[nodiscard]] constexpr ScaleInt<NEW_SCALE> rescale() const noexcept {
        if constexpr (NEW_SCALE == SCALE) {
            return ScaleInt<NEW_SCALE>::from_raw(v_);
        } else if constexpr (NEW_SCALE > SCALE) {
            return ScaleInt<NEW_SCALE>::from_raw(mul_raw(v_, ipow10<int64_t>(NEW_SCALE - SCALE)));
        } else {
            return ScaleInt<NEW_SCALE>::from_raw(div_round_half_away(v_, ipow10<int64_t>(SCALE - NEW_SCALE)));
        }
    }

    // ---- tick rounding ---------------------------------------------------------------------
    // round: nearest multiple, ties away from zero (symmetric for negative values)
    // floor: toward -infinity (e.g. buy price, order quantity)
    // ceil:  toward +infinity (e.g. sell price)

    [[nodiscard]] static constexpr int64_t tick_to_raw(double tick) noexcept {
        const int64_t t = to_raw(tick);
        assert(t > 0 && "tick must be positive and at least one unit of this scale");
        return t;
    }

    // Tick known at compile time, in raw units: e.g. floor_to_tick<1'000'000>() is 0.01 at E8.
    template <int64_t TICK_RAW>
    constexpr SelfT& round_to_tick() noexcept {
        static_assert(TICK_RAW > 0);
        v_ = round_to_multiple(v_, TICK_RAW);
        return *this;
    }

    template <int64_t TICK_RAW>
    constexpr SelfT& floor_to_tick() noexcept {
        static_assert(TICK_RAW > 0);
        v_ = floor_to_multiple(v_, TICK_RAW);
        return *this;
    }

    template <int64_t TICK_RAW>
    constexpr SelfT& ceil_to_tick() noexcept {
        static_assert(TICK_RAW > 0);
        v_ = ceil_to_multiple(v_, TICK_RAW);
        return *this;
    }

    template <int64_t TICK_RAW>
    [[nodiscard]] constexpr bool is_multiple_of_tick() const noexcept {
        static_assert(TICK_RAW > 0);
        return is_multiple_of(v_, TICK_RAW);
    }

    // Tick converted once (e.g. per instrument).
    constexpr SelfT& round_to_tick(const TickSize<SCALE>& tick) noexcept {
        v_ = tick.visit([v = v_](auto d) { return round_to_multiple(v, static_cast<int64_t>(d)); });
        return *this;
    }

    constexpr SelfT& floor_to_tick(const TickSize<SCALE>& tick) noexcept {
        v_ = tick.visit([v = v_](auto d) { return floor_to_multiple(v, static_cast<int64_t>(d)); });
        return *this;
    }

    constexpr SelfT& ceil_to_tick(const TickSize<SCALE>& tick) noexcept {
        v_ = tick.visit([v = v_](auto d) { return ceil_to_multiple(v, static_cast<int64_t>(d)); });
        return *this;
    }

    [[nodiscard]] constexpr bool is_multiple_of_tick(const TickSize<SCALE>& tick) const noexcept {
        return tick.visit([v = v_](auto d) { return is_multiple_of(v, static_cast<int64_t>(d)); });
    }

    // Runtime integer tick (tick_raw > 0): one 64-bit division.
    constexpr SelfT& round_to_tick_raw(int64_t tick_raw) noexcept {
        v_ = round_to_multiple(v_, tick_raw);
        return *this;
    }

    constexpr SelfT& floor_to_tick_raw(int64_t tick_raw) noexcept {
        v_ = floor_to_multiple(v_, tick_raw);
        return *this;
    }

    constexpr SelfT& ceil_to_tick_raw(int64_t tick_raw) noexcept {
        v_ = ceil_to_multiple(v_, tick_raw);
        return *this;
    }

    [[nodiscard]] constexpr bool is_multiple_of_tick_raw(int64_t tick_raw) const noexcept {
        return is_multiple_of(v_, tick_raw);
    }

    // Convenience overloads converting the tick on every call. A tick that is finer than this
    // type's resolution (or not positive) leaves the value unchanged instead of dividing by zero.
    constexpr SelfT& round_to_tick(double tick) noexcept {
        const int64_t tick_raw = to_raw(tick);
        return tick_raw > 0 ? round_to_tick_raw(tick_raw) : *this;
    }

    constexpr SelfT& floor_to_tick(double tick) noexcept {
        const int64_t tick_raw = to_raw(tick);
        return tick_raw > 0 ? floor_to_tick_raw(tick_raw) : *this;
    }

    constexpr SelfT& ceil_to_tick(double tick) noexcept {
        const int64_t tick_raw = to_raw(tick);
        return tick_raw > 0 ? ceil_to_tick_raw(tick_raw) : *this;
    }

    // ---- ScaleInt <op> ScaleInt: exact integer arithmetic ----------------------------------

    constexpr bool operator==(const SelfT&) const noexcept = default;
    constexpr auto operator<=>(const SelfT&) const noexcept = default;

    [[nodiscard]] constexpr SelfT operator+(const SelfT rhs) const noexcept { return from_raw(add_raw(v_, rhs.v_)); }
    [[nodiscard]] constexpr SelfT operator-(const SelfT rhs) const noexcept { return from_raw(sub_raw(v_, rhs.v_)); }
    [[nodiscard]] constexpr SelfT operator-() const noexcept { return from_raw(sub_raw(0, v_)); }
    [[nodiscard]] constexpr SelfT operator*(int64_t k) const noexcept { return from_raw(mul_raw(v_, k)); }
    [[nodiscard]] friend constexpr SelfT operator*(int64_t k, const SelfT x) noexcept { return x * k; }

    constexpr SelfT& operator+=(const SelfT rhs) noexcept {
        v_ = add_raw(v_, rhs.v_);
        return *this;
    }

    constexpr SelfT& operator-=(const SelfT rhs) noexcept {
        v_ = sub_raw(v_, rhs.v_);
        return *this;
    }

    // abs of INT64_MIN does not fit: asserts in debug builds, stays INT64_MIN in release.
    [[nodiscard]] constexpr SelfT abs() const noexcept { return from_raw(math::abs(v_)); }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return v_ == 0; }

    // ---- ScaleInt <op> double: the double is ROUNDED to this scale first ------------------
    // Convenient at the edges, but each call pays a multiply + round, and comparisons are lossy:
    // ScaleInt<2>(0.12) == 0.1249 is true. Keep these out of tight loops and exact checks.

    [[nodiscard]] constexpr bool operator==(double rhs) const noexcept { return v_ == to_raw(rhs); }
    [[nodiscard]] constexpr auto operator<=>(double rhs) const noexcept { return v_ <=> to_raw(rhs); }

    [[nodiscard]] constexpr SelfT operator+(double rhs) const noexcept { return from_raw(add_raw(v_, to_raw(rhs))); }
    [[nodiscard]] constexpr SelfT operator-(double rhs) const noexcept { return from_raw(sub_raw(v_, to_raw(rhs))); }

    constexpr SelfT& operator+=(double rhs) noexcept {
        v_ = add_raw(v_, to_raw(rhs));
        return *this;
    }

    constexpr SelfT& operator-=(double rhs) noexcept {
        v_ = sub_raw(v_, to_raw(rhs));
        return *this;
    }

    // ---- formatting ------------------------------------------------------------------------

    // Shortest exact decimal ("-12.034", "5", "0.0001"). Writes at most MAX_CHARS bytes, no NUL.
    // Returns one past the last written character. Integer only, no allocation.
    char* to_chars(char* first) const noexcept {
        const uint64_t abs_v = uabs(v_);  // well defined for INT64_MIN
        if (v_ < 0) {
            *first++ = '-';
        }
        first = std::to_chars(first, first + 20, abs_v / SCALE_AS_UINT).ptr;
        uint64_t rem = abs_v % SCALE_AS_UINT;
        if (rem != 0) {
            *first++ = '.';
            // write all SCALE digits right to left (fixed trip count, unrolled by the compiler),
            // then drop the trailing zeros; rem != 0 so at least one digit is not '0'
            for (int i = SCALE - 1; i >= 0; --i) {
                first[i] = static_cast<char>('0' + rem % 10);
                rem /= 10;
            }
            first += SCALE;
            while (first[-1] == '0') {
                --first;
            }
        }
        return first;
    }

    void to_string(char (&dst)[64]) const noexcept { *to_chars(dst) = '\0'; }

    [[nodiscard]] SmallStrBuf to_string() const noexcept {
        SmallStrBuf buffer;
        to_string(buffer.data_);
        return buffer;
    }

private:
    // Overflow asserts in debug builds and wraps in release (never UB); a plain add/sub/imul.
    static constexpr int64_t add_raw(int64_t a, int64_t b) noexcept {
        int64_t out = 0;
        [[maybe_unused]] const bool overflow = add_overflow(a, b, out);
        assert(!overflow && "ScaleInt overflow");
        return out;
    }

    static constexpr int64_t sub_raw(int64_t a, int64_t b) noexcept {
        int64_t out = 0;
        [[maybe_unused]] const bool overflow = sub_overflow(a, b, out);
        assert(!overflow && "ScaleInt overflow");
        return out;
    }

    static constexpr int64_t mul_raw(int64_t a, int64_t b) noexcept {
        int64_t out = 0;
        [[maybe_unused]] const bool overflow = mul_overflow(a, b, out);
        assert(!overflow && "ScaleInt overflow");
        return out;
    }

    int64_t v_ = 0;
};

using DoubleE4 = ScaleInt<4>;
using DoubleE6 = ScaleInt<6>;
using DoubleE8 = ScaleInt<8>;
using DoubleE9 = ScaleInt<9>;
using DoubleE13 = ScaleInt<13>;

// A value below the pivot is handled with the matching scale. Each pivot keeps
// |v| * 10^SCALE <= 1e15 (integer digits + SCALE <= 15), where conversion is exact.
constexpr double E13_PIVOT = 10.0;
constexpr double E9_PIVOT = 1.0e6;
constexpr double E6_PIVOT = 1.0e9;
// E4 is only exact below this; double_to_string falls back to std::to_chars above it
constexpr double E4_PIVOT = 1.0e11;
constexpr double MAX_SUPPORT_DOUBLE = DoubleE4::MAX_ABS_DOUBLE;

namespace detail {

// Calls f with a default constructed ScaleInt of the scale chosen for abs_x (E4 for anything
// at or above E6_PIVOT, including NaN). All branches must return the same type.
// Callers must make sure the value converted is finite and below MAX_SUPPORT_DOUBLE.
template <class F>
QUARK_ALWAYS_INLINE constexpr auto visit_scale(double abs_x, F&& f) {
    if (abs_x < E13_PIVOT) {
        return f(DoubleE13{});
    } else if (abs_x < E9_PIVOT) {
        return f(DoubleE9{});
    } else if (abs_x < E6_PIVOT) {
        return f(DoubleE6{});
    } else {
        return f(DoubleE4{});
    }
}

}  // namespace detail

QUARK_ALWAYS_INLINE SmallStrBuf double_to_string(double x) noexcept {
    const double abs_x = math::abs(x);
    if (abs_x < E4_PIVOT) {
        return detail::visit_scale(abs_x, [x](auto s) { return decltype(s)(x).to_string(); });
    }
    // try the best to use general conversion: shortest round trip, also covers NaN and inf
    SmallStrBuf buf;
    const auto buf_size = sizeof(buf.data_);
    const auto result = std::to_chars(buf.data_, buf.data_ + buf_size - 1, x);
    if (result.ec == std::errc()) {
        *result.ptr = '\0';
    } else {
        std::strncpy(buf.data_, "NOT_SUPPORT", buf_size);
    }
    return buf;
}

// NaN, infinities and |x| >= MAX_SUPPORT_DOUBLE are returned unchanged.
QUARK_ALWAYS_INLINE constexpr double round_double(double x) noexcept {
    const double abs_x = math::abs(x);
    if (!(abs_x < MAX_SUPPORT_DOUBLE)) {
        return x;
    }
    return detail::visit_scale(abs_x, [x](auto s) { return decltype(s)(x).as_double(); });
}

// Rounds x with the precision chosen by the magnitude of base_val (or of x, if larger,
// so the scaled value cannot overflow). Non-finite or too large x is returned unchanged.
QUARK_ALWAYS_INLINE constexpr double round_double(double base_val, double x) noexcept {
    const double abs_x = math::abs(x);
    if (!(abs_x < MAX_SUPPORT_DOUBLE)) {
        return x;
    }
    // max(a, b) is a < b ? b : a, so with abs_x first a NaN base_val keeps abs_x
    return detail::visit_scale(math::max(abs_x, math::abs(base_val)),
                               [x](auto s) { return decltype(s)(x).as_double(); });
}

// NaN, infinities and |x| >= MAX_SUPPORT_DOUBLE are returned unchanged.
// Prefer ScaleInt::round_to_tick_raw / TickSize with a pre-converted tick in hot paths.
QUARK_ALWAYS_INLINE constexpr double round_to_tick(double x, double tick) noexcept {
    const double abs_x = math::abs(x);
    if (!(abs_x < MAX_SUPPORT_DOUBLE)) {
        return x;
    }
    return detail::visit_scale(abs_x, [x, tick](auto s) {
        decltype(s) v(x);
        return v.round_to_tick(tick).as_double();
    });
}

// {raw_value, SCALE}. x must be finite with |x| < MAX_SUPPORT_DOUBLE.
QUARK_ALWAYS_INLINE constexpr std::pair<int64_t, int> round_double_get_raw(double x) noexcept {
    return detail::visit_scale(math::abs(x), [x](auto s) {
        using T = decltype(s);
        return std::make_pair(T(x).get_raw(), T::get_scale());
    });
}

// Decimal grid equality, unlike the tolerance based math::equals (DoubleOps.h): x and y are equal
// when they round to the same value at the scale chosen by the larger magnitude (13 decimals
// below 10, 9 below 1e6, ...). Within one scale this is an equivalence relation (transitive),
// which tolerance based equality is not. Values on either side of a pivot (e.g. 9.99999999999999
// and 10) still compare equal. NaN, infinities and huge values compare exactly.
// For an absolute epsilon use math::equals(x, y, {.rel = 0.0, .abs = epsilon}).
QUARK_ALWAYS_INLINE constexpr bool equals_rounded(double x, double y) noexcept {
    const double abs_max = math::max(math::abs(x), math::abs(y));
    if (!is_finite(x) || !is_finite(y) || !(abs_max < MAX_SUPPORT_DOUBLE)) {
        return equals(x, y, {.rel = 0.0});  // exact; NaN never equal
    }
    return detail::visit_scale(abs_max, [x, y](auto s) { return decltype(s)(x) == decltype(s)(y); });
}

} // namespace math

QUARK_END_NAMESPACE

template <int S>
struct std::hash<::quark::math::ScaleInt<S>> {
    std::size_t operator()(const ::quark::math::ScaleInt<S>& x) const noexcept {
        return std::hash<int64_t>{}(x.get_raw());
    }
};
