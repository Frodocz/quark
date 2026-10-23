#pragma once

#include "quark/Attributes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// OpenSSL types, kept out of this header (see src/crypto/Ed25519Signer.cpp)
struct evp_pkey_st;
struct evp_md_ctx_st;

QUARK_BEGIN_NAMESPACE

namespace crypto {

/// Ed25519 (RFC 8032) signatures with a fixed private key, e.g. Binance Ed25519 API keys
/// (WebSocket API session.logon). Signing costs tens of microseconds: keep it off the per-order
/// path (log on once, then send unsigned requests). Not thread-safe: use one instance per thread.
class Ed25519Signer {
public:
    static constexpr std::size_t kKeySize = 32;
    static constexpr std::size_t kSignatureSize = 64;
    using PublicKey = std::array<uint8_t, kKeySize>;
    using Signature = std::array<uint8_t, kSignatureSize>;

    /// From a PKCS#8 PEM private key ("-----BEGIN PRIVATE KEY-----"). Throws QuarkError if it
    /// can't be parsed or isn't an Ed25519 key.
    static Ed25519Signer from_pem(std::string_view pem);

    /// From the raw 32-byte RFC 8032 private key. Throws QuarkError on an OpenSSL failure.
    static Ed25519Signer from_private_key(std::span<const uint8_t, kKeySize> key);

    ~Ed25519Signer();

    Ed25519Signer(const Ed25519Signer&) = delete;
    Ed25519Signer& operator=(const Ed25519Signer&) = delete;
    Ed25519Signer(Ed25519Signer&& rhs) noexcept;
    Ed25519Signer& operator=(Ed25519Signer&& rhs) noexcept;

    [[nodiscard]] Signature sign(std::string_view message);

    /// Base64 signature (88 chars) into `out`. Returns the length, or 0 if `out` is too small.
    std::size_t sign_base64(std::string_view message, std::span<char> out);

    [[nodiscard]] PublicKey public_key() const;

private:
    explicit Ed25519Signer(evp_pkey_st* pkey);

    evp_pkey_st* pkey_{nullptr};
    evp_md_ctx_st* md_ctx_{nullptr};
};

} // namespace crypto

QUARK_END_NAMESPACE
