#pragma once

#include "quark/Attributes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
    #include <nmmintrin.h>
    #define QUARK_CRC32C_X86 1
#elif defined(__ARM_FEATURE_CRC32)
    #include <arm_acle.h>
    #define QUARK_CRC32C_ARM 1
#endif

QUARK_BEGIN_NAMESPACE

namespace detail {

// Every variant works on the inverted CRC state and gives the same result: data checksummed on
// one machine verifies on another.

/// Byte-wise table of the reflected Castagnoli polynomial, built at compile time.
inline constexpr auto kCrc32cTable = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        t[i] = c;
    }
    return t;
}();

/// Portable: any CPU.
inline uint32_t crc32c_software(const uint8_t* p, std::size_t n, uint32_t c) noexcept {
    for (; n > 0; --n, ++p) c = kCrc32cTable[(c ^ *p) & 0xFF] ^ (c >> 8);
    return c;
}

#if defined(QUARK_CRC32C_X86)
/// SSE4.2: 8 bytes per instruction. Only call when has_sse42().
__attribute__((target("sse4.2"))) inline uint32_t crc32c_sse42(const uint8_t* p, std::size_t n, uint32_t crc) noexcept {
    uint64_t c = crc;
    for (; n >= 8; n -= 8, p += 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        c = _mm_crc32_u64(c, v);
    }
    for (; n > 0; --n, ++p) c = _mm_crc32_u8(static_cast<uint32_t>(c), *p);
    return static_cast<uint32_t>(c);
}

/// Every x86-64 CPU since 2008 has it; checked once anyway.
inline bool has_sse42() noexcept {
    static const bool v = __builtin_cpu_supports("sse4.2");
    return v;
}
#elif defined(QUARK_CRC32C_ARM)
/// ARMv8 CRC32 extension (compiled in when the target has it, e.g. -march=armv8-a+crc).
inline uint32_t crc32c_arm(const uint8_t* p, std::size_t n, uint32_t c) noexcept {
    for (; n >= 8; n -= 8, p += 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        c = __crc32cd(c, v);
    }
    for (; n > 0; --n, ++p) c = __crc32cb(c, *p);
    return c;
}
#endif

}  // namespace detail

/// CRC-32C (Castagnoli), continuing from `crc`: hardware instructions where the CPU has them
/// (x86 SSE4.2, ARMv8 CRC), a table elsewhere.
inline uint32_t crc32c(const void* data, std::size_t n, uint32_t crc = 0) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
#if defined(QUARK_CRC32C_X86)
    const uint32_t c = detail::has_sse42() ? detail::crc32c_sse42(p, n, ~crc) : detail::crc32c_software(p, n, ~crc);
#elif defined(QUARK_CRC32C_ARM)
    const uint32_t c = detail::crc32c_arm(p, n, ~crc);
#else
    const uint32_t c = detail::crc32c_software(p, n, ~crc);
#endif
    return ~c;
}

QUARK_END_NAMESPACE
