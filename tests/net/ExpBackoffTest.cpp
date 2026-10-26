#include "quark/net/ExpBackoff.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <limits>
#include <set>

using namespace quark::net;

class TestExpBackoff : public ::testing::Test {};

TEST_F(TestExpBackoff, DoublesUpToMaxWithoutJitter) {
    ExpBackoff b(1'000, 10'000, /*jitter=*/0);
    EXPECT_EQ(b.next_delay_ns(), 1'000);
    EXPECT_EQ(b.next_delay_ns(), 2'000);
    EXPECT_EQ(b.next_delay_ns(), 4'000);
    EXPECT_EQ(b.next_delay_ns(), 8'000);
    EXPECT_EQ(b.next_delay_ns(), 10'000);
    EXPECT_EQ(b.next_delay_ns(), 10'000);
    EXPECT_EQ(b.attempts(), 6u);

    b.reset();
    EXPECT_EQ(b.attempts(), 0u);
    EXPECT_EQ(b.next_delay_ns(), 1'000);
}

TEST_F(TestExpBackoff, NoOverflowNearInt64Max) {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    ExpBackoff b(kMax / 2 + 1, kMax, 0);
    EXPECT_EQ(b.next_delay_ns(), kMax / 2 + 1);
    EXPECT_EQ(b.next_delay_ns(), kMax);
    EXPECT_EQ(b.next_delay_ns(), kMax);
}

TEST_F(TestExpBackoff, JitterStaysInBandAndSpreadsClients) {
    // 100 clients that dropped at the same moment, each with its own seed
    std::set<int64_t> first_delays;
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        ExpBackoff b(1'000'000, 64'000'000, 0.2, seed);
        int64_t base = 1'000'000;
        for (int i = 0; i < 10; ++i) {
            const int64_t d = b.next_delay_ns();
            EXPECT_GE(d, base * 8 / 10);
            EXPECT_LE(d, base * 12 / 10);
            if (i == 0) first_delays.insert(d);
            base = std::min<int64_t>(base * 2, 64'000'000);
        }
    }
    EXPECT_GT(first_delays.size(), 90u);  // not in lockstep
}

TEST_F(TestExpBackoff, SameSeedIsDeterministic) {
    ExpBackoff a(1'000, 100'000, 0.5, 42);
    ExpBackoff b(1'000, 100'000, 0.5, 42);
    for (int i = 0; i < 10; ++i) EXPECT_EQ(a.next_delay_ns(), b.next_delay_ns());
}

TEST_F(TestExpBackoff, JitterNearInt64MaxStaysPositiveAndCapped) {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    for (uint64_t seed = 1; seed <= 50; ++seed) {
        ExpBackoff b(kMax / 2 + 1, kMax, 0.2, seed);
        for (int i = 0; i < 10; ++i) {
            const int64_t d = b.next_delay_ns();
            EXPECT_GT(d, 0);
            EXPECT_LE(d, kMax);
        }
    }
}

TEST_F(TestExpBackoff, JitterNeverExceedsTheCap) {
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        ExpBackoff b(100, 1'000, 0.2, seed);
        for (int i = 0; i < 20; ++i) {
            const int64_t d = b.next_delay_ns();
            EXPECT_LE(d, 1'000);
            EXPECT_GE(d, 100 * 8 / 10);
        }
    }
    // at the cap the delays still spread (below it) instead of all being max_ns
    std::set<int64_t> at_cap;
    for (uint64_t seed = 1; seed <= 100; ++seed) {
        ExpBackoff b(1'000'000, 1'000'000, 0.2, seed);
        at_cap.insert(b.next_delay_ns());
    }
    EXPECT_GT(at_cap.size(), 90u);
    EXPECT_GE(*at_cap.begin(), 800'000);
}
