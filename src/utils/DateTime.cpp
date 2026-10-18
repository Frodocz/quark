#include "quark/utils/DateTime.h"
#include "quark/QuarkError.h"
#include "quark/QuarkFormat.h"

#include "absl/time/clock.h"
#include "absl/time/time.h"

#include <cstdint>
#include <string>

QUARK_BEGIN_NAMESPACE

namespace {

struct CivilYmd {
    int32_t year;
    int32_t month;  // 1..12
    int32_t day;    // 1..31
};

// Floor division (rounds toward negative infinity), b > 0.
constexpr int64_t floor_div(int64_t a, int64_t b) noexcept {
    int64_t q = a / b;
    return (a % b < 0) ? q - 1 : q;
}

// Days since 1970-01-01 for a proleptic Gregorian date.
// Howard Hinnant's algorithm: https://howardhinnant.github.io/date_algorithms.html
// All arithmetic is signed, so day == 0 simply means "the day before the 1st"
// instead of wrapping around.
constexpr int64_t days_from_civil(int64_t y, int64_t m, int64_t d) noexcept {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                    // [0, 399]
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;  // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
    return era * 146097 + doe - 719468;
}

// Inverse of days_from_civil.
constexpr CivilYmd civil_from_days(int64_t z) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                                         // [0, 146096]
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;    // [0, 399]
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                  // [0, 365]
    const int64_t mp = (5 * doy + 2) / 153;                                       // [0, 11]
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;                               // [1, 31]
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;                                  // [1, 12]
    const int64_t y = yoe + era * 400 + (m <= 2);
    return {static_cast<int32_t>(y), static_cast<int32_t>(m), static_cast<int32_t>(d)};
}

constexpr Date date_from_days(int64_t days) noexcept {
    const auto c = civil_from_days(days);
    return Date(static_cast<int16_t>(c.year), static_cast<int8_t>(c.month), static_cast<int8_t>(c.day));
}

constexpr int64_t days_from_date(const Date& d) noexcept {
    return days_from_civil(d.year, d.month, d.day);
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const Date& date) {
    fmtlib::print(os, "{}-{:02d}-{:02d}", date.year, date.month, date.day);
    return os;
}

Nanoseconds parse_timestamp(std::string_view ts, std::string_view format) {
    absl::Time tm;
    std::string err;  // absl explains *why* parsing failed; keep it in the message
    if (absl::ParseTime(format, ts, &tm, &err)) [[likely]] {
        return absl::ToUnixNanos(tm);
    }
    QUARK_THROW(QuarkError{fmtlib::format("parse_timestamp failed: {}, format: {}, reason: {}", ts, format, err)});
}

std::string Date::to_string() const {
    return fmtlib::format("{}-{:02d}-{:02d}", year, month, day);
}

Date Date::utc_today() noexcept {
    // Now() is always >= 1970 so plain division is a floor here.
    return date_from_days(absl::ToUnixSeconds(absl::Now()) / 86'400);
}

Nanoseconds Date::to_epoch_timestamp_ns() const noexcept {
    return days_from_date(*this) * kNsPerDay;
}

Date Date::next_day() const noexcept { return date_from_days(days_from_date(*this) + 1); }
Date Date::prev_day() const noexcept { return date_from_days(days_from_date(*this) - 1); }

Date& Date::operator+=(int32_t date_gap) noexcept {
    *this = date_from_days(days_from_date(*this) + static_cast<int64_t>(date_gap));
    return *this;
}

Date& Date::operator-=(int32_t date_gap) noexcept {
    // widen first: -date_gap would overflow for INT32_MIN
    *this = date_from_days(days_from_date(*this) - static_cast<int64_t>(date_gap));
    return *this;
}

// All six fields are derived from one day number; the start-of-day timestamps are
// plain multiplication because a UTC day is always exactly 24h.
void TradingDay::set_from_days(int64_t days) noexcept {
    current_date_ = date_from_days(days);
    next_date_ = date_from_days(days + 1);
    previous_date_ = date_from_days(days - 1);

    current_day_start_time_ns_ = days * kNsPerDay;
    next_day_start_time_ns_ = current_day_start_time_ns_ + kNsPerDay;
    previous_day_start_time_ns_ = current_day_start_time_ns_ - kNsPerDay;
}

bool TradingDay::init(int64_t now_ns) noexcept {
    if (now_ns < 0) [[unlikely]] {
        return false;
    }
    set_from_days(now_ns / kNsPerDay);
    return true;
}

TradingDay& TradingDay::operator+=(int64_t date_gap) noexcept {
    // current_day_start_time_ns_ is an exact multiple of kNsPerDayS, so this division is exact
    set_from_days(current_day_start_time_ns_ / kNsPerDay + date_gap);
    return *this;
}

TradingDay& TradingDay::operator-=(int64_t date_gap) noexcept {
    set_from_days(current_day_start_time_ns_ / kNsPerDay - date_gap);
    return *this;
}

Date TradingDay::get_date(int64_t time_ns) noexcept {
    return date_from_days(floor_div(time_ns, kNsPerDay));
}

int64_t TradingDay::to_unix_time_ns(int yyyymmdd) noexcept {
    return days_from_date(Date(yyyymmdd)) * kNsPerDay;
}

QUARK_END_NAMESPACE
