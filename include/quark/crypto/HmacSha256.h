#pragma once

#include "quark/Attributes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

QUARK_BEGIN_NAMESPACE

namespace crypto {

/// HMAC-SHA256 with a fixed key, e.g. a Binance/Bitget API secret signing request parameters.
/// The SHA-256 states after absorbing the inner and outer padded keys (RFC 2104) are computed
/// once in the constructor, so sign() hashes only the message and never allocates. Not
/// thread-safe: use one instance per thread.
class HmacSha256 {
public:
    static constexpr std::size_t kDigestSize = 32;
    using Digest = std::array<uint8_t, kDigestSize>;

    /// Throws QuarkError on an empty key or an OpenSSL failure.
    explicit HmacSha256(std::string_view key);
    ~HmacSha256();

    HmacSha256(const HmacSha256&) = delete;
    HmacSha256& operator=(const HmacSha256&) = delete;
    HmacSha256(HmacSha256&&) noexcept;
    HmacSha256& operator=(HmacSha256&&) noexcept;

    [[nodiscard]] Digest sign(std::string_view data);

    /// Lowercase hex digest (64 chars) into `out`. Returns the length, or 0 if `out` is too small.
    std::size_t sign_hex(std::string_view data, std::span<char> out);

    /// Base64 digest (44 chars) into `out`. Returns the length, or 0 if `out` is too small.
    std::size_t sign_base64(std::string_view data, std::span<char> out);

private:
    struct State;  // OpenSSL types, kept out of this header
    std::unique_ptr<State> state_;
};

} // namespace crypto

QUARK_END_NAMESPACE
