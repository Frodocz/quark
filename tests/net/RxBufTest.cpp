#include "quark/net/RxBuf.h"
#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
#include <string_view>

using namespace quark::net;

class TestRxBuf : public ::testing::Test {};

TEST_F(TestRxBuf, RxBuf_Basics) {
    RxBuf rx_buf(128);
    EXPECT_EQ(rx_buf.capacity(), 128);
    EXPECT_EQ(rx_buf.read_size(), 0);
    EXPECT_EQ(rx_buf.write_size(), 128);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(rx_buf.read_ptr()) % RxBuf::kAlignment, 0u);

    std::string_view msg = "hello world";
    const int msg_size = static_cast<int>(msg.size());
    std::memcpy(rx_buf.write_ptr(), msg.data(), msg_size);
    rx_buf.append(msg_size);
    EXPECT_EQ(rx_buf.read_size(), msg_size);
    EXPECT_EQ(rx_buf.write_size(), 128 - msg_size);
    EXPECT_EQ(std::string_view(rx_buf.read_ptr(), rx_buf.read_size()), msg);
}

TEST_F(TestRxBuf, RxBuf_CapacityRoundsUpToAlignment) {
    RxBuf rx_buf(100);
    EXPECT_EQ(rx_buf.capacity(), 128);
}

TEST_F(TestRxBuf, RxBuf_RewindsWhenFullyConsumed) {
    RxBuf rx_buf(128);
    rx_buf.append(40);
    rx_buf.consume(40);
    EXPECT_EQ(rx_buf.read_size(), 0);
    EXPECT_EQ(rx_buf.write_size(), 128);
}

TEST_F(TestRxBuf, RxBuf_CompactsPastHalf) {
    RxBuf rx_buf(128);
    for (int i = 0; i < 80; i++) rx_buf.write_ptr()[i] = static_cast<char>(i);
    rx_buf.append(80);

    rx_buf.consume(64);  // read_pos reaches half: remaining 16 bytes move to the front
    EXPECT_EQ(rx_buf.read_size(), 16);
    EXPECT_EQ(rx_buf.write_size(), 128 - 16);
    for (int i = 0; i < 16; i++) EXPECT_EQ(rx_buf.read_ptr()[i], static_cast<char>(64 + i));
}

TEST_F(TestRxBuf, RxBuf_CompactsWhenTailSpaceLow) {
    RxBuf rx_buf(128);
    for (int i = 0; i < 120; i++) rx_buf.write_ptr()[i] = static_cast<char>(i);
    rx_buf.append(120);  // only 8 bytes left at the tail, below the 1/8 low watermark

    rx_buf.consume(10);  // below half, but tail space is low: compact anyway
    EXPECT_EQ(rx_buf.read_size(), 110);
    EXPECT_EQ(rx_buf.write_size(), 128 - 110);
    for (int i = 0; i < 110; i++) EXPECT_EQ(rx_buf.read_ptr()[i], static_cast<char>(10 + i));
}

TEST_F(TestRxBuf, RxBuf_MoveConstruct) {
    RxBuf a(64);
    a.append(10);
    RxBuf b(std::move(a));
    EXPECT_EQ(b.capacity(), 64);
    EXPECT_EQ(b.read_size(), 10);
}
