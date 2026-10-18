#pragma once

#include "quark/Attributes.h"
#include "quark/utils/RdtscClock.h"

#include <cstdint>
#include <string>
#include <string_view>

using Nanoseconds = int64_t;

QUARK_BEGIN_NAMESPACE

constexpr Nanoseconds kNsPerUs = 1'000ll;
constexpr Nanoseconds kNsPerMs = 1'000'000ll;
constexpr Nanoseconds kNsPerSecond = 1'000'000'000ll;
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

    constexpr bool operator==(const Date&) const noexcept = default;
    constexpr auto operator<=>(const Date&) const noexcept = default;

    Date& operator+=(int32_t date_gap) noexcept;
    Date& operator-=(int32_t date_gap) noexcept;

    static Date utc_today() noexcept;

    std::string to_string() const;

    Nanoseconds to_epoch_timestamp_ns() const noexcept;

    Date next_day() const noexcept;
    Date prev_day() const noexcept;

    constexpr int to_int_date() const noexcept {
        return year * 10000 + month * 100 + day;
    }
} QUARK_PACKED;
static_assert(sizeof(Date) == sizeof(int16_t) + sizeof(int8_t) + sizeof(int8_t));

QUARK_API std::ostream& operator<<(std::ostream& os, const Date& date);

// Only support UTC Time Zone
class QUARK_API TradingDay {
public:
    constexpr TradingDay() = default;
    explicit TradingDay(int64_t now_ns) { init(now_ns); }

    bool init(int64_t now_ns) noexcept;

    Date get_prev_date() const noexcept { return previous_date_; }
    int64_t get_prev_sod_ns() const noexcept { return previous_day_start_time_ns_; }
    Date get_curr_date() const noexcept { return current_date_; }
    int64_t get_curr_sod_ns() const noexcept { return current_day_start_time_ns_; }
    Date get_next_date() const noexcept { return next_date_; }
    int64_t get_next_sod_ns() const noexcept { return next_day_start_time_ns_; }

    void check_and_update(int64_t now_ns) noexcept {
        if (now_ns >= next_day_start_time_ns_) {
            init(now_ns);
        }
    }

    TradingDay& operator+=(int64_t date_gap) noexcept;
    TradingDay& operator-=(int64_t date_gap) noexcept;

    static Date get_date(int64_t time_ns) noexcept;
    static int64_t to_unix_time_ns(int yyyymmdd) noexcept;
private:
    void set_from_days(int64_t days) noexcept;

    int64_t next_day_start_time_ns_{0}; // hot path field read
    int64_t current_day_start_time_ns_{0};
    int64_t previous_day_start_time_ns_{0};
    Date next_date_;
    Date current_date_;
    Date previous_date_;
};
static_assert(sizeof(TradingDay) == 40);

QUARK_END_NAMESPACE
