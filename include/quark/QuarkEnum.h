#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

#include <magic_enum/magic_enum.hpp>

#include "quark/Attributes.h"

/**
 * Enum reflection, backed by magic_enum (https://github.com/Neargye/magic_enum). Include this header rather than
 * magic_enum directly, so every module of a process uses the magic_enum pinned by quark.
 *
 * Reflection covers enumerator values in [-128, 127] by default. Enums with values outside it declare their range
 * next to their definition, at global scope:
 *
 *     template <>
 *     struct magic_enum::customize::enum_range<my::ErrorCode> {
 *         static constexpr int min = 0;
 *         static constexpr int max = 400;
 *     };
 *
 * A value outside the reflected range has no name: enum_name() returns "" and enum_from_value() nullopt.
 * Don't redefine MAGIC_ENUM_RANGE_MIN / MAGIC_ENUM_RANGE_MAX instead: it slows every compilation, and they would
 * have to be identical in every translation unit of the process.
 */

QUARK_BEGIN_NAMESPACE

template <class E>
concept Enum = std::is_enum_v<E>;

/// Name of the enumerator, "" if `value` names none. A constexpr table lookup: fine on hot paths.
template <Enum E>
[[nodiscard]] constexpr std::string_view enum_name(E value) noexcept {
    return magic_enum::enum_name(value);
}

/// The enumerator called `name` (case-sensitive), nullopt if there is none. Linear search: cold paths only
/// (configuration, command line).
template <Enum E>
[[nodiscard]] constexpr std::optional<E> enum_from_name(std::string_view name) noexcept {
    return magic_enum::enum_cast<E>(name);
}

/// The enumerator with the underlying value `value`, nullopt if there is none. Validates enums decoded from
/// untrusted bytes (wire messages) before they are used, e.g. in a switch.
template <Enum E>
[[nodiscard]] constexpr std::optional<E> enum_from_value(std::underlying_type_t<E> value) noexcept {
    return magic_enum::enum_cast<E>(value);
}

/// Number of enumerators of E
template <Enum E>
inline constexpr std::size_t enum_count_v = magic_enum::enum_count<E>();

/// All enumerators of E in increasing order of value, as a constexpr std::array
template <Enum E>
[[nodiscard]] constexpr auto enum_values() noexcept {
    return magic_enum::enum_values<E>();
}

QUARK_END_NAMESPACE
