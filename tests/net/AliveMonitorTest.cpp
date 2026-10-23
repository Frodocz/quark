#include "quark/net/AliveMonitor.h"
#include "gtest/gtest.h"

using namespace quark::net;

class TestAliveMonitor : public ::testing::Test {};

TEST_F(TestAliveMonitor, AliveUntilConnected) {
    AliveMonitor m(/*max_recv_gap_ns=*/100, /*max_ack_delay_ns=*/50);
    EXPECT_TRUE(m.is_alive(1'000'000));  // not monitoring yet
    m.on_connect(1000);
    EXPECT_TRUE(m.is_alive(1100));
    EXPECT_FALSE(m.is_alive(1101));
    m.reset();
    EXPECT_TRUE(m.is_alive(1'000'000));
}

TEST_F(TestAliveMonitor, RecvGap) {
    AliveMonitor m(100, 0);
    m.on_connect(0);
    m.on_recv(90);
    EXPECT_TRUE(m.is_alive(190));
    EXPECT_FALSE(m.is_alive(191));
    m.on_recv(191);
    EXPECT_TRUE(m.is_alive(200));
    EXPECT_EQ(m.last_recv_ns(), 191);
}

TEST_F(TestAliveMonitor, UnackedRequest) {
    AliveMonitor m(/*max_recv_gap_ns=*/0, /*max_ack_delay_ns=*/50);  // quiet line is fine
    m.on_connect(0);
    EXPECT_TRUE(m.is_alive(1'000'000));

    m.on_send(1000);
    m.on_send(1040);  // the oldest unanswered request is what counts
    EXPECT_EQ(m.oldest_unacked_send_ns(), 1000);
    EXPECT_TRUE(m.is_alive(1050));
    EXPECT_FALSE(m.is_alive(1051));

    EXPECT_TRUE(m.has_unacked_send());
    m.on_recv(1060);  // any receive answers everything outstanding
    EXPECT_FALSE(m.has_unacked_send());
    EXPECT_TRUE(m.is_alive(1'000'000));
}

TEST_F(TestAliveMonitor, TimestampZeroIsValid) {
    AliveMonitor m(100, 50);
    m.on_connect(0);   // clocks that start at 0 (e.g. tests, replay) must still be monitored
    m.on_send(0);
    EXPECT_FALSE(m.is_alive(51));
    m.on_recv(51);
    EXPECT_FALSE(m.is_alive(152));
}

TEST_F(TestAliveMonitor, BothChecks) {
    AliveMonitor m(100, 20);
    m.on_connect(0);
    m.on_send(10);
    EXPECT_FALSE(m.is_alive(31));  // ack check trips first
    m.on_recv(31);
    EXPECT_TRUE(m.is_alive(131));
    EXPECT_FALSE(m.is_alive(132));  // then the gap check
}
