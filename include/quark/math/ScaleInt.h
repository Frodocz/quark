#pragma once

// Fixed-point decimal stored as a "raw" count of 10^-SCALE units, in an int64_t (default) or an int128_t.
//
// Representation
//   * ScaleInt<SCALE> (Rep = int64_t): about 19 significant digits, SCALE in [1, 13].
//   * ScaleInt<SCALE, int128_t>: about 38 significant digits, SCALE in [1, 18], for values whose range exceeds
//     int64 (e.g. crypto prices from 1e-13 to 1e9). Add, subtract and compare cost two instructions instead
//     of one; the API is the same.
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
//   * Parsing (from_chars / parse) is exact: decimal text converts without going through double, and text
//     that needs more than SCALE fraction digits or doesn't fit is rejected, never rounded or wrapped.
//   * ScaleInt x ScaleInt and ScaleInt / ScaleInt are the free functions mul() and div(), exact up to one
//     rounding with an explicit Rounding mode; they return false instead of overflowing.
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

#include <array>
#include <cassert>
#include <charconv>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
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

template <int SCALE, class Rep = int64_t>
struct ScaleInt;

// Rounding of mul() and div() and of narrowing conversions
enum class Rounding : uint8_t {
    HalfAway,    // nearest, ties away from zero (symmetric)
    Floor,       // toward -infinity
    Ceil,        // toward +infinity
    TowardZero,  // truncation
};

namespace detail {

// v / d rounded as requested, d > 0. Exact, no overflow for any v.
template <SignedInteger T>
[[nodiscard]] constexpr T div_rounded(T v, T d, Rounding r) noexcept {
    switch (r) {
        case Rounding::Floor:
            return floor_div(v, d);
        case Rounding::Ceil:
            return ceil_div(v, d);
        case Rounding::TowardZero:
            return static_cast<T>(v / d);
        case Rounding::HalfAway:
        default:
            return div_round_half_away(v, d);
    }
}

}  // namespace detail

// A positive tick size in raw units of ScaleInt<SCALE, Rep>, converted once (e.g. per instrument).
// Remembers whether the tick is a power of ten in raw units, which covers most exchange ticks
// (0.01 at E8 is 10^6, 1e-8 at E8 is 1), so the rounding can use a compile-time divisor.
template <int SCALE, class Rep = int64_t>
class TickSize {
public:
    // tick must be at least one unit of ScaleInt<SCALE> (asserts in debug builds). In release
    // builds a tick below one unit becomes one unit, so rounding leaves values unchanged (as the
    // per-call double overloads do) instead of dividing by zero. Use try_from_double() for ticks
    // read from configuration or an exchange.
    using ValueT = ScaleInt<SCALE, Rep>;

    constexpr explicit TickSize(double tick) noexcept : TickSize(ValueT::tick_to_raw(tick), Raw{}) {}

    [[nodiscard]] static constexpr TickSize from_raw(Rep tick_raw) noexcept { return TickSize(tick_raw, Raw{}); }

    // Checked conversion from a value (e.g. parsed exactly from an exchange's "0.0000000000001"): false
    // unless the tick is positive.
    [[nodiscard]] static constexpr bool try_from_value(ValueT tick, TickSize& out) noexcept {
        if (tick.get_raw() <= 0) {
            return false;
        }
        out = TickSize(tick.get_raw(), Raw{});
        return true;
    }

    // Checked conversion: false when the tick is not finite, not positive, finer than one unit of
    // this scale, or not a whole number of units (1.5e-8 at E8 would silently become 1e-8).
    [[nodiscard]] static constexpr bool try_from_double(double tick, TickSize& out) noexcept {
        if (!(tick > 0.0 && tick < ValueT::MAX_ABS_DOUBLE)) {  // written so NaN fails
            return false;
        }
        const double scaled = tick * ValueT::SCALE_AS_DOUBLE;
        const Rep raw = ValueT::to_raw(tick);
        // a tick parsed from decimal text scales to within a few ulps of a whole number
        if (raw <= 0 || math::abs(scaled - static_cast<double>(raw)) > 1e-6 * math::max(scaled, 1.0)) {
            return false;
        }
        out = TickSize(raw, Raw{});
        return true;
    }

