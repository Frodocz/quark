#include "quark/net/TxBuf.h"
#include "gtest/gtest.h"

#include <cstring>
#include <string>
#include <string_view>

using namespace quark::net;

namespace {

std::string gather(const struct iovec (&iov)[2], int n) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        out.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
    }
    return out;
}

} // namespace

class TestTxBuf : public ::testing::Test {};

TEST_F(TestTxBuf, TxBuf_Basics) {
    TxBuf tx_buf(128);
    EXPECT_EQ(tx_buf.capacity(), 128);
    EXPECT_EQ(tx_buf.size(), 0);
    EXPECT_EQ(tx_buf.free_size(), 128);

    std::string_view msg = "the same individual phenomena are reproduced in the mass";
    const int msg_size = static_cast<int>(msg.size());
    EXPECT_TRUE(tx_buf.append(msg.data(), msg_size));
    EXPECT_EQ(tx_buf.size(), msg_size);
    EXPECT_EQ(tx_buf.free_size(), 128 - msg_size);

    struct iovec iov[2];
    EXPECT_EQ(tx_buf.get_out_data(iov), 1);
    EXPECT_EQ(static_cast<int>(iov[0].iov_len), msg_size);
    EXPECT_EQ(std::string_view(static_cast<const char*>(iov[0].iov_base), iov[0].iov_len), msg);
    tx_buf.consume(msg_size);
    EXPECT_EQ(tx_buf.size(), 0);
    EXPECT_EQ(tx_buf.free_size(), 128);

    // consuming everything rewinds, so these two appends stay contiguous
    EXPECT_TRUE(tx_buf.append(msg.data(), msg_size));
    EXPECT_TRUE(tx_buf.append(msg.data(), msg_size));
    EXPECT_FALSE(tx_buf.append(msg.data(), msg_size));
    EXPECT_EQ(tx_buf.size(), 2 * msg_size);
    EXPECT_EQ(tx_buf.free_size(), 128 - tx_buf.size());

    EXPECT_EQ(tx_buf.get_out_data(iov), 1);
    EXPECT_EQ(static_cast<int>(iov[0].iov_len), 2 * msg_size);
    EXPECT_EQ(gather(iov, 1), std::string(msg) + std::string(msg));
    tx_buf.consume(2 * msg_size);
    EXPECT_EQ(tx_buf.size(), 0);
    EXPECT_EQ(tx_buf.free_size(), 128);
}

TEST_F(TestTxBuf, TxBuf_FullCapacityUsable) {
    TxBuf tx_buf(64);
    char data[64];
    std::memset(data, 'Z', sizeof(data));
    EXPECT_TRUE(tx_buf.append(data, 64));
    EXPECT_EQ(tx_buf.size(), 64);
    EXPECT_EQ(tx_buf.free_size(), 0);
    EXPECT_FALSE(tx_buf.append(data, 1));

    struct iovec iov[2];
    EXPECT_EQ(tx_buf.get_out_data(iov), 1);
    EXPECT_EQ(iov[0].iov_len, 64u);
    EXPECT_EQ(std::memcmp(iov[0].iov_base, data, 64), 0);
}

TEST_F(TestTxBuf, TxBuf_WrapAroundAppendCorrectness) {
    TxBuf tx_buf(64);
    EXPECT_EQ(tx_buf.capacity(), 64);

    // Fill 48 bytes and leave one unconsumed so the buffer does not rewind: read index = 47
    char fill[48];
    std::memset(fill, 'A', sizeof(fill));
    EXPECT_TRUE(tx_buf.append(fill, 48));
    tx_buf.consume(47);
    EXPECT_EQ(tx_buf.size(), 1);

    // write index = 48: 32 bytes must wrap around
    char data[32];
    for (int i = 0; i < 32; i++) data[i] = static_cast<char>(i);
    EXPECT_TRUE(tx_buf.append(data, 32));
    EXPECT_EQ(tx_buf.size(), 33);

    struct iovec iov[2];
    EXPECT_EQ(tx_buf.get_out_data(iov), 2);
    EXPECT_EQ(iov[0].iov_len, 17u);  // 'A' + 16 bytes up to the end
    EXPECT_EQ(iov[1].iov_len, 16u);  // wrapped portion
    EXPECT_EQ(gather(iov, 2), std::string("A") + std::string(data, 32));

    // drop the 'A', content must be exactly `data`
    tx_buf.consume(1);
    EXPECT_EQ(tx_buf.get_out_data(iov), 2);
    EXPECT_EQ(gather(iov, 2), std::string(data, 32));

    // consume first segment, verify second
    tx_buf.consume(16);
    EXPECT_EQ(tx_buf.get_out_data(iov), 1);
    EXPECT_EQ(iov[0].iov_len, 16u);
    EXPECT_EQ(std::memcmp(iov[0].iov_base, data + 16, 16), 0);
}

