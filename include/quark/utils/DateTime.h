#pragma once

#include "quark/Attributes.h"
#include "quark/utils/RdtscClock.h"

#include <cstdint>
#include <optional>
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

// Parses a UTC timestamp with a strptime-style `format`, without allocating; returns nanoseconds
// since the Unix epoch. Conversions (fixed width, so no ambiguity between adjacent fields):
//   %Y  4-digit year          %m  2-digit month      %d  2-digit day
//   %H  2-digit hour (0-23)   %M  2-digit minute     %S  2-digit second (0-59)
//   %E*S  %S with an optional fraction after '.' or ',': 1 or more digits, beyond 9 truncated
//   %F  = %Y-%m-%d            %T  = %H:%M:%S
//   %z  UTC offset: Z, +hh, +hhmm or +hh:mm (or '-'); the result is still UTC
//   %%  a literal '%'
// Any other format character must match itself; the whole input must be consumed. Fields not
// in the format default to 1970-01-01 00:00:00 UTC. Dates are checked (no 02-30), as is the
// range of the result (1677-09-21 to 2262-04-11). Examples:
//   "2026-11-04T09:30:00.123456789Z", "%Y-%m-%dT%H:%M:%E*S%z"
//   "20261104-09:30:00.123" (FIX SendingTime), "%Y%m%d-%H:%M:%E*S"
//
// On a hot path prefer TimestampFormat (below): it checks the format once, at startup, and parses
// without re-reading it.
//
// Returns std::nullopt for an unsupported format or input that doesn't match: no exception, never
// takes the process down.
QUARK_API std::optional<Nanoseconds> try_parse_timestamp(std::string_view ts, std::string_view format) noexcept;

// try_parse_timestamp(), throwing QuarkError with the reason and position on failure, a fatal
// error with QUARK_NO_EXCEPTIONS (QuarkError.h): for configuration and tools, not for market data.
QUARK_API Nanoseconds parse_timestamp(std::string_view ts, std::string_view format);

// A timestamp format (the conversions above) checked once, then applied to every message:
//
//   const TimestampFormat fmt{"%Y-%m-%dT%H:%M:%E*S%z"};    // at startup: throws on a bad format
//   if (auto ns = fmt.parse(msg.time)) { ... } else { drop or count the message }
//
// parse() is noexcept and doesn't allocate; a wrong format is a startup error, a malformed
// message a std::nullopt. Trivially copyable, no heap.
class QUARK_API TimestampFormat {
public:
    // Throws QuarkError naming the problem (a fatal error with QUARK_NO_EXCEPTIONS).
    explicit TimestampFormat(std::string_view format);
    // Non-throwing alternative: std::nullopt for an unsupported format.
    static std::optional<TimestampFormat> create(std::string_view format) noexcept;

    [[nodiscard]] std::optional<Nanoseconds> parse(std::string_view ts) const noexcept;

    enum class Op : uint8_t { Literal, Year, Month, Day, Hour, Minute, Second, SecondFraction, UtcOffset };
    struct Step {
        Op op;
        char literal;  // Op::Literal only
    };
    static constexpr std::size_t kMaxSteps = 64;  // "%F %T" is 11 steps

private:
    TimestampFormat() = default;
    static const char* compile(std::string_view format, TimestampFormat& out) noexcept;  // null or a reason

    Step steps_[kMaxSteps]{};
    uint8_t count_{0};

    friend struct TimestampFormatAccess;
};

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
