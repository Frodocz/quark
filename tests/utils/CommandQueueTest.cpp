#include "quark/utils/CommandQueue.h"
#include "gtest/gtest.h"

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace quark;

class TestCommandQueue : public ::testing::Test {};

TEST_F(TestCommandQueue, RunsInOrderAndReportsFull) {
    CommandQueue<4> q;
    std::vector<int> ran;
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.post([&ran, i] { ran.push_back(i); }));
    EXPECT_FALSE(q.post([] {}));  // full
    EXPECT_EQ(q.drain(2), 2u);
    EXPECT_EQ(ran, (std::vector<int>{0, 1}));
    EXPECT_TRUE(q.post([&ran] { ran.push_back(4); }));
    EXPECT_EQ(q.drain(), 3u);
    EXPECT_EQ(ran, (std::vector<int>{0, 1, 2, 3, 4}));
    EXPECT_EQ(q.drain(), 0u);
}

TEST_F(TestCommandQueue, ReleasesCapturedStateAfterRunning) {
    CommandQueue<4> q;
    auto token = std::make_shared<int>(1);
    ASSERT_TRUE(q.post([token] {}));
    EXPECT_EQ(token.use_count(), 2);
    q.drain();
    EXPECT_EQ(token.use_count(), 1);
}

TEST_F(TestCommandQueue, CrossThreadHandoff) {
    constexpr int kCommands = 200'000;
    CommandQueue<1024> q;
    std::atomic<bool> done{false};
    int64_t sum = 0;  // only touched by the consumer
    int next_expected = 0;
    bool in_order = true;
    std::thread producer([&] {
        for (int i = 0; i < kCommands; ++i) {
            std::array<int64_t, 4> payload{i, 0, 0, 0};  // a 32-byte capture: stored inline
            while (!q.post([&sum, &next_expected, &in_order, payload] {
                in_order = in_order && payload[0] == next_expected++;
                sum += payload[0];
            })) {
            }
        }
        done = true;
    });
    while (!done || q.drain() > 0) q.drain();
    producer.join();
    q.drain();
    EXPECT_TRUE(in_order);
    EXPECT_EQ(next_expected, kCommands);
    EXPECT_EQ(sum, static_cast<int64_t>(kCommands) * (kCommands - 1) / 2);
}
