#pragma once

#include "quark/Attributes.h"

#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>

QUARK_BEGIN_NAMESPACE

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "mixed-endian platforms are not supported");

/// Reverses the byte order of an unsigned integer (std::byteswap is C++23).
template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE constexpr T byte_swap(T x) noexcept {
    if constexpr (sizeof(T) == 1) {
        return x;
    } else if constexpr (sizeof(T) == 2) {
        return __builtin_bswap16(x);
    } else if constexpr (sizeof(T) == 4) {
        return __builtin_bswap32(x);
    } else {
        static_assert(sizeof(T) == 8);
        return __builtin_bswap64(x);
    }
}

/// Host <-> big-endian (network order, e.g. WebSocket and TCP/IP headers).
template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE constexpr T host_to_be(T x) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byte_swap(x);
    } else {
        return x;
    }
}

template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE constexpr T be_to_host(T x) noexcept {
    return host_to_be(x);
}

/// Host <-> little-endian (e.g. SBE binary market data).
template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE constexpr T host_to_le(T x) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        return byte_swap(x);
    } else {
        return x;
    }
}

template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE constexpr T le_to_host(T x) noexcept {
    return host_to_le(x);
}

/// Unaligned loads/stores of a fixed-endian integer from/to a byte buffer.
template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE T load_be(const void* src) noexcept {
    T v;
    std::memcpy(&v, src, sizeof(T));
    return be_to_host(v);
}

template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE T load_le(const void* src) noexcept {
    T v;
    std::memcpy(&v, src, sizeof(T));
    return le_to_host(v);
}

template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE void store_be(void* dst, T v) noexcept {
    v = host_to_be(v);
    std::memcpy(dst, &v, sizeof(T));
}

template <std::unsigned_integral T>
QUARK_ALWAYS_INLINE void store_le(void* dst, T v) noexcept {
    v = host_to_le(v);
    std::memcpy(dst, &v, sizeof(T));
}

QUARK_END_NAMESPACE
