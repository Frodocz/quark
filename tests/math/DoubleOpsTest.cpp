#include "quark/math/DoubleOps.h"
#include "gtest/gtest.h"

#include <cmath>
#include <limits>
#include <vector>

namespace math = quark::math;
using math::Tolerance;

namespace {

constexpr double kEps = std::numeric_limits<double>::epsilon();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---- everything below is usable in constant expressions -------------------
static_assert(math::equals(1.0, 1.0));
static_assert(math::equals(1.0, 1.0 + 4 * kEps));
static_assert(!math::equals(1.0, 1.0 + 8 * kEps));
static_assert(math::equals(0.1 + 0.2, 0.3));
static_assert(math::equals(0.0, -0.0));
static_assert(math::is_zero(0.0));
static_assert(!math::is_zero(1e-300));
static_assert(math::is_zero(1e-13, {.abs = 1e-12}));
static_assert(math::greater(2.0, 1.0));
static_assert(!math::greater(1.0, 1.0 + kEps));
static_assert(math::less_than(1.0, 2.0));
static_assert(math::greater_or_equal(1.0, 1.0 + kEps));
static_assert(math::less_or_equal(1.0 + kEps, 1.0));
static_assert(math::equals(1.0f, 1.0f + 4 * std::numeric_limits<float>::epsilon()));
static_assert(math::equals(kInf, kInf));
static_assert(!math::equals(kInf, 1e308));
static_assert(!math::equals(1e308, -1e308));  // a - b overflows; must not report equal

}  // namespace

class TestDoubleOps : public ::testing::Test {};

TEST_F(TestDoubleOps, RelativeToleranceScalesWithMagnitude) {
    for (double x : {1.0, 100.0, 1000.0, 65000.0, 1e5, 1e9, 1e15}) {
        const double next = std::nextafter(x, kInf);
        EXPECT_TRUE(math::equals(x, next)) << x;                  // 1 ulp apart
        EXPECT_FALSE(math::equals(x, x * (1 + 1e-9))) << x;       // clearly different
        EXPECT_FALSE(math::greater(next, x)) << x;
        EXPECT_FALSE(math::less_than(x, next)) << x;
        EXPECT_TRUE(math::greater_or_equal(x, next)) << x;
        EXPECT_TRUE(math::less_or_equal(x, next)) << x;
    }
}

TEST_F(TestDoubleOps, ClearlyDifferentValues) {
    EXPECT_TRUE(math::greater(100000.0001, 100000.0));
    EXPECT_TRUE(math::less_than(100000.0, 100000.0001));
    EXPECT_FALSE(math::equals(100000.0, 100000.0001));
    EXPECT_FALSE(math::greater(1.0, 2.0));
    EXPECT_FALSE(math::less_than(2.0, 1.0));
    EXPECT_FALSE(math::greater_or_equal(1.0, 2.0));
    EXPECT_FALSE(math::less_or_equal(2.0, 1.0));
}

TEST_F(TestDoubleOps, NegativeValues) {
    EXPECT_TRUE(math::equals(-0.1 - 0.2, -0.3));
    EXPECT_TRUE(math::greater(-1.0, -2.0));
    EXPECT_TRUE(math::less_than(-2.0, -1.0));
    EXPECT_FALSE(math::equals(-1.0, 1.0));
}

TEST_F(TestDoubleOps, NearZeroNeedsAbsoluteTolerance) {
    EXPECT_FALSE(math::equals(1e-20, 0.0));  // relative tolerance against 0 is 0
    EXPECT_TRUE(math::equals(1e-20, 0.0, {.abs = 1e-12}));
    EXPECT_TRUE(math::equals(1e-20, -1e-20, {.abs = 1e-12}));
    EXPECT_FALSE(math::equals(1e-9, 0.0, {.abs = 1e-12}));
    EXPECT_TRUE(math::is_zero(1e-13, {.abs = 1e-12}));
    EXPECT_FALSE(math::is_zero(1e-11, {.abs = 1e-12}));
    EXPECT_TRUE(math::is_zero(-0.0));
    EXPECT_FALSE(math::is_zero(1e-300));
    EXPECT_TRUE(math::is_zero(0.1 + 0.2 - 0.3, {.abs = 1e-15}));
}

TEST_F(TestDoubleOps, CustomRelativeTolerance) {
    EXPECT_TRUE(math::equals(100.0, 100.5, {.rel = 0.01}));
    EXPECT_FALSE(math::equals(100.0, 102.0, {.rel = 0.01}));
    EXPECT_TRUE(math::less_or_equal(100.5, 100.0, {.rel = 0.01}));
    EXPECT_FALSE(math::less_than(100.0, 100.5, {.rel = 0.01}));
}

TEST_F(TestDoubleOps, SignedZero) {
    EXPECT_TRUE(math::equals(0.0, -0.0));
    EXPECT_FALSE(math::greater(0.0, -0.0));
    EXPECT_FALSE(math::less_than(-0.0, 0.0));
}

