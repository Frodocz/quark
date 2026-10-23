#include "quark/crypto/Ed25519Signer.h"
#include "quark/crypto/HmacSha256.h"
#include "quark/QuarkError.h"
#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace quark;
using namespace quark::crypto;

namespace {

std::vector<uint8_t> from_hex(std::string_view hex) {
    std::vector<uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

template <std::size_t N>
std::string to_hex(const std::array<uint8_t, N>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    for (const uint8_t b : bytes) {
        s += kDigits[b >> 4];
        s += kDigits[b & 0xF];
    }
    return s;
}

std::string_view as_view(const std::vector<uint8_t>& v) {
    return {reinterpret_cast<const char*>(v.data()), v.size()};
}

} // namespace

class TestHmacSha256 : public ::testing::Test {};

// RFC 4231 test cases 1 and 2
TEST_F(TestHmacSha256, Rfc4231) {
    HmacSha256 h1(as_view(from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")));
    EXPECT_EQ(to_hex(h1.sign("Hi There")), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    HmacSha256 h2("Jefe");
    EXPECT_EQ(to_hex(h2.sign("what do ya want for nothing?")),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// RFC 4231 test case 6: a key longer than the SHA-256 block is hashed first
TEST_F(TestHmacSha256, Rfc4231LongKey) {
    HmacSha256 h(std::string(131, '\xaa'));
    EXPECT_EQ(to_hex(h.sign("Test Using Larger Than Block-Size Key - Hash Key First")),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// Example from the Binance API docs (SIGNED endpoint security)
TEST_F(TestHmacSha256, BinanceDocExample) {
    HmacSha256 h("NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j");
    char out[64];
    ASSERT_EQ(h.sign_hex("symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1"
                         "&recvWindow=5000&timestamp=1499827319559",
                         out),
              64u);
    EXPECT_EQ(std::string_view(out, 64), "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71");
}

// The keyed context is re-initialized per sign(): results must not depend on previous calls
TEST_F(TestHmacSha256, RepeatedSignsAreIndependent) {
    HmacSha256 h("Jefe");
    for (int i = 0; i < 3; ++i) {
        (void)h.sign("some other message, of a different length");
        EXPECT_EQ(to_hex(h.sign("what do ya want for nothing?")),
                  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
}

TEST_F(TestHmacSha256, Base64AndSmallBuffers) {
    HmacSha256 h1(as_view(from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")));
    char b64[44];
    ASSERT_EQ(h1.sign_base64("Hi There", b64), 44u);
    EXPECT_EQ(std::string_view(b64, 44), "sDRMYdjbOFNcqK/OrwvxK4gdwgDJgz2nJuk3bC4yz/c=");

    char small[10];
    EXPECT_EQ(h1.sign_hex("x", small), 0u);
    EXPECT_EQ(h1.sign_base64("x", small), 0u);
}

TEST_F(TestHmacSha256, MoveAndEmptyKey) {
    HmacSha256 a("Jefe");
    HmacSha256 b(std::move(a));
    EXPECT_EQ(to_hex(b.sign("what do ya want for nothing?")),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    EXPECT_THROW(HmacSha256(""), QuarkError);
}

class TestEd25519Signer : public ::testing::Test {};

// RFC 8032 section 7.1, TEST 1 (empty message) and TEST 2
TEST_F(TestEd25519Signer, Rfc8032) {
    auto key1 = from_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    auto s1 = Ed25519Signer::from_private_key(std::span<const uint8_t, 32>(key1.data(), 32));
    EXPECT_EQ(to_hex(s1.public_key()), "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    EXPECT_EQ(to_hex(s1.sign("")),
              "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46b"
              "d25bf5f0595bbe24655141438e7a100b");

    auto key2 = from_hex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
    auto s2 = Ed25519Signer::from_private_key(std::span<const uint8_t, 32>(key2.data(), 32));
    EXPECT_EQ(to_hex(s2.public_key()), "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    for (int i = 0; i < 2; ++i) {  // the context is re-initialized per message
        EXPECT_EQ(to_hex(s2.sign("\x72")),
                  "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c"
                  "387b2eaeb4302aeeb00d291612bb0c00");
    }
}

// The RFC 8032 TEST 1 key wrapped as PKCS#8 PEM (as Binance's key generator produces)
TEST_F(TestEd25519Signer, FromPem) {
    constexpr std::string_view kPem =
        "-----BEGIN PRIVATE KEY-----\n"
        "MC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n"
        "-----END PRIVATE KEY-----\n";
    auto signer = Ed25519Signer::from_pem(kPem);
    EXPECT_EQ(to_hex(signer.public_key()), "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");

    char b64[88];
    ASSERT_EQ(signer.sign_base64("", b64), 88u);
    EXPECT_EQ(std::string_view(b64, 88),
              "5VZDAMNgrHKQhuLMgG6CioSHfx645dl02HPgZSJJAVVfuIIVkKM7rMYeOXAc+bRr0lv18FlbviRlUUFDjnoQCw==");
}

TEST_F(TestEd25519Signer, RejectsBadKeys) {
    EXPECT_THROW(Ed25519Signer::from_pem("not a pem"), QuarkError);
    // a valid PEM private key of another type (EC P-256)
    constexpr std::string_view kEcPem =
        "-----BEGIN PRIVATE KEY-----\n"
        "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg0fRaLrDt+sdm12Q5\n"
        "C1gVVvHJG2O9ipblAf6huMl4v0ihRANCAAToZheuDBvokx22wUQryJ2T6vhxtsOO\n"
        "2A4kwICfP/WkgoVk5nBIJF1WjcxPk0TjIf3KWZWXKPBXLN4ryP5dWZBc\n"
        "-----END PRIVATE KEY-----\n";
    try {
        (void)Ed25519Signer::from_pem(kEcPem);
        FAIL() << "expected QuarkError";
    } catch (const QuarkError& e) {
        EXPECT_NE(std::string_view(e.what()).find("not an Ed25519 key"), std::string_view::npos) << e.what();
    }
}