    [[nodiscard]] constexpr Rep raw() const noexcept { return raw_; }

    // The tick as a value of its ScaleInt type
    [[nodiscard]] constexpr ValueT value() const noexcept { return ValueT::from_raw(raw_); }

    // k if raw() == 10^k, otherwise -1
    [[nodiscard]] constexpr int pow10_exponent() const noexcept { return pow10_exponent_; }

    // Calls f(d) where d is the raw tick: a std::integral_constant<Rep, 10^k> when the tick is a
    // power of ten up to 10^18 (the divisor is then a compile time constant once f is inlined), the
    // Rep value otherwise. Branches only on pow10_exponent(), which is perfectly predictable.
    template <class F>
    QUARK_ALWAYS_INLINE constexpr auto visit(F&& f) const noexcept {
        return visit_pow10(f, std::make_index_sequence<std::numeric_limits<int64_t>::digits10 + 1>{});
    }

private:
    struct Raw {};

    constexpr TickSize(Rep tick_raw, Raw) noexcept
        : raw_(tick_raw > 0 ? tick_raw : 1), pow10_exponent_(exact_log10(raw_)) {
        assert(tick_raw > 0 && "tick must be positive and at least one unit of this scale");
    }

    template <class F, std::size_t... K>
    QUARK_ALWAYS_INLINE constexpr auto visit_pow10(F& f, std::index_sequence<K...>) const noexcept {
        decltype(f(Rep{})) out{};
        const bool is_pow10 =
            ((pow10_exponent_ == static_cast<int>(K) &&
              (out = f(std::integral_constant<Rep, ipow10<Rep>(static_cast<int>(K))>{}), true)) || ...);
        return is_pow10 ? out : f(raw_);
    }

    Rep raw_;
    int pow10_exponent_;
};

template <int SCALE, class Rep>
struct ScaleInt {
public:
    static_assert(std::same_as<Rep, int64_t> || std::same_as<Rep, int128_t>,
                  "ScaleInt stores its raw value in an int64_t or an int128_t");
    static constexpr int MAX_SCALE = std::same_as<Rep, int64_t> ? 13 : 18;
    static_assert(SCALE >= 1 && SCALE <= MAX_SCALE,
                  "ScaleInt supports SCALE in [1, 13] with int64_t and in [1, 18] with int128_t");

    using SelfT = ScaleInt<SCALE, Rep>;
    using RepT = Rep;
    using URepT = make_unsigned_t<Rep>;

    static constexpr Rep SCALE_AS_INT = ipow10<Rep>(SCALE);
    static constexpr URepT SCALE_AS_UINT = static_cast<URepT>(SCALE_AS_INT);
    static constexpr double SCALE_AS_DOUBLE = static_cast<double>(SCALE_AS_INT);  // exact: 10^18 < 2^53 * 2^18
    static constexpr double TICK = 1.0 / SCALE_AS_DOUBLE;
    // Every v with |v| < MAX_ABS_DOUBLE has |v * SCALE_AS_DOUBLE| < 2^(N-1) after rounding, so the
    // conversion fits in Rep (checked for every SCALE in the tests). The bound itself may scale
    // to exactly 2^(N-1) and is excluded, which is why every check is a strict "<".
    static constexpr double MAX_ABS_DOUBLE =
        (std::same_as<Rep, int64_t> ? TWO_POW_63 : TWO_POW_63 * TWO_POW_63 * 2.0) / SCALE_AS_DOUBLE;
    // sign + up to digits10 + 1 integer digits + '.' + SCALE fraction digits
    static constexpr std::size_t MAX_CHARS = 1 + (std::numeric_limits<Rep>::digits10 + 1) + 1 + SCALE;
    static_assert(MAX_CHARS < sizeof(SmallStrBuf::data_));

    static constexpr int get_scale() noexcept { return SCALE; }

