#include "quark/utils/LatencyHistogram.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace quark;

class TestLatencyHistogram : public ::testing::Test {};

TEST_F(TestLatencyHistogram, EmptyAndExactSmallValues) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.percentile(50), 0);
    EXPECT_EQ(h.min(), 0);
    for (int v = 0; v < 64; ++v) h.record(v);  // exact below 64
    EXPECT_EQ(h.count(), 64u);
    EXPECT_EQ(h.min(), 0);
    EXPECT_EQ(h.max(), 63);
    EXPECT_EQ(h.percentile(50), 31);
    EXPECT_EQ(h.percentile(100), 63);
    EXPECT_DOUBLE_EQ(h.mean(), 31.5);
}

TEST_F(TestLatencyHistogram, PercentilesWithinRelativeError) {
    // log-normal-ish latencies: mostly ~1-5 us with a long tail
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> dist(7.5, 0.8);
    std::vector<int64_t> samples;
    LatencyHistogram h;
    for (int i = 0; i < 200'000; ++i) {
        const auto v = static_cast<int64_t>(dist(rng));
        samples.push_back(v);
        h.record(v);
    }
    std::sort(samples.begin(), samples.end());
    for (const double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
        const auto exact = samples[static_cast<std::size_t>(std::ceil(p / 100 * samples.size())) - 1];
        const auto approx = h.percentile(p);
        EXPECT_GE(approx, exact) << "p" << p;
        EXPECT_LE(static_cast<double>(approx), static_cast<double>(exact) * (1 + 1.0 / 32) + 1) << "p" << p;
    }
    EXPECT_EQ(h.max(), samples.back());
    EXPECT_EQ(h.percentile(100), samples.back());
}

TEST_F(TestLatencyHistogram, ExtremesAndMerge) {
    LatencyHistogram a, b;
    a.record(-5);  // clamped to 0
    a.record(std::numeric_limits<int64_t>::max());
    b.record(1000);
    a.merge(b);
    EXPECT_EQ(a.count(), 3u);
    EXPECT_EQ(a.min(), 0);
    EXPECT_EQ(a.max(), std::numeric_limits<int64_t>::max());
    EXPECT_GE(a.percentile(50), 1000);
    EXPECT_LE(a.percentile(50), 1000 + 1000 / 32 + 1);
    a.reset();
    EXPECT_EQ(a.count(), 0u);
}
