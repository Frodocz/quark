#pragma once

#include "quark/Attributes.h"
#include "quark/utils/HwEndian.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#if defined(__AVX2__)
    #include <immintrin.h>
#endif

QUARK_BEGIN_NAMESPACE

namespace net {

// ---------------------------------------------------------------- RFC 6455 framing

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

[[nodiscard]] constexpr bool ws_is_control(WsOpcode op) noexcept { return (static_cast<uint8_t>(op) & 0x08u) != 0; }

struct WsFrameHeader {
    bool fin{true};
    bool masked{false};
    WsOpcode opcode{WsOpcode::Text};
    uint64_t payload_len{0};
    uint8_t mask_key[4]{};
};

/// ws_parse_header() result for a protocol violation.
inline constexpr std::size_t kWsParseError = std::numeric_limits<std::size_t>::max();

/// Largest header: 2 bytes + 8-byte length + 4-byte mask key.
inline constexpr std::size_t kWsMaxHeaderSize = 14;

/// Parses a frame header at the start of `buf`. Returns the header size, 0 if `buf` doesn't hold
/// the whole header yet, or kWsParseError on a protocol violation (reserved bits or opcodes,
/// non-minimal length encoding, oversized or fragmented control frame). No field is read before
/// the buffer is known to contain it.
[[nodiscard]] QUARK_ALWAYS_INLINE std::size_t ws_parse_header(std::span<const uint8_t> buf,
                                                             WsFrameHeader& out) noexcept {
    const std::size_t len = buf.size();
    if (len < 2) return 0;
    const uint8_t b0 = buf[0];
    const uint8_t b1 = buf[1];
    if ((b0 & 0x70u) != 0) return kWsParseError;  // RSV1-3: no extension negotiated

    out.fin = (b0 & 0x80u) != 0;
    out.opcode = static_cast<WsOpcode>(b0 & 0x0Fu);
    switch (out.opcode) {
        case WsOpcode::Continuation:
        case WsOpcode::Text:
        case WsOpcode::Binary:
        case WsOpcode::Close:
        case WsOpcode::Ping:
        case WsOpcode::Pong: break;
        default: return kWsParseError;  // reserved opcodes
    }

    out.masked = (b1 & 0x80u) != 0;
    uint64_t payload_len = b1 & 0x7Fu;
    std::size_t pos = 2;
    if (payload_len == 126) {
        if (len < 4) return 0;
        payload_len = load_be<uint16_t>(buf.data() + 2);
        if (payload_len < 126) return kWsParseError;
        pos = 4;
    } else if (payload_len == 127) {
        if (len < 10) return 0;
        payload_len = load_be<uint64_t>(buf.data() + 2);
        if ((payload_len >> 63) != 0 || payload_len <= 0xFFFF) return kWsParseError;
        pos = 10;
    }
    if (ws_is_control(out.opcode) && (payload_len > 125 || !out.fin)) return kWsParseError;
    if (out.masked) {
        if (len < pos + 4) return 0;
        std::memcpy(out.mask_key, buf.data() + pos, 4);
        pos += 4;
    }
    out.payload_len = payload_len;
    return pos;
}

/// Header size of a (masked) client frame carrying `payload_len` bytes.
[[nodiscard]] constexpr std::size_t ws_client_header_size(std::size_t payload_len) noexcept {
    return 2 + 4 + (payload_len > 0xFFFF ? 8 : (payload_len >= 126 ? 2 : 0));
}

/// Writes a masked client frame header into `dst` (at least ws_client_header_size() bytes).
/// Returns the bytes written, 0 if `dst` is too small. The payload is masked separately.
QUARK_ALWAYS_INLINE std::size_t ws_encode_client_header(std::span<uint8_t> dst, WsOpcode opcode,
                                                        std::size_t payload_len, const uint8_t mask_key[4],
                                                        bool fin) noexcept {
    const std::size_t need = ws_client_header_size(payload_len);
    if (dst.size() < need) return 0;
    uint8_t* p = dst.data();
    p[0] = static_cast<uint8_t>((fin ? 0x80u : 0u) | static_cast<uint8_t>(opcode));
    std::size_t pos = 1;
    if (payload_len < 126) {
        p[pos++] = static_cast<uint8_t>(0x80u | payload_len);
    } else if (payload_len <= 0xFFFF) {
        p[pos++] = 0x80u | 126u;
        store_be<uint16_t>(p + pos, static_cast<uint16_t>(payload_len));
        pos += 2;
    } else {
        p[pos++] = 0x80u | 127u;
        store_be<uint64_t>(p + pos, payload_len);
        pos += 8;
    }
    std::memcpy(p + pos, mask_key, 4);
    return pos + 4;
}

// ---------------------------------------------------------------- masking

/// XORs `data[0, len)` in place with the 4-byte `key` (as on the wire), starting at key
/// position `key_offset` (non-zero when one payload is masked in several calls).
QUARK_ALWAYS_INLINE void ws_mask(uint8_t* data, std::size_t len, const uint8_t key[4],
                                 std::size_t key_offset = 0) noexcept {
    uint8_t rot[4];
    for (std::size_t k = 0; k < 4; ++k) rot[k] = key[(key_offset + k) & 3];
    uint32_t pattern;
    std::memcpy(&pattern, rot, 4);
    std::size_t i = 0;
#if defined(__AVX2__)
    if (len >= 32) {
        const __m256i vkey = _mm256_set1_epi32(static_cast<int>(pattern));
        for (; i + 32 <= len; i += 32) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(data + i), _mm256_xor_si256(v, vkey));
        }
    }
#endif
    // i is a multiple of 4 here, so the pattern is still aligned with the key
    const uint64_t mask64 = (static_cast<uint64_t>(pattern) << 32) | pattern;
    for (; i + 8 <= len; i += 8) {
        uint64_t chunk;
        std::memcpy(&chunk, data + i, 8);
        chunk ^= mask64;
        std::memcpy(data + i, &chunk, 8);
    }
    for (; i < len; ++i) data[i] ^= rot[i & 3];
}

/// Per-session generator of frame mask keys: PCG-XSH-RR 64/32, a few ALU ops per key, seeded once
/// from the OS. Mask keys only stop proxies from treating client frames as cache poisoning
/// payloads; they don't need a CSPRNG per frame, but must not be constant.
class WsMaskKeyGen {
public:
    explicit WsMaskKeyGen(uint64_t seed) noexcept : state_{seed} {}

    QUARK_ALWAYS_INLINE uint32_t next() noexcept {
        const uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

private:
    uint64_t state_;
};

} // namespace net

QUARK_END_NAMESPACE
