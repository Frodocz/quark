#include "quark/utils/LatencyHistogram.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#include <pthread.h>

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
        // nearest rank, as the histogram defines it: ceil(n * p / 100), ignoring rounding noise
        const auto rank =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(samples.size()) * p / 100 - 1e-6)));
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

TYPED_TEST(TestLatencyHistogram, NearestRankNeverReportsBelowThePercentile) {
    auto h = std::make_unique<TypeParam>();
    h->record(10);
    h->record(1000);
    // 74% of 2 samples is 1.48 samples: the second one (a rounded rank would report 10, below
    // which only 50% fall)
    EXPECT_GE(h->percentile(74), 1000);
    EXPECT_EQ(h->percentile(50), 10);
    EXPECT_EQ(h->percentile(1), 10);
    EXPECT_GE(h->percentile(100), 1000);
    EXPECT_EQ(h->percentile(std::numeric_limits<double>::quiet_NaN()), 10);  // as p == 0: the min
    EXPECT_EQ(h->percentile(-1), 10);
    EXPECT_GE(h->percentile(1e9), 1000);
}

TYPED_TEST(TestLatencyHistogram, SingleSampleAndMergeIntoEmpty) {
    auto h = std::make_unique<TypeParam>();
    h->record(12345);
    for (double p : {0.0, 1.0, 50.0, 99.99, 100.0}) {
        EXPECT_GE(h->percentile(p), 12345) << p;
        EXPECT_LE(h->percentile(p), 12345) << p;  // clamped to the exact max
    }
    auto empty = std::make_unique<TypeParam>();
    empty->merge(*h);
    EXPECT_EQ(empty->min(), 12345);
    EXPECT_EQ(empty->max(), 12345);
    auto other = std::make_unique<TypeParam>();
    h->merge(*other);  // merging an empty one changes nothing
    EXPECT_EQ(h->min(), 12345);
    EXPECT_EQ(h->count(), 1u);
}

TYPED_TEST(TestLatencyHistogram, ResetNeedsNoStackSpace) {
    // reset() of the 440 KiB <10> histogram must not build a temporary: run it on a 64 KiB stack
    auto h = std::make_unique<TypeParam>();
    h->record(42);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_t t;
    ASSERT_EQ(pthread_create(&t, &attr, [](void* p) -> void* {
        static_cast<TypeParam*>(p)->reset();
        return nullptr;
    }, h.get()), 0);
    pthread_join(t, nullptr);
    pthread_attr_destroy(&attr);
    EXPECT_EQ(h->count(), 0u);
    EXPECT_EQ(h->percentile(50), 0);
    h->record(7);
    EXPECT_EQ(h->min(), 7);
}
