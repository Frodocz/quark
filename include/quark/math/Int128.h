#pragma once

// 128-bit integers for quark's integer helpers and ScaleInt<SCALE, int128_t>.
//
// GCC and Clang provide __int128 on x86-64, but in strict ISO mode (-std=c++20, no GNU extensions) the
// standard traits don't count it as an integer: std::integral<__int128>, std::is_signed_v and
// std::make_unsigned_t reject it, and std::to_chars has no overload for it. std::numeric_limits and the
// __builtin_*_overflow builtins do support it. This header supplies the missing pieces:
//   * int128_t / uint128_t, spelled through __extension__ so -Wpedantic stays quiet
//   * the Integer / SignedInteger concepts: the std concepts plus the 128-bit types
//   * make_unsigned_t, also for the 128-bit types
//   * to_chars for uint128_t (std::to_chars for the 64-bit and smaller types)

#include "quark/Attributes.h"

#include <charconv>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <type_traits>

QUARK_BEGIN_NAMESPACE

namespace math {

__extension__ using int128_t = __int128;
__extension__ using uint128_t = unsigned __int128;

template <class T>
concept Integer = std::integral<T> || std::same_as<std::remove_cv_t<T>, int128_t> ||
                  std::same_as<std::remove_cv_t<T>, uint128_t>;

template <class T>
concept SignedInteger = std::signed_integral<T> || std::same_as<std::remove_cv_t<T>, int128_t>;

template <class T>
struct make_unsigned {
    using type = std::make_unsigned_t<T>;
};
template <>
struct make_unsigned<int128_t> {
    using type = uint128_t;
};
template <>
struct make_unsigned<uint128_t> {
    using type = uint128_t;
};

template <class T>
using make_unsigned_t = typename make_unsigned<T>::type;

// Decimal digits of v, at most 39 bytes, no NUL. Returns one past the last written character.
// Up to two 64-bit conversions plus one 128-bit division by 10^19; no allocation.
[[nodiscard]] inline char* to_chars(char* first, uint128_t v) noexcept {
    constexpr uint64_t kTen19 = 10'000'000'000'000'000'000ULL;
    if (v <= UINT64_MAX) {
        return std::to_chars(first, first + 20, static_cast<uint64_t>(v)).ptr;
    }
    const uint128_t high = v / kTen19;
    const auto low = static_cast<uint64_t>(v % kTen19);
    first = to_chars(first, high);  // high < 2^128 / 10^19 < 10^20: no further recursion past one level
    char digits[20];
    const char* end = std::to_chars(digits, digits + sizeof(digits), low).ptr;
    const auto n = static_cast<std::size_t>(end - digits);
    std::memset(first, '0', 19 - n);  // low is printed on exactly 19 digits
    std::memcpy(first + (19 - n), digits, n);
    return first + 19;
}

}  // namespace math

QUARK_END_NAMESPACE
