#include "quark/math/MathOps.h"
#include "gtest/gtest.h"

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace math = quark::math;

namespace {

constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---- constant evaluation ------------------------------------------------------------------
static_assert(math::max(1, 2) == 2 && math::min(1, 2) == 1);
static_assert(math::max(-1.5, 2.5) > 2.0 && math::min(-1.5, 2.5) < -1.0);
static_assert(math::abs(-1.5) > 1.0 && math::abs(1.5) > 1.0);
static_assert(math::is_finite(1.0) && !math::is_finite(kInf) && !math::is_finite(-kInf));  // GCC: NaN compares are not constant
static_assert(math::TWO_POW_63 > 9.2e18 && static_cast<double>(kMax) >= math::TWO_POW_63);  // rounds up to 2^63
static_assert(math::abs(int64_t{-5}) == 5 && math::abs(kMax) == kMax && math::abs(kMin + 1) == kMax);
static_assert(math::uabs(kMin) == uint64_t{1} << 63);
static_assert(math::uabs(kMax) == static_cast<uint64_t>(kMax));
static_assert(math::uabs(int64_t{-1}) == 1u && math::uabs(int64_t{0}) == 0u);
static_assert([] {
    int64_t out = 0;
    return math::add_overflow(kMax, int64_t{1}, out) && out == kMin;  // wrapped result is stored
}());
static_assert([] {
    int64_t out = 0;
    return math::sub_overflow(int64_t{0}, kMin, out) && out == kMin;  // -INT64_MIN
}());
static_assert([] {
    int64_t out = 0;
    return !math::mul_overflow(int64_t{-3}, int64_t{7}, out) && out == -21;
}());
static_assert(math::ipow10<int64_t>(0) == 1 && math::ipow10<int64_t>(18) == 1'000'000'000'000'000'000);
static_assert(math::exact_log10(int64_t{1}) == 0 && math::exact_log10(int64_t{1'000'000}) == 6);
static_assert(math::exact_log10(int64_t{1'000'000'000'000'000'000}) == 18);
static_assert(math::exact_log10(int64_t{0}) == -1 && math::exact_log10(int64_t{-10}) == -1);
static_assert(math::exact_log10(int64_t{20}) == -1 && math::exact_log10(int64_t{5}) == -1);
static_assert(math::is_pow2(int64_t{1}) && math::is_pow2(int64_t{1} << 62) && !math::is_pow2(int64_t{6}));
static_assert(!math::is_pow2(int64_t{0}) && !math::is_pow2(kMin));
static_assert(math::floor_div(int64_t{-7}, int64_t{2}) == -4 && math::ceil_div(int64_t{-7}, int64_t{2}) == -3);
static_assert(math::div_round_half_away(int64_t{-5}, int64_t{2}) == -3 && math::div_round_half_away(int64_t{5}, int64_t{2}) == 3);
static_assert(math::floor_to_multiple(kMin, int64_t{10}) == kMin + 8);  // -...810 does not fit
static_assert(math::ceil_to_multiple(kMax, int64_t{10}) == kMax - 7);   // ...810 does not fit

// ---- exact references in 128 bit ---------------------------------------------------------------
__extension__ typedef __int128 i128;  // __extension__: no -Wpedantic error

i128 ref_floor_div(i128 v, i128 d) {
    const i128 mod = ((v % d) + d) % d;  // in [0, d)
    return (v - mod) / d;
}

i128 ref_ceil_div(i128 v, i128 d) { return -ref_floor_div(-v, d); }

i128 ref_round_half_away(i128 v, i128 d) {
    const i128 lo = ref_floor_div(v, d);
    const i128 dist_lo = v - lo * d;  // in [0, d)
    const i128 dist_hi = d - dist_lo;
    if (dist_lo < dist_hi) return lo;
    if (dist_hi < dist_lo) return lo + 1;
    return v < 0 ? lo : lo + 1;  // tie: away from zero
}

// the multiple, stepped one d toward zero when it does not fit in int64_t
int64_t ref_fit(i128 m, i128 d) {
    if (m > kMax) m -= d;
    if (m < kMin) m += d;
    return static_cast<int64_t>(m);
}

std::vector<int64_t> sample_values() {
    std::vector<int64_t> v = {kMin, kMin + 1, kMin + 2, kMin + 9, -1'000'000'007, -10, -9, -5, -1, 0, 1, 5, 9, 10,
                              1'000'000'007, kMax - 9, kMax - 2, kMax - 1, kMax};
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int64_t> any(kMin, kMax);
    std::uniform_int_distribution<int64_t> small(-100'000, 100'000);
    for (int i = 0; i < 20'000; ++i) {
        v.push_back(any(rng));
        v.push_back(small(rng));
        v.push_back(kMin + small(rng) % 1000 + 1000);
        v.push_back(kMax - small(rng) % 1000 - 1000);
    }
    return v;
}

std::vector<int64_t> sample_divisors() {
    std::vector<int64_t> d = {1, 2, 3, 5, 7, 10, 25, 100, 1000, 999'999'937, 1'000'000, kMax / 2, kMax - 1, kMax};
    for (int k = 0; k <= 18; ++k) d.push_back(math::ipow10<int64_t>(k));
    for (int k = 0; k <= 62; ++k) d.push_back(int64_t{1} << k);
    return d;
}

}  // namespace

TEST(TestMathOps, DivisionMatchesExactReference) {
    const auto values = sample_values();
    for (int64_t d : sample_divisors()) {
        for (int64_t v : values) {
            ASSERT_EQ(math::floor_div(v, d), static_cast<int64_t>(ref_floor_div(v, d))) << v << " / " << d;
            ASSERT_EQ(math::ceil_div(v, d), static_cast<int64_t>(ref_ceil_div(v, d))) << v << " / " << d;
            ASSERT_EQ(math::div_round_half_away(v, d), static_cast<int64_t>(ref_round_half_away(v, d))) << v << " / " << d;
            ASSERT_EQ(math::is_multiple_of(v, d), v % d == 0) << v << " / " << d;
        }
    }
}

TEST(TestMathOps, ToMultipleMatchesExactReferenceIncludingLimits) {
    const auto values = sample_values();
    for (int64_t d : sample_divisors()) {
        for (int64_t v : values) {
            ASSERT_EQ(math::floor_to_multiple(v, d), ref_fit(ref_floor_div(v, d) * d, d)) << v << " / " << d;
            ASSERT_EQ(math::ceil_to_multiple(v, d), ref_fit(ref_ceil_div(v, d) * d, d)) << v << " / " << d;
            ASSERT_EQ(math::round_to_multiple(v, d), ref_fit(ref_round_half_away(v, d) * d, d)) << v << " / " << d;
        }
    }
}

TEST(TestMathOps, ToMultipleNearLimits) {
    // representable results are exact
    EXPECT_EQ(math::floor_to_multiple(kMin, int64_t{1}), kMin);
    EXPECT_EQ(math::ceil_to_multiple(kMax, int64_t{1}), kMax);
    EXPECT_EQ(math::floor_to_multiple(kMax, int64_t{10}), kMax - 7);
    EXPECT_EQ(math::ceil_to_multiple(kMin, int64_t{10}), kMin + 8);
    EXPECT_EQ(math::round_to_multiple(kMax, int64_t{100}), kMax - 7);  // ...807 -> ...800
    // the exact result does not fit: nearest representable multiple toward zero
    EXPECT_EQ(math::round_to_multiple(kMax, int64_t{10}), kMax - 7);  // ...810 -> ...800
    EXPECT_EQ(math::round_to_multiple(kMin, int64_t{10}), kMin + 8);  // -...810 -> -...800
    EXPECT_EQ(math::floor_to_multiple(kMin, kMax), -kMax);
    EXPECT_EQ(math::ceil_to_multiple(kMax - 1, kMax), kMax);
    EXPECT_EQ(math::round_to_multiple(kMin, kMax), -kMax);
}

TEST(TestMathOps, SignedAbsAndNegationOfMin) {
    EXPECT_EQ(math::uabs(kMin), uint64_t{9'223'372'036'854'775'808u});
    EXPECT_EQ(math::abs(kMin + 1), kMax);
    EXPECT_EQ(math::abs(int32_t{-7}), 7);
    EXPECT_EQ(math::uabs(std::numeric_limits<int32_t>::min()), uint32_t{2'147'483'648u});
    // abs(INT64_MIN) is not representable: asserts in debug, wraps to INT64_MIN in release
    EXPECT_DEBUG_DEATH((void)math::abs(kMin), "abs\\(min\\) overflows");
#ifdef NDEBUG
    EXPECT_EQ(math::abs(kMin), kMin);
#endif
}

TEST(TestMathOps, OverflowHelpersWrap) {
    int64_t out = 0;
    EXPECT_FALSE(math::add_overflow(int64_t{1}, int64_t{2}, out));
    EXPECT_EQ(out, 3);
    EXPECT_TRUE(math::add_overflow(kMax, int64_t{1}, out));
    EXPECT_EQ(out, kMin);
    EXPECT_TRUE(math::sub_overflow(kMin, int64_t{1}, out));
    EXPECT_EQ(out, kMax);
    EXPECT_TRUE(math::sub_overflow(int64_t{0}, kMin, out));
    EXPECT_EQ(out, kMin);
    EXPECT_TRUE(math::mul_overflow(kMin, int64_t{-1}, out));
    EXPECT_EQ(out, kMin);
    EXPECT_TRUE(math::mul_overflow(int64_t{1} << 62, int64_t{2}, out));
    EXPECT_EQ(out, kMin);
}

TEST(TestMathOps, FloatingHelpers) {
    EXPECT_FALSE(math::is_finite(kNaN));
    EXPECT_TRUE(math::is_finite(std::numeric_limits<double>::max()));
    EXPECT_TRUE(math::is_finite(std::numeric_limits<double>::denorm_min()));
    EXPECT_TRUE(math::is_finite(std::numeric_limits<float>::lowest()));
    EXPECT_FALSE(math::is_finite(std::numeric_limits<float>::infinity()));
    EXPECT_DOUBLE_EQ(math::abs(-0.25), 0.25);
    EXPECT_FLOAT_EQ(math::abs(-0.25f), 0.25f);
    EXPECT_DOUBLE_EQ(math::max(1.0, kNaN), 1.0);  // a < b ? b : a: NaN second is ignored
    EXPECT_DOUBLE_EQ(math::min(1.0, kNaN), 1.0);
}