    // ---- construction / conversion -------------------------------------------------------

    ScaleInt() = default;

    constexpr explicit ScaleInt(double v) noexcept : v_(to_raw(v)) {}

    [[nodiscard]] static constexpr SelfT from_raw(Rep raw) noexcept {
        SelfT ret;
        ret.v_ = raw;
        return ret;
    }

    // Round to nearest unit, ties away from zero (llround of the scaled value), branch-free.
    // Adds the largest double below 0.5, then truncates: adding 0.5 itself would round twice
    // (0.49999999999999994 + 0.5 is 1.0, and an odd integer in [2^52, 2^53) plus 0.5 rounds up
    // to the next even one), while exact ties still reach the next integer.
    [[nodiscard]] static constexpr Rep to_raw(double v) noexcept {
        assert(math::abs(v) < MAX_ABS_DOUBLE && "not finite or out of range");  // NaN fails too
        constexpr double kJustBelowHalf = 0.49999999999999994;  // nextafter(0.5, 0)
        return static_cast<Rep>(v * SCALE_AS_DOUBLE + (v < 0.0 ? -kJustBelowHalf : kJustBelowHalf));
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

    [[nodiscard]] constexpr Rep get_raw() const noexcept { return v_; }

    constexpr void set_raw(Rep raw) noexcept { v_ = raw; }

    constexpr SelfT& operator=(double v) noexcept {
        v_ = to_raw(v);
        return *this;
    }

    // Widening is exact (overflow asserts / wraps, see the conventions); narrowing rounds half
    // away from zero. The representation stays Rep; see try_rescale() to change it.
    template <int NEW_SCALE>
    [[nodiscard]] constexpr ScaleInt<NEW_SCALE, Rep> rescale() const noexcept {
        using OutT = ScaleInt<NEW_SCALE, Rep>;
        if constexpr (NEW_SCALE == SCALE) {
            return OutT::from_raw(v_);
        } else if constexpr (NEW_SCALE > SCALE) {
            return OutT::from_raw(mul_raw(v_, ipow10<Rep>(NEW_SCALE - SCALE)));
        } else {
            return OutT::from_raw(div_round_half_away(v_, ipow10<Rep>(SCALE - NEW_SCALE)));
        }
    }

    // Checked conversion to another scale and/or representation (e.g. int128_t <-> int64_t): false,
    // with out untouched, when the value doesn't fit. Narrowing the scale rounds as requested.
    template <int NEW_SCALE, class NEW_REP>
    [[nodiscard]] constexpr bool try_rescale(ScaleInt<NEW_SCALE, NEW_REP>& out,
                                             Rounding rounding = Rounding::HalfAway) const noexcept {
        int128_t raw = v_;  // every Rep fits, and so does every widening below
        if constexpr (NEW_SCALE > SCALE) {
            if (mul_overflow(raw, ipow10<int128_t>(NEW_SCALE - SCALE), raw)) {
                return false;
            }
        } else if constexpr (NEW_SCALE < SCALE) {
            raw = detail::div_rounded(raw, ipow10<int128_t>(SCALE - NEW_SCALE), rounding);
        }
        if (raw < static_cast<int128_t>(std::numeric_limits<NEW_REP>::min()) ||
            raw > static_cast<int128_t>(std::numeric_limits<NEW_REP>::max())) {
            return false;
        }
        out = ScaleInt<NEW_SCALE, NEW_REP>::from_raw(static_cast<NEW_REP>(raw));
        return true;
    }

    // ---- tick rounding ---------------------------------------------------------------------
    // round: nearest multiple, ties away from zero (symmetric for negative values)
    // floor: toward -infinity (e.g. buy price, order quantity)
    // ceil:  toward +infinity (e.g. sell price)

    [[nodiscard]] static constexpr Rep tick_to_raw(double tick) noexcept {
        const Rep t = to_raw(tick);
        assert(t > 0 && "tick must be positive and at least one unit of this scale");
        return t;
    }

    // Tick known at compile time, in raw units: e.g. floor_to_tick<1'000'000>() is 0.01 at E8.
    template <int64_t TICK_RAW>
    constexpr SelfT& round_to_tick() noexcept {
        static_assert(TICK_RAW > 0);
        v_ = round_to_multiple(v_, static_cast<Rep>(TICK_RAW));
        return *this;
    }

    template <int64_t TICK_RAW>
    constexpr SelfT& floor_to_tick() noexcept {
        static_assert(TICK_RAW > 0);
        v_ = floor_to_multiple(v_, static_cast<Rep>(TICK_RAW));
        return *this;
    }

    template <int64_t TICK_RAW>
    constexpr SelfT& ceil_to_tick() noexcept {
        static_assert(TICK_RAW > 0);
        v_ = ceil_to_multiple(v_, static_cast<Rep>(TICK_RAW));
        return *this;
    }

    template <int64_t TICK_RAW>
    [[nodiscard]] constexpr bool is_multiple_of_tick() const noexcept {
        static_assert(TICK_RAW > 0);
        return is_multiple_of(v_, static_cast<Rep>(TICK_RAW));
    }

    // Tick converted once (e.g. per instrument).
    constexpr SelfT& round_to_tick(const TickSize<SCALE, Rep>& tick) noexcept {
        v_ = tick.visit([v = v_](auto d) { return round_to_multiple(v, static_cast<Rep>(d)); });
        return *this;
    }

    constexpr SelfT& floor_to_tick(const TickSize<SCALE, Rep>& tick) noexcept {
        v_ = tick.visit([v = v_](auto d) { return floor_to_multiple(v, static_cast<Rep>(d)); });
        return *this;
    }

    constexpr SelfT& ceil_to_tick(const TickSize<SCALE, Rep>& tick) noexcept {
        v_ = tick.visit([v = v_](auto d) { return ceil_to_multiple(v, static_cast<Rep>(d)); });
        return *this;
    }

    [[nodiscard]] constexpr bool is_multiple_of_tick(const TickSize<SCALE, Rep>& tick) const noexcept {
        return tick.visit([v = v_](auto d) { return is_multiple_of(v, static_cast<Rep>(d)); });
    }

    // Runtime integer tick (tick_raw > 0): one division (64-bit, or 128-bit for int128_t).
    constexpr SelfT& round_to_tick_raw(Rep tick_raw) noexcept {
        v_ = round_to_multiple(v_, tick_raw);
        return *this;
    }

    constexpr SelfT& floor_to_tick_raw(Rep tick_raw) noexcept {
        v_ = floor_to_multiple(v_, tick_raw);
        return *this;
    }

    constexpr SelfT& ceil_to_tick_raw(Rep tick_raw) noexcept {
        v_ = ceil_to_multiple(v_, tick_raw);
        return *this;
    }

    [[nodiscard]] constexpr bool is_multiple_of_tick_raw(Rep tick_raw) const noexcept {
        return is_multiple_of(v_, tick_raw);
    }

    // Convenience overloads converting the tick on every call. A tick that is finer than this
    // type's resolution, not positive, not finite or too large leaves the value unchanged.
    constexpr SelfT& round_to_tick(double tick) noexcept {
        const Rep tick_raw = checked_tick_raw(tick);
        return tick_raw > 0 ? round_to_tick_raw(tick_raw) : *this;
    }

    constexpr SelfT& floor_to_tick(double tick) noexcept {
        const Rep tick_raw = checked_tick_raw(tick);
        return tick_raw > 0 ? floor_to_tick_raw(tick_raw) : *this;
    }

    constexpr SelfT& ceil_to_tick(double tick) noexcept {
        const Rep tick_raw = checked_tick_raw(tick);
        return tick_raw > 0 ? ceil_to_tick_raw(tick_raw) : *this;
    }

    // ---- ScaleInt <op> ScaleInt: exact integer arithmetic ----------------------------------

    constexpr bool operator==(const SelfT&) const noexcept = default;
    constexpr auto operator<=>(const SelfT&) const noexcept = default;

    [[nodiscard]] constexpr SelfT operator+(const SelfT rhs) const noexcept { return from_raw(add_raw(v_, rhs.v_)); }
    [[nodiscard]] constexpr SelfT operator-(const SelfT rhs) const noexcept { return from_raw(sub_raw(v_, rhs.v_)); }
    [[nodiscard]] constexpr SelfT operator-() const noexcept { return from_raw(sub_raw(0, v_)); }
    [[nodiscard]] constexpr SelfT operator*(Rep k) const noexcept { return from_raw(mul_raw(v_, k)); }
    [[nodiscard]] friend constexpr SelfT operator*(Rep k, const SelfT x) noexcept { return x * k; }
    // would silently truncate the factor to an integer (px * 1.5 == px); scale explicitly instead
    template <std::floating_point F>
    SelfT operator*(F) const = delete;
    template <std::floating_point F>
    friend SelfT operator*(F, const SelfT) = delete;

    constexpr SelfT& operator+=(const SelfT rhs) noexcept {
        v_ = add_raw(v_, rhs.v_);
        return *this;
    }

    constexpr SelfT& operator-=(const SelfT rhs) noexcept {
        v_ = sub_raw(v_, rhs.v_);
        return *this;
    }

    // abs of the minimum does not fit: asserts in debug builds, stays the minimum in release.
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
        const URepT abs_v = uabs(v_);  // well defined for the minimum
        if (v_ < 0) {
            *first++ = '-';
        }
        if constexpr (std::same_as<Rep, int64_t>) {
            first = std::to_chars(first, first + 20, abs_v / SCALE_AS_UINT).ptr;
        } else {
            first = math::to_chars(first, abs_v / SCALE_AS_UINT);
        }
        URepT rem = abs_v % SCALE_AS_UINT;
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

    // ---- parsing -------------------------------------------------------------------------------

    // Exact decimal text to value, like std::from_chars: [+-]digits[.digits][(e|E)[+-]digits], with at
    // least one mantissa digit (".5" and "5." are accepted). No locale, no whitespace, no allocation.
    //   * success: {one past the number, errc{}}, out set
    //   * no number at first: {first, errc::invalid_argument}
    //   * the value doesn't fit, or needs more than SCALE fraction digits (non-zero digits beyond the
    //     scale; trailing zeros are fine): {one past the number, errc::result_out_of_range}
    // out is untouched on error.
    static constexpr std::from_chars_result from_chars(const char* first, const char* last, SelfT& out) noexcept {
        const char* p = first;
        bool negative = false;
        if (p != last && (*p == '-' || *p == '+')) {
            negative = *p == '-';
            ++p;
        }
        const char* int_begin = p;
        while (p != last && is_digit(*p)) {
            ++p;
        }
        const char* int_end = p;
        const char* frac_begin = p;
        const char* frac_end = p;
        if (p != last && *p == '.') {
            frac_begin = ++p;
            while (p != last && is_digit(*p)) {
                ++p;
            }
            frac_end = p;
        }
        if (int_begin == int_end && frac_begin == frac_end) {
            return {first, std::errc::invalid_argument};
        }

        int exponent = 0;
        if (p != last && (*p == 'e' || *p == 'E')) {
            const char* q = p + 1;
            bool exp_negative = false;
            if (q != last && (*q == '-' || *q == '+')) {
                exp_negative = *q == '-';
                ++q;
            }
            if (q != last && is_digit(*q)) {  // otherwise the 'e' is not part of the number
                int e = 0;
                for (; q != last && is_digit(*q); ++q) {
                    e = e < 10000 ? e * 10 + (*q - '0') : e;  // saturates far beyond any representable exponent
                }
                exponent = exp_negative ? -e : e;
                p = q;
            }
        }

        // significant digits: no leading zeros in the integer part, no trailing zeros in the fraction
        while (int_begin != int_end && *int_begin == '0') {
            ++int_begin;
        }
        while (frac_end != frac_begin && frac_end[-1] == '0') {
            --frac_end;
        }
        const auto frac_digits = static_cast<int>(frac_end - frac_begin);
        // raw = digits * 10^shift
        const int shift = SCALE + exponent - frac_digits;

        URepT digits = 0;
        if ((int_end - int_begin) + frac_digits <= 19) [[likely]] {
            // up to 19 digits fit in uint64 without overflow checks (10^19 - 1 < 2^64): one 64-bit
            // multiply-add per digit, widened once
            uint64_t acc = 0;
            for (const char* c = int_begin; c != int_end; ++c) {
                acc = acc * 10 + static_cast<uint64_t>(*c - '0');
            }
            for (const char* c = frac_begin; c != frac_end; ++c) {
                acc = acc * 10 + static_cast<uint64_t>(*c - '0');
            }
            digits = static_cast<URepT>(acc);
        } else if (!accumulate(int_begin, int_end, digits) || !accumulate(frac_begin, frac_end, digits)) {
            return {p, std::errc::result_out_of_range};
        }
        if (digits != 0) {
            if (shift > 0) {
                if (shift > std::numeric_limits<URepT>::digits10 || mul_overflow(digits, kPow10[shift], digits)) {
                    return {p, std::errc::result_out_of_range};
                }
            } else if (shift < 0) {
                // only integer-part zeros can absorb a negative exponent ("1200e-2" at SCALE 0)
                if (-shift > std::numeric_limits<URepT>::digits10 || digits % kPow10[-shift] != 0) {
                    return {p, std::errc::result_out_of_range};
                }
                digits /= kPow10[-shift];
            }
        }

        constexpr URepT kMaxPositive = static_cast<URepT>(std::numeric_limits<Rep>::max());
        if (digits > kMaxPositive + static_cast<URepT>(negative)) {  // the minimum is only reachable negated
            return {p, std::errc::result_out_of_range};
        }
        // modulo conversion (well defined since C++20): -2^(N-1) for the negated minimum
        out.v_ = negative ? static_cast<Rep>(URepT{0} - digits) : static_cast<Rep>(digits);
        return {p, std::errc{}};
    }

    // The whole of text as a value (see from_chars), nullopt if text is not exactly one number.
    [[nodiscard]] static constexpr std::optional<SelfT> parse(std::string_view text) noexcept {
        SelfT out;
        const char* last = text.data() + text.size();
        const auto [ptr, ec] = from_chars(text.data(), last, out);
        if (ec != std::errc{} || ptr != last) {
            return std::nullopt;
        }
        return out;
    }

private:
    static constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

    // 10^k for k in [0, digits10] of URepT: a table lookup instead of a runtime loop of multiplications
    static constexpr auto kPow10 = [] {
        std::array<URepT, std::numeric_limits<URepT>::digits10 + 1> t{};
        for (std::size_t k = 0; k < t.size(); ++k) {
            t[k] = ipow10<URepT>(static_cast<int>(k));
        }
        return t;
    }();

    // acc = acc * 10^n + digits, false on overflow
    static constexpr bool accumulate(const char* first, const char* last, URepT& acc) noexcept {
        for (; first != last; ++first) {
            if (mul_overflow(acc, URepT{10}, acc) || add_overflow(acc, static_cast<URepT>(*first - '0'), acc)) {
                return false;
            }
        }
        return true;
    }

    // 0 unless |tick| converts without overflow (NaN fails the comparison too)
    static constexpr Rep checked_tick_raw(double tick) noexcept {
        return math::abs(tick) < MAX_ABS_DOUBLE ? to_raw(tick) : 0;
    }

    // Overflow asserts in debug builds and wraps in release (never UB); a plain add/sub/imul.
    static constexpr Rep add_raw(Rep a, Rep b) noexcept {
        Rep out = 0;
        [[maybe_unused]] const bool overflow = add_overflow(a, b, out);
        assert(!overflow && "ScaleInt overflow");
        return out;
    }

    static constexpr Rep sub_raw(Rep a, Rep b) noexcept {
        Rep out = 0;
        [[maybe_unused]] const bool overflow = sub_overflow(a, b, out);
        assert(!overflow && "ScaleInt overflow");
        return out;
    }

    static constexpr Rep mul_raw(Rep a, Rep b) noexcept {
        Rep out = 0;
        [[maybe_unused]] const bool overflow = mul_overflow(a, b, out);
        assert(!overflow && "ScaleInt overflow");
        return out;
    }

    Rep v_ = 0;
};

// ---- ScaleInt x ScaleInt, ScaleInt / ScaleInt ------------------------------------------------------

// out = a * b at OUT_SCALE: the exact product (scale S1 + S2) rounded once. False, with out untouched,
// on overflow. E.g. notional = price * quantity:  mul(px, qty, notional, Rounding::HalfAway).
template <int OUT_SCALE, int S1, int S2, class Rep>
[[nodiscard]] constexpr bool mul(ScaleInt<S1, Rep> a, ScaleInt<S2, Rep> b, ScaleInt<OUT_SCALE, Rep>& out,
                                 Rounding rounding = Rounding::HalfAway) noexcept {
    static_assert(S1 + S2 - OUT_SCALE <= std::numeric_limits<Rep>::digits10, "product scale too far from OUT_SCALE");
    Rep raw{};
    if (mul_overflow(a.get_raw(), b.get_raw(), raw)) {
        return false;
    }
    if constexpr (OUT_SCALE > S1 + S2) {
        if (mul_overflow(raw, ipow10<Rep>(OUT_SCALE - S1 - S2), raw)) {
            return false;
        }
    } else if constexpr (OUT_SCALE < S1 + S2) {
        raw = detail::div_rounded(raw, ipow10<Rep>(S1 + S2 - OUT_SCALE), rounding);
    }
    out = ScaleInt<OUT_SCALE, Rep>::from_raw(raw);
    return true;
}

// out = a / b at OUT_SCALE, rounded once. False, with out untouched, when b is zero or on overflow.
// E.g. average price = notional / quantity:  div(notional, qty, avg_px, Rounding::HalfAway).
template <int OUT_SCALE, int S1, int S2, class Rep>
[[nodiscard]] constexpr bool div(ScaleInt<S1, Rep> a, ScaleInt<S2, Rep> b, ScaleInt<OUT_SCALE, Rep>& out,
                                 Rounding rounding = Rounding::HalfAway) noexcept {
    // a / b = (a.raw * 10^-S1) / (b.raw * 10^-S2) = a.raw * 10^(OUT + S2 - S1) / b.raw, in OUT units
    constexpr int k = OUT_SCALE + S2 - S1;
    static_assert(k <= std::numeric_limits<Rep>::digits10 && -k <= std::numeric_limits<Rep>::digits10,
                  "scales too far apart");
    Rep num = a.get_raw();
    Rep den = b.get_raw();
    if (den == 0) {
        return false;
    }
    if constexpr (k > 0) {
        if (mul_overflow(num, ipow10<Rep>(k), num)) {
            return false;
        }
    } else if constexpr (k < 0) {
        if (mul_overflow(den, ipow10<Rep>(-k), den)) {
            return false;
        }
    }
    if (den < 0) {  // the rounding helpers take a positive divisor
        if (sub_overflow(Rep{0}, num, num) || sub_overflow(Rep{0}, den, den)) {
            return false;
        }
    }
    out = ScaleInt<OUT_SCALE, Rep>::from_raw(detail::div_rounded(num, den, rounding));
    return true;
}

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

template <int S, class Rep>
struct std::hash<::quark::math::ScaleInt<S, Rep>> {
    std::size_t operator()(const ::quark::math::ScaleInt<S, Rep>& x) const noexcept {
        if constexpr (std::same_as<Rep, int64_t>) {
            return std::hash<int64_t>{}(x.get_raw());
        } else {
            const auto u = static_cast<::quark::math::uint128_t>(x.get_raw());
            const auto lo = static_cast<uint64_t>(u);
            const auto hi = static_cast<uint64_t>(u >> 64);
            return std::hash<uint64_t>{}(lo ^ (hi * 0x9E3779B97F4A7C15ULL));
        }
    }
};
