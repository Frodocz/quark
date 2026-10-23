#include "quark/utils/HwEndian.h"
#include "gtest/gtest.h"

#include <cstdint>

using namespace quark;

static_assert(byte_swap(uint8_t{0x12}) == 0x12);
static_assert(byte_swap(uint16_t{0x1234}) == 0x3412);
static_assert(byte_swap(uint32_t{0x12345678}) == 0x78563412);
static_assert(byte_swap(uint64_t{0x0123456789ABCDEF}) == 0xEFCDAB8967452301);
static_assert(be_to_host(host_to_be(uint32_t{0xDEADBEEF})) == 0xDEADBEEF);
static_assert(le_to_host(host_to_le(uint64_t{0xDEADBEEF01020304})) == 0xDEADBEEF01020304);

class TestHwEndian : public ::testing::Test {};

TEST_F(TestHwEndian, LoadStoreBigEndian) {
    uint8_t buf[9] = {};
    store_be<uint16_t>(buf + 1, 0x1234);  // unaligned
    EXPECT_EQ(buf[1], 0x12);
    EXPECT_EQ(buf[2], 0x34);
    EXPECT_EQ(load_be<uint16_t>(buf + 1), 0x1234);

    store_be<uint64_t>(buf + 1, 0x0102030405060708);
    for (int i = 0; i < 8; ++i) EXPECT_EQ(buf[1 + i], i + 1);
    EXPECT_EQ(load_be<uint64_t>(buf + 1), 0x0102030405060708u);
}

TEST_F(TestHwEndian, LoadStoreLittleEndian) {
    uint8_t buf[5] = {};
    store_le<uint32_t>(buf + 1, 0x01020304);
    EXPECT_EQ(buf[1], 0x04);
    EXPECT_EQ(buf[4], 0x01);
    EXPECT_EQ(load_le<uint32_t>(buf + 1), 0x01020304u);
}
