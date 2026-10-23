#include "quark/crypto/Base64Encoding.h"
#include "gtest/gtest.h"

#include <string>
#include <string_view>

using namespace quark;

namespace {
std::string b64(std::string_view in) {
    std::string out(base64_encoded_size(in.size()), '\0');
    const std::size_t n = base64_encode({reinterpret_cast<const uint8_t*>(in.data()), in.size()}, out);
    EXPECT_EQ(n, out.size());
    return out;
}
} // namespace

class TestEncoding : public ::testing::Test {};

// RFC 4648 section 10 test vectors
TEST_F(TestEncoding, Base64Rfc4648) {
    EXPECT_EQ(b64(""), "");
    EXPECT_EQ(b64("f"), "Zg==");
    EXPECT_EQ(b64("fo"), "Zm8=");
    EXPECT_EQ(b64("foo"), "Zm9v");
    EXPECT_EQ(b64("foob"), "Zm9vYg==");
    EXPECT_EQ(b64("fooba"), "Zm9vYmE=");
    EXPECT_EQ(b64("foobar"), "Zm9vYmFy");
}

TEST_F(TestEncoding, Base64AllByteValuesAndTooSmall) {
    const uint8_t bytes[] = {0x00, 0xFF, 0xFE, 0x3E, 0x3F};
    char out[8];
    ASSERT_EQ(base64_encode(bytes, out), 8u);
    EXPECT_EQ(std::string_view(out, 8), "AP/+Pj8=");

    char small[7];
    EXPECT_EQ(base64_encode(bytes, small), 0u);
}

TEST_F(TestEncoding, Hex) {
    const uint8_t bytes[] = {0x00, 0x0F, 0xA5, 0xFF};
    char out[8];
    ASSERT_EQ(hex_encode(bytes, out), 8u);
    EXPECT_EQ(std::string_view(out, 8), "000fa5ff");

    char small[7];
    EXPECT_EQ(hex_encode(bytes, small), 0u);
}
