#include "quark/utils/RateLimiter.h"

#include <gtest/gtest.h>

using quark::SlidingWindowLimiter;

namespace {
constexpr int64_t kSec = 1'000'000'000;
}

TEST(TestRateLimiter, WindowSlides) {
    SlidingWindowLimiter l(10 * kSec, 3);
    EXPECT_TRUE(l.try_acquire(0));
    EXPECT_TRUE(l.try_acquire(1 * kSec));
    EXPECT_TRUE(l.try_acquire(2 * kSec));
    EXPECT_FALSE(l.try_acquire(9 * kSec));    // 3 in (-1, 9]
    EXPECT_TRUE(l.try_acquire(10 * kSec));    // the one at 0 left the window
    EXPECT_FALSE(l.try_acquire(10 * kSec));
    EXPECT_EQ(l.used(10 * kSec), 3u);
    EXPECT_EQ(l.used(100 * kSec), 0u);
}

TEST(TestRateLimiter, WeightsAndRefusalsTakeNothing) {
    SlidingWindowLimiter l(kSec, 10);
    EXPECT_TRUE(l.try_acquire(0, 7));
    EXPECT_FALSE(l.try_acquire(0, 4));
    EXPECT_EQ(l.used(0), 7u);
    EXPECT_TRUE(l.try_acquire(0, 3));
}

TEST(TestRateLimiter, ResyncToTheExchangeNeverLowers) {
    SlidingWindowLimiter l(10 * kSec, 300);
    l.try_acquire(0, 5);
    l.resync(1, 290);  // other clients of the account used more
    EXPECT_EQ(l.used(1), 290u);
    EXPECT_TRUE(l.try_acquire(2, 10));
    EXPECT_FALSE(l.try_acquire(2));
    l.resync(3, 100);  // behind ours: ignored
    EXPECT_EQ(l.used(3), 300u);
}