TEST_F(TestDoubleOps, NaNIsAlwaysFalse) {
    for (double other : {0.0, 1.0, -1.0, kInf, -kInf, kNaN}) {
        EXPECT_FALSE(math::equals(kNaN, other));
        EXPECT_FALSE(math::equals(other, kNaN));
        EXPECT_FALSE(math::greater(kNaN, other));
        EXPECT_FALSE(math::greater(other, kNaN));
        EXPECT_FALSE(math::less_than(kNaN, other));
        EXPECT_FALSE(math::less_than(other, kNaN));
        EXPECT_FALSE(math::greater_or_equal(kNaN, other));  // the old helper returned true here
        EXPECT_FALSE(math::greater_or_equal(other, kNaN));
        EXPECT_FALSE(math::less_or_equal(kNaN, other));
        EXPECT_FALSE(math::less_or_equal(other, kNaN));
    }
    EXPECT_FALSE(math::is_zero(kNaN, {.abs = 1.0}));
}

TEST_F(TestDoubleOps, Infinities) {
    EXPECT_TRUE(math::equals(kInf, kInf));
    EXPECT_TRUE(math::equals(-kInf, -kInf));
    EXPECT_FALSE(math::equals(kInf, -kInf));
    EXPECT_FALSE(math::equals(kInf, 1e308));
    EXPECT_FALSE(math::equals(kInf, 1.0, {.rel = 1.0, .abs = 1e300}));
    EXPECT_TRUE(math::greater(kInf, 1e308));
    EXPECT_TRUE(math::less_than(-kInf, -1e308));
    EXPECT_TRUE(math::greater_or_equal(kInf, kInf));
    EXPECT_TRUE(math::less_or_equal(-kInf, kInf));
    EXPECT_FALSE(math::greater(kInf, kInf));
    EXPECT_FALSE(math::is_zero(kInf, {.abs = 1e300}));
}

TEST_F(TestDoubleOps, FloatType) {
    constexpr float e = std::numeric_limits<float>::epsilon();
    EXPECT_TRUE(math::equals(1.0f, 1.0f + 2 * e));
    EXPECT_FALSE(math::equals(1.0f, 1.0f + 16 * e));
    EXPECT_TRUE(math::greater(1.0f, 0.5f));
    EXPECT_TRUE(math::is_zero(1e-9f, {.abs = 1e-6f}));
}

TEST_F(TestDoubleOps, LongDoubleType) {
    EXPECT_TRUE(math::equals(0.1L + 0.2L, 0.3L));
    EXPECT_TRUE(math::less_than(1.0L, 2.0L));
}

// Trichotomy: for non-NaN inputs exactly one of greater / less_than / equals holds,
// and the *_or_equal variants are consistent with them.
TEST_F(TestDoubleOps, TrichotomyAndConsistency) {
    std::vector<double> v = {0.0, -0.0, 1e-300, 1.0, 1.0 + kEps, 1.0 + 8 * kEps, 2.0, -1.0, 100000.0,
                             std::nextafter(100000.0, kInf), 1e15, 1e300, kInf, -kInf};
    for (double a : v) {
        for (double b : v) {
            const int n = math::greater(a, b) + math::less_than(a, b) + math::equals(a, b);
            EXPECT_EQ(1, n) << a << " vs " << b;
            EXPECT_EQ(math::greater(a, b) || math::equals(a, b), math::greater_or_equal(a, b)) << a << " vs " << b;
            EXPECT_EQ(math::less_than(a, b) || math::equals(a, b), math::less_or_equal(a, b)) << a << " vs " << b;
            EXPECT_EQ(math::equals(a, b), math::equals(b, a)) << a << " vs " << b;  // symmetric
            EXPECT_EQ(math::greater(a, b), math::less_than(b, a)) << a << " vs " << b;
        }
        EXPECT_TRUE(math::equals(a, a));  // reflexive
    }
}

TEST_F(TestDoubleOps, DocumentedNonTransitivity) {
    // Tolerance equality is not an equivalence relation; do not use it as a map key ordering.
    const double a = 1.0;
    const double c = 1.0 + 6 * kEps;
    const double b = 1.0 + 3 * kEps;
    EXPECT_TRUE(math::equals(a, b));
    EXPECT_TRUE(math::equals(b, c));
    EXPECT_FALSE(math::equals(a, c));
}

TEST_F(TestDoubleOps, Subnormals) {
    constexpr double kDenormMin = std::numeric_limits<double>::denorm_min();
    constexpr double kMinNormal = std::numeric_limits<double>::min();
    EXPECT_FALSE(math::is_zero(kDenormMin));
    EXPECT_TRUE(math::is_zero(kDenormMin, {.abs = kMinNormal}));
    EXPECT_FALSE(math::equals(kDenormMin, 0.0));
    EXPECT_FALSE(math::equals(kDenormMin, 2 * kDenormMin));  // relative tolerance is below 1 ulp here
    EXPECT_TRUE(math::equals(kDenormMin, 2 * kDenormMin, {.abs = kMinNormal}));
    EXPECT_TRUE(math::equals(kDenormMin, -kDenormMin, {.abs = kMinNormal}));
    EXPECT_TRUE(math::greater(kDenormMin, 0.0));
    EXPECT_TRUE(math::less_than(-kDenormMin, 0.0));
    EXPECT_TRUE(math::equals(kMinNormal, std::nextafter(kMinNormal, 0.0), {.abs = kDenormMin}));
}