TEST_F(TestTxBuf, TxBuf_GetOutDataEmpty) {
    TxBuf tx_buf(64);
    struct iovec iov[2];
    EXPECT_EQ(tx_buf.get_out_data(iov), 0);
}

TEST_F(TestTxBuf, TxBuf_ConsumeClampsToSize) {
    TxBuf tx_buf(64);
    char data[10] = {};
    EXPECT_TRUE(tx_buf.append(data, 10));
    tx_buf.consume(100);  // more than size
    EXPECT_EQ(tx_buf.size(), 0);
    EXPECT_EQ(tx_buf.free_size(), 64);
}

TEST_F(TestTxBuf, TxBuf_NonPowerOf2CapacityRoundsUp) {
    TxBuf tx_buf(100);
    EXPECT_EQ(tx_buf.capacity(), 128);
}

TEST_F(TestTxBuf, TxBuf_ZeroLengthAppend) {
    TxBuf tx_buf(64);
    EXPECT_TRUE(tx_buf.append(nullptr, 0));
    EXPECT_EQ(tx_buf.size(), 0);
}

TEST_F(TestTxBuf, TxBuf_ReserveCommit) {
    TxBuf tx_buf(64);
    std::string_view msg = "8=FIX.4.4|35=D|";
    const int msg_size = static_cast<int>(msg.size());

    char* p = tx_buf.reserve(msg_size);
    ASSERT_NE(p, nullptr);
    std::memcpy(p, msg.data(), msg_size);
    tx_buf.commit(msg_size);
    EXPECT_EQ(tx_buf.size(), msg_size);

    struct iovec iov[2];
    EXPECT_EQ(tx_buf.get_out_data(iov), 1);
    EXPECT_EQ(gather(iov, 1), std::string(msg));

    // commit fewer bytes than reserved
    p = tx_buf.reserve(32);
    ASSERT_NE(p, nullptr);
    std::memcpy(p, "abc", 3);
    tx_buf.commit(3);
    EXPECT_EQ(tx_buf.size(), msg_size + 3);
    EXPECT_EQ(tx_buf.get_out_data(iov), 1);
    EXPECT_EQ(gather(iov, 1), std::string(msg) + "abc");
}

TEST_F(TestTxBuf, TxBuf_ReserveFailsWhenNotContiguous) {
    TxBuf tx_buf(64);
    char fill[60] = {};
    EXPECT_TRUE(tx_buf.append(fill, 60));
    tx_buf.consume(40);  // read index = 40, write index = 60, free = 44

    // 44 bytes free in total but only 4 contiguous at the tail
    EXPECT_EQ(tx_buf.free_size(), 44);
    EXPECT_NE(tx_buf.reserve(4), nullptr);
    EXPECT_EQ(tx_buf.reserve(5), nullptr);
    EXPECT_EQ(tx_buf.reserve(45), nullptr);

    // append() still handles the wrap
    char data[20];
    for (int i = 0; i < 20; i++) data[i] = static_cast<char>('a' + i);
    EXPECT_TRUE(tx_buf.append(data, 20));
    tx_buf.consume(20);
    struct iovec iov[2];
    EXPECT_EQ(tx_buf.get_out_data(iov), 2);
    EXPECT_EQ(gather(iov, 2), std::string(data, 20));
}

TEST_F(TestTxBuf, TxBuf_RewindsWhenEmpty) {
    TxBuf tx_buf(64);
    char fill[40] = {};
    EXPECT_TRUE(tx_buf.append(fill, 40));
    tx_buf.consume(40);

    // without the rewind only 24 contiguous bytes would be available
    EXPECT_NE(tx_buf.reserve(64), nullptr);
}
