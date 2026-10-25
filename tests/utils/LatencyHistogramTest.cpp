#include "quark/utils/LatencyHistogram.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <vector>

using namespace quark;

static_assert(std::is_same_v<LatencyHistogram, BasicLatencyHistogram<5>>);

template <class H>
class TestLatencyHistogram : public ::testing::Test {};

using Precisions = ::testing::Types<BasicLatencyHistogram<5>, BasicLatencyHistogram<7>, BasicLatencyHistogram<10>>;
TYPED_TEST_SUITE(TestLatencyHistogram, Precisions);

TYPED_TEST(TestLatencyHistogram, EmptyAndExactSmallValues) {
    TypeParam h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.percentile(50), 0);
    EXPECT_EQ(h.min(), 0);
    constexpr int kExactBelow = 2 * TypeParam::kSubBuckets;
    for (int v = 0; v < kExactBelow; ++v) h.record(v);
    EXPECT_EQ(h.count(), static_cast<uint64_t>(kExactBelow));
    EXPECT_EQ(h.max(), kExactBelow - 1);
    EXPECT_EQ(h.percentile(50), kExactBelow / 2 - 1);
    EXPECT_EQ(h.percentile(100), kExactBelow - 1);
    EXPECT_DOUBLE_EQ(h.mean(), (kExactBelow - 1) / 2.0);
}

TYPED_TEST(TestLatencyHistogram, PercentilesWithinTheDocumentedRelativeError) {
    // log-normal latencies: mostly ~1-5 us with a long tail
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> dist(7.5, 0.8);
    std::vector<int64_t> samples;
    auto h = std::make_unique<TypeParam>();
    for (int i = 0; i < 200'000; ++i) {
        const auto v = static_cast<int64_t>(dist(rng));
        samples.push_back(v);
        h->record(v);
    }
    std::sort(samples.begin(), samples.end());
    const double bound = 1.0 / TypeParam::kSubBuckets;  // the bucket width, relative
    for (const double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
        // the sample at the same rank the histogram uses (rounded, as HdrHistogram does): with
        // ceil(), p99.9 of 200000 lands one sample further because 99.9/100*200000 isn't exact
        const auto rank = std::max<std::size_t>(1, static_cast<std::size_t>(samples.size() * p / 100 + 0.5));
        const auto exact = samples[rank - 1];
        const auto approx = h->percentile(p);
        EXPECT_GE(approx, exact) << "p" << p;  // never below the true value
        EXPECT_LE(static_cast<double>(approx), static_cast<double>(exact) * (1 + bound) + 1) << "p" << p;
    }
    EXPECT_EQ(h->max(), samples.back());
    EXPECT_EQ(h->percentile(100), samples.back());
}

TYPED_TEST(TestLatencyHistogram, ExtremesMergeAndReset) {
    auto a = std::make_unique<TypeParam>();
    auto b = std::make_unique<TypeParam>();
    a->record(-5);  // clamped to 0
    a->record(std::numeric_limits<int64_t>::max());
    b->record(100'000);
    a->merge(*b);
    EXPECT_EQ(a->count(), 3u);
    EXPECT_EQ(a->min(), 0);
    EXPECT_EQ(a->max(), std::numeric_limits<int64_t>::max());
    EXPECT_GE(a->percentile(50), 100'000);
    EXPECT_LE(a->percentile(50), 100'000 + 100'000 / TypeParam::kSubBuckets + 1);
    a->reset();
    EXPECT_EQ(a->count(), 0u);
}
