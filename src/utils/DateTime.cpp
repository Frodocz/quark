#include "quark/utils/DateTime.h"
#include "quark/QuarkError.h"
#include "quark/QuarkFormat.h"

#include <cstddef>
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

constexpr bool is_leap_year(int64_t y) noexcept { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

constexpr int64_t days_in_month(int64_t y, int64_t m) noexcept {
    constexpr int64_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && is_leap_year(y)) ? 29 : kDays[m - 1];
}

constexpr std::size_t kNoPosition = static_cast<std::size_t>(-1);

// Outcome of parsing: the timestamp, or a static reason and the input position it refers to
// (kNoPosition for a field out of range).
struct ParseOutcome {
    Nanoseconds ns{0};
    const char* error{nullptr};
    std::size_t pos{kNoPosition};
};

}  // namespace

// Runs a compiled TimestampFormat over one input.
struct TimestampFormatAccess {
    using Op = TimestampFormat::Op;

    static ParseOutcome run(const TimestampFormat& fmt, std::string_view ts) noexcept {
        Fields f;
        std::size_t pos = 0;
        for (uint8_t i = 0; i < fmt.count_; ++i) {
            const TimestampFormat::Step step = fmt.steps_[i];
            bool ok = true;
            switch (step.op) {
                case Op::Literal: ok = literal(ts, pos, step.literal); break;
                case Op::Year: ok = digits(ts, pos, 4, f.year); break;
                case Op::Month: ok = digits(ts, pos, 2, f.month); break;
                case Op::Day: ok = digits(ts, pos, 2, f.day); break;
                case Op::Hour: ok = digits(ts, pos, 2, f.hour); break;
                case Op::Minute: ok = digits(ts, pos, 2, f.minute); break;
                case Op::Second: ok = digits(ts, pos, 2, f.second); break;
                case Op::SecondFraction: ok = digits(ts, pos, 2, f.second) && fraction(ts, pos, f.frac_ns); break;
                case Op::UtcOffset: ok = utc_offset(ts, pos, f.offset_s); break;
            }
            if (!ok) return {0, "input doesn't match the format", pos};
        }
        if (pos != ts.size()) return {0, "trailing characters after the timestamp", pos};
        return finish(f);
    }

private:
    struct Fields {
        int64_t year{1970}, month{1}, day{1}, hour{0}, minute{0}, second{0};
        int64_t frac_ns{0};
        int64_t offset_s{0};
    };

    static bool literal(std::string_view ts, std::size_t& pos, char c) noexcept {
        if (pos >= ts.size() || ts[pos] != c) return false;
        ++pos;
        return true;
    }

    static bool digits(std::string_view ts, std::size_t& pos, std::size_t n, int64_t& out) noexcept {
        if (ts.size() - pos < n) return false;
        int64_t v = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const char c = ts[pos + k];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        pos += n;
        out = v;
        return true;
    }

    // optional ".ddd" or ",ddd" after the seconds: nanoseconds from the first 9 digits
    static bool fraction(std::string_view ts, std::size_t& pos, int64_t& out) noexcept {
        if (pos == ts.size() || (ts[pos] != '.' && ts[pos] != ',')) return true;
        ++pos;
        const std::size_t start = pos;
        int64_t ns = 0;
        int scale = 0;
        while (pos < ts.size() && ts[pos] >= '0' && ts[pos] <= '9') {
            if (scale < 9) {
                ns = ns * 10 + (ts[pos] - '0');
                ++scale;
            }
            ++pos;
        }
        if (pos == start) return false;  // a separator with no digits
        for (; scale < 9; ++scale) ns *= 10;
        out = ns;
        return true;
    }

    // Z, +hh, +hhmm or +hh:mm
    static bool utc_offset(std::string_view ts, std::size_t& pos, int64_t& out) noexcept {
        if (literal(ts, pos, 'Z')) {
            out = 0;
            return true;
        }
        if (pos == ts.size() || (ts[pos] != '+' && ts[pos] != '-')) return false;
        const int64_t sign = ts[pos++] == '-' ? -1 : 1;
        int64_t hh = 0, mm = 0;
        if (!digits(ts, pos, 2, hh)) return false;
        if (pos < ts.size() && ts[pos] == ':') {
            ++pos;
            if (!digits(ts, pos, 2, mm)) return false;
        } else if (pos < ts.size() && ts[pos] >= '0' && ts[pos] <= '9') {
            if (!digits(ts, pos, 2, mm)) return false;
        }
        if (hh > 23 || mm > 59) return false;
        out = sign * (hh * 3600 + mm * 60);
        return true;
    }

