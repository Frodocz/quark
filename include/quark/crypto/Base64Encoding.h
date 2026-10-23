#pragma once

#include "quark/Attributes.h"

#include <cstddef>
#include <cstdint>
#include <span>

QUARK_BEGIN_NAMESPACE

/// Length of the padded standard base64 encoding (RFC 4648) of `n` bytes.
constexpr std::size_t base64_encoded_size(std::size_t n) noexcept { return 4 * ((n + 2) / 3); }

/// Standard padded base64 (RFC 4648 section 4) of `in` into `out`. Returns the number of chars
/// written, or 0 if `out` is too small. The output is not NUL-terminated.
inline std::size_t base64_encode(std::span<const uint8_t> in, std::span<char> out) noexcept {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const std::size_t n = in.size();
    if (out.size() < base64_encoded_size(n)) return 0;

    std::size_t o = 0;
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = (uint32_t{in[i]} << 16) | (uint32_t{in[i + 1]} << 8) | in[i + 2];
        out[o++] = kTable[(v >> 18) & 0x3F];
        out[o++] = kTable[(v >> 12) & 0x3F];
        out[o++] = kTable[(v >> 6) & 0x3F];
        out[o++] = kTable[v & 0x3F];
    }
    if (const std::size_t rem = n - i; rem > 0) {
        const uint32_t v = (uint32_t{in[i]} << 16) | (rem == 2 ? uint32_t{in[i + 1]} << 8 : 0);
        out[o++] = kTable[(v >> 18) & 0x3F];
        out[o++] = kTable[(v >> 12) & 0x3F];
        out[o++] = rem == 2 ? kTable[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    return o;
}

/// Lowercase hex of `in` into `out` (2 chars per byte), e.g. for HMAC signatures. Returns the
/// number of chars written, or 0 if `out` is too small. The output is not NUL-terminated.
inline std::size_t hex_encode(std::span<const uint8_t> in, std::span<char> out) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    if (out.size() < 2 * in.size()) return 0;
    std::size_t o = 0;
    for (const uint8_t b : in) {
        out[o++] = kDigits[b >> 4];
        out[o++] = kDigits[b & 0xF];
    }
    return o;
}

QUARK_END_NAMESPACE
