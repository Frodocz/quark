#include "quark/net/WsFrame.h"
#include "gtest/gtest.h"

#include <set>
#include <vector>

using namespace quark::net;

class TestWsFrame : public ::testing::Test {};

TEST_F(TestWsFrame, ParseSmallUnmaskedFrame) {
    const uint8_t buf[] = {0x81, 0x05, 'h', 'e', 'l', 'l', 'o'};
    WsFrameHeader h;
    EXPECT_EQ(ws_parse_header({buf, 1}, h), 0u);  // incomplete
    ASSERT_EQ(ws_parse_header(buf, h), 2u);
    EXPECT_TRUE(h.fin);
    EXPECT_FALSE(h.masked);
    EXPECT_EQ(h.opcode, WsOpcode::Text);
    EXPECT_EQ(h.payload_len, 5u);
}

TEST_F(TestWsFrame, ParseExtendedLengths) {
    WsFrameHeader h;
    const uint8_t len16[] = {0x82, 126, 0x01, 0x00};
    EXPECT_EQ(ws_parse_header({len16, 3}, h), 0u);
    ASSERT_EQ(ws_parse_header(len16, h), 4u);
    EXPECT_EQ(h.payload_len, 256u);
    EXPECT_EQ(h.opcode, WsOpcode::Binary);

    const uint8_t len64[] = {0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0};
    EXPECT_EQ(ws_parse_header({len64, 9}, h), 0u);
    ASSERT_EQ(ws_parse_header(len64, h), 10u);
    EXPECT_EQ(h.payload_len, 65536u);

    const uint8_t masked[] = {0x81, 0x83, 1, 2, 3, 4};
    EXPECT_EQ(ws_parse_header({masked, 5}, h), 0u);
    ASSERT_EQ(ws_parse_header(masked, h), 6u);
    EXPECT_TRUE(h.masked);
    EXPECT_EQ(h.mask_key[3], 4);
}

TEST_F(TestWsFrame, ProtocolViolations) {
    WsFrameHeader h;
    const uint8_t rsv[] = {0xC1, 0x00};                       // RSV1 without an extension
    const uint8_t reserved_op[] = {0x83, 0x00};               // opcode 3
    const uint8_t non_minimal16[] = {0x81, 126, 0x00, 0x7D};  // 125 in the 16-bit form
    const uint8_t non_minimal64[] = {0x81, 127, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    const uint8_t msb64[] = {0x81, 127, 0x80, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t big_control[] = {0x89, 126, 0x00, 0x7E};    // 126-byte ping
    const uint8_t fragmented_control[] = {0x09, 0x00};        // ping without FIN
    for (const std::span<const uint8_t> frame : {std::span<const uint8_t>(rsv), std::span<const uint8_t>(reserved_op),
                                                 std::span<const uint8_t>(non_minimal16), std::span<const uint8_t>(non_minimal64),
                                                 std::span<const uint8_t>(msb64), std::span<const uint8_t>(big_control),
                                                 std::span<const uint8_t>(fragmented_control)}) {
        EXPECT_EQ(ws_parse_header(frame, h), kWsParseError);
    }
}

TEST_F(TestWsFrame, EncodeClientHeaderRoundTrips) {
    const uint8_t key[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    for (const std::size_t len : {std::size_t{0}, std::size_t{125}, std::size_t{126}, std::size_t{65535},
                                  std::size_t{65536}}) {
        uint8_t hdr[kWsMaxHeaderSize];
        const std::size_t n = ws_encode_client_header(hdr, WsOpcode::Binary, len, key, true);
        ASSERT_EQ(n, ws_client_header_size(len));
        WsFrameHeader h;
        ASSERT_EQ(ws_parse_header({hdr, n}, h), n);
        EXPECT_EQ(h.payload_len, len);
        EXPECT_TRUE(h.masked);
        EXPECT_EQ(std::memcmp(h.mask_key, key, 4), 0);
    }
    uint8_t small[5];
    EXPECT_EQ(ws_encode_client_header(small, WsOpcode::Text, 10, key, true), 0u);
}

TEST_F(TestWsFrame, MaskMatchesScalarReferenceAtAnyOffsetAndLength) {
    const uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
    for (std::size_t len = 0; len < 100; ++len) {
        for (std::size_t offset = 0; offset < 4; ++offset) {
            std::vector<uint8_t> data(len), expected(len);
            for (std::size_t i = 0; i < len; ++i) data[i] = expected[i] = static_cast<uint8_t>(i * 7 + 3);
            for (std::size_t i = 0; i < len; ++i) expected[i] ^= key[(offset + i) & 3];
            ws_mask(data.data(), len, key, offset);
            ASSERT_EQ(data, expected) << "len " << len << " offset " << offset;
            ws_mask(data.data(), len, key, offset);  // masking twice restores the input
            for (std::size_t i = 0; i < len; ++i) ASSERT_EQ(data[i], static_cast<uint8_t>(i * 7 + 3));
        }
    }
}

TEST_F(TestWsFrame, MaskKeysVary) {
    WsMaskKeyGen gen(12345);
    std::set<uint32_t> keys;
    for (int i = 0; i < 1000; ++i) keys.insert(gen.next());
    EXPECT_GT(keys.size(), 990u);
}
