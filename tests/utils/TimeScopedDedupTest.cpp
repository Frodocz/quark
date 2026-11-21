#include "quark/utils/TimeScopedDedup.h"

#include <gtest/gtest.h>

using quark::TimeScopedDedup;

TEST(TestTimeScopedDedup, FirstThenDuplicatesWithinTheWindow) {
    TimeScopedDedup d(1000, 64);
    EXPECT_TRUE(d.first(42, 10).first);
    const auto dup = d.first(42, 500);
    EXPECT_FALSE(dup.first);
    EXPECT_EQ(dup.first_ns, 10);  // for the late copy's delay
    EXPECT_TRUE(d.first(42, 1010).first);  // the window passed
    EXPECT_TRUE(d.first(0, 1).first);
    EXPECT_FALSE(d.first(0, 2).first);    // key 0 works like any other
}

TEST(TestTimeScopedDedup, ManyKeysInATinyTableNeverLoseRecentOnes) {
    TimeScopedDedup d(100, 16);
    for (uint64_t k = 1; k <= 10'000; ++k) {
        const int64_t t = static_cast<int64_t>(k);
        EXPECT_TRUE(d.first(k, t).first) << k;
        EXPECT_FALSE(d.first(k, t).first) << k;  // just recorded: always found
    }
}

TEST(TestTimeScopedDedup, ExpiredSlotsAreReused) {
    TimeScopedDedup d(10, 16);
    for (uint64_t round = 0; round < 100; ++round) {
        for (uint64_t k = 1; k <= 8; ++k) {
            const int64_t t = static_cast<int64_t>(round * 100);
            EXPECT_TRUE(d.first(k + round * 1000, t).first);
            EXPECT_FALSE(d.first(k + round * 1000, t + 1).first);
        }
    }
}
