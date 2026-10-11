#pragma once

#include "quark/Attributes.h"
#include "quark/utils/RdtscClock.h"

#include <cstdint>
#include <string>
#include <string_view>

using Nanoseconds = uint64_t;

QUARK_BEGIN_NAMESPACE

constexpr Nanoseconds kNsPerUs = 1'000ul;
constexpr Nanoseconds kNsPerMs = 1'000'000ul;
constexpr Nanoseconds kNsPerSecond = 1'000'000'000ul;
constexpr Nanoseconds kNsPerMin = 60 * kNsPerSecond;
constexpr Nanoseconds kNsPerHour = 60 * kNsPerMin;
constexpr Nanoseconds kNsPerDay = 24 * kNsPerHour;

QUARK_ALWAYS_INLINE Nanoseconds GetSysTimestamp() {
    return RdtscClock::sysns();
}

QUARK_ALWAYS_INLINE Nanoseconds GetTscTimestamp() {
    return RdtscClock::tscns();
}

Nanoseconds parse_timestamp(std::string_view ts, std::string_view format);

struct QUARK_API Date {
    int16_t year{0};
    int8_t month{0};
    int8_t day{0};

    constexpr Date() = default;
    constexpr Date(int16_t y, int8_t m, int8_t d) : year{y}, month{m}, day{d} {}
    constexpr explicit Date(int yyyymmdd)
        : year{static_cast<int16_t>(yyyymmdd / 10000)},
          month{static_cast<int8_t>((yyyymmdd / 100) % 100)},
          day{static_cast<int8_t>(yyyymmdd % 100)} {}

    constexpr bool operator==(const Date& other) noexcept {
        return year == other.year && month == other.month && day == other.day;
    }

    constexpr bool operator!=(const Date& other) noexcept {
        return !(*this == other);
    }

    static Date utc_today();

    std::string to_string() const;

    Nanoseconds to_epoch_timestamp() const;

    Date next_day() const;

    constexpr int to_int_date() const noexcept {
        return year * 10000 + month * 100 + day;
    }
} QUARK_PACKED;
static_assert(sizeof(Date) == sizeof(int16_t) + sizeof(int8_t) + sizeof(int8_t));

QUARK_API std::ostream& operator<<(std::ostream& os, const Date& date);

QUARK_END_NAMESPACE
