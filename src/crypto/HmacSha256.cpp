// The precomputed-state construction needs the low-level SHA256_* API, deprecated (not removed)
// in OpenSSL 3. It is ~2x faster than duplicating an EVP_MAC context per message and doesn't
// allocate. If a future OpenSSL drops it, the EVP_MAC_CTX_dup fallback below is used. Must come
// before any OpenSSL header.
#define OPENSSL_SUPPRESS_DEPRECATED

#include "quark/crypto/HmacSha256.h"
#include "quark/crypto/Base64Encoding.h"

#include "OpenSslError.h"

#include <openssl/opensslv.h>

#if OPENSSL_VERSION_NUMBER < 0x30000000L
    #error "quark::crypto requires OpenSSL 3.0 or newer"
#endif

#if OPENSSL_VERSION_MAJOR == 3
    #define QUARK_HMAC_PRECOMPUTED_SHA256 1
    #include <openssl/sha.h>
#else
    #include <openssl/core_names.h>
    #include <openssl/evp.h>
#endif
#include <openssl/crypto.h>

#include <cassert>
#include <cstring>

QUARK_BEGIN_NAMESPACE

namespace crypto {

#if defined(QUARK_HMAC_PRECOMPUTED_SHA256)

struct HmacSha256::State {
    SHA256_CTX inner;  // after SHA-256(key ^ ipad)
    SHA256_CTX outer;  // after SHA-256(key ^ opad)

    ~State() { ::OPENSSL_cleanse(this, sizeof(*this)); }
};

HmacSha256::HmacSha256(std::string_view key) : state_{std::make_unique<State>()} {
    if (key.empty()) {
        QUARK_THROW(QuarkError{"HmacSha256: empty key"});
    }
    constexpr std::size_t kBlockSize = 64;
    unsigned char block[kBlockSize] = {};
    if (key.size() > kBlockSize) {
        // RFC 2104: keys longer than the block size are hashed first
        ::SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), block);
    } else {
        std::memcpy(block, key.data(), key.size());
    }
    unsigned char ipad[kBlockSize];
    unsigned char opad[kBlockSize];
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        ipad[i] = block[i] ^ 0x36;
        opad[i] = block[i] ^ 0x5c;
    }
    const bool ok = ::SHA256_Init(&state_->inner) == 1 && ::SHA256_Update(&state_->inner, ipad, kBlockSize) == 1 &&
                    ::SHA256_Init(&state_->outer) == 1 && ::SHA256_Update(&state_->outer, opad, kBlockSize) == 1;
    ::OPENSSL_cleanse(block, sizeof(block));
    ::OPENSSL_cleanse(ipad, sizeof(ipad));
    ::OPENSSL_cleanse(opad, sizeof(opad));
    if (!ok) detail::throw_openssl_error("HmacSha256: SHA256 init failed");
}

HmacSha256::Digest HmacSha256::sign(std::string_view data) {
    assert(state_ && "HmacSha256: moved from");
    Digest out;
    unsigned char inner_hash[kDigestSize];
    SHA256_CTX ctx = state_->inner;
    ::SHA256_Update(&ctx, data.data(), data.size());
    ::SHA256_Final(inner_hash, &ctx);
    ctx = state_->outer;
    ::SHA256_Update(&ctx, inner_hash, kDigestSize);
    ::SHA256_Final(out.data(), &ctx);
    ::OPENSSL_cleanse(&ctx, sizeof(ctx));
    ::OPENSSL_cleanse(inner_hash, sizeof(inner_hash));
    return out;
}

#else

struct HmacSha256::State {
    EVP_MAC* mac{nullptr};
    EVP_MAC_CTX* keyed{nullptr};  // never finalized, duplicated per message

    ~State() {
        ::EVP_MAC_CTX_free(keyed);
        ::EVP_MAC_free(mac);
    }
};

HmacSha256::HmacSha256(std::string_view key) : state_{std::make_unique<State>()} {
    if (key.empty()) {
        QUARK_THROW(QuarkError{"HmacSha256: empty key"});
    }
    state_->mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (state_->mac == nullptr) detail::throw_openssl_error("HmacSha256: EVP_MAC_fetch failed");
    state_->keyed = ::EVP_MAC_CTX_new(state_->mac);
    if (state_->keyed == nullptr) detail::throw_openssl_error("HmacSha256: EVP_MAC_CTX_new failed");
    char digest[] = "SHA256";
    const OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
                                 OSSL_PARAM_construct_end()};
    if (::EVP_MAC_init(state_->keyed, reinterpret_cast<const unsigned char*>(key.data()), key.size(), params) != 1) {
        detail::throw_openssl_error("HmacSha256: EVP_MAC_init failed");
    }
}

HmacSha256::Digest HmacSha256::sign(std::string_view data) {
    Digest out;
    std::size_t len = 0;
    EVP_MAC_CTX* ctx = ::EVP_MAC_CTX_dup(state_->keyed);
    const bool ok = ctx != nullptr &&
                    ::EVP_MAC_update(ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 1 &&
                    ::EVP_MAC_final(ctx, out.data(), &len, out.size()) == 1 && len == kDigestSize;
    ::EVP_MAC_CTX_free(ctx);
    if (!ok) detail::throw_openssl_error("HmacSha256: sign failed");
    return out;
}

#endif

HmacSha256::~HmacSha256() = default;
HmacSha256::HmacSha256(HmacSha256&&) noexcept = default;
HmacSha256& HmacSha256::operator=(HmacSha256&&) noexcept = default;

std::size_t HmacSha256::sign_hex(std::string_view data, std::span<char> out) {
    if (out.size() < 2 * kDigestSize) return 0;
    return hex_encode(sign(data), out);
}

std::size_t HmacSha256::sign_base64(std::string_view data, std::span<char> out) {
    if (out.size() < base64_encoded_size(kDigestSize)) return 0;
    return base64_encode(sign(data), out);
}

} // namespace crypto

QUARK_END_NAMESPACE