    static ParseOutcome finish(const Fields& f) noexcept {
        if (f.month < 1 || f.month > 12) return {0, "month out of range", kNoPosition};
        if (f.day < 1 || f.day > days_in_month(f.year, f.month)) return {0, "day out of range for the month", kNoPosition};
        if (f.hour > 23 || f.minute > 59 || f.second > 59) return {0, "time of day out of range", kNoPosition};
        // local time minus the UTC offset; the year has 4 digits, so the seconds can't overflow
        const int64_t seconds =
            days_from_civil(f.year, f.month, f.day) * 86'400 + f.hour * 3600 + f.minute * 60 + f.second - f.offset_s;
        int64_t ns = 0;
        if (__builtin_mul_overflow(seconds, kNsPerSecond, &ns) || __builtin_add_overflow(ns, f.frac_ns, &ns)) {
            return {0, "timestamp out of the nanosecond range (1677-09-21 to 2262-04-11)", kNoPosition};
        }
        return {ns, nullptr, kNoPosition};
    }
};

const char* TimestampFormat::compile(std::string_view format, TimestampFormat& out) noexcept {
    out.count_ = 0;
    const auto push = [&out](Op op, char c = 0) noexcept {
        if (out.count_ == kMaxSteps) return false;
        out.steps_[out.count_++] = Step{op, c};
        return true;
    };
    constexpr const char* kTooLong = "format too long";
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            if (!push(Op::Literal, format[i])) return kTooLong;
            continue;
        }
        if (++i == format.size()) return "format ends with '%'";
        bool ok = true;
        switch (format[i]) {
            case 'Y': ok = push(Op::Year); break;
            case 'm': ok = push(Op::Month); break;
            case 'd': ok = push(Op::Day); break;
            case 'H': ok = push(Op::Hour); break;
            case 'M': ok = push(Op::Minute); break;
            case 'S': ok = push(Op::Second); break;
            case 'z': ok = push(Op::UtcOffset); break;
            case '%': ok = push(Op::Literal, '%'); break;
            case 'F': ok = push(Op::Year) && push(Op::Literal, '-') && push(Op::Month) && push(Op::Literal, '-') && push(Op::Day); break;
            case 'T': ok = push(Op::Hour) && push(Op::Literal, ':') && push(Op::Minute) && push(Op::Literal, ':') && push(Op::Second); break;
            case 'E':
                if (format.substr(i, 3) != "E*S") return "unsupported conversion in format (supported: %Y %m %d %H %M %S %E*S %F %T %z %%)";
                i += 2;
                ok = push(Op::SecondFraction);
                break;
            default: return "unsupported conversion in format (supported: %Y %m %d %H %M %S %E*S %F %T %z %%)";
        }
        if (!ok) return kTooLong;
    }
    return nullptr;
}

TimestampFormat::TimestampFormat(std::string_view format) {
    if (const char* error = compile(format, *this); error != nullptr) {
        QUARK_THROW(QuarkError{fmtlib::format("TimestampFormat '{}': {}", format, error)});
    }
}

std::optional<TimestampFormat> TimestampFormat::create(std::string_view format) noexcept {
    TimestampFormat f;
    if (compile(format, f) != nullptr) return std::nullopt;
    return f;
}

std::optional<Nanoseconds> TimestampFormat::parse(std::string_view ts) const noexcept {
    const ParseOutcome r = TimestampFormatAccess::run(*this, ts);
    if (r.error != nullptr) [[unlikely]] {
        return std::nullopt;
    }
    return r.ns;
}

std::ostream& operator<<(std::ostream& os, const Date& date) {
    fmtlib::print(os, "{}-{:02d}-{:02d}", date.year, date.month, date.day);
    return os;
}

std::optional<Nanoseconds> try_parse_timestamp(std::string_view ts, std::string_view format) noexcept {
    const auto fmt = TimestampFormat::create(format);
    if (!fmt) [[unlikely]] {
        return std::nullopt;
    }
    return fmt->parse(ts);
}

Nanoseconds parse_timestamp(std::string_view ts, std::string_view format) {
    const TimestampFormat fmt{format};  // throws on a bad format
    const ParseOutcome r = TimestampFormatAccess::run(fmt, ts);
    if (r.error == nullptr) [[likely]] {
        return r.ns;
    }
    std::string msg = fmtlib::format("parse_timestamp failed: '{}', format: '{}', reason: {}", ts, format, r.error);
    if (r.pos != kNoPosition) msg += fmtlib::format(" (at offset {})", r.pos);
    QUARK_THROW(QuarkError{msg});
}

std::string Date::to_string() const {
    return fmtlib::format("{}-{:02d}-{:02d}", year, month, day);
}

Date Date::utc_today() noexcept {
    // the system clock (RdtscClock::sysns), not the TSC estimate: the date flips at real midnight
    return date_from_days(floor_div(RdtscClock::sysns(), kNsPerDay));
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
