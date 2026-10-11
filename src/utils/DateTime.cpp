#include "quark/utils/DateTime.h"
#include "quark/QuarkError.h"
#include "quark/QuarkFormat.h"

#include "absl/time/clock.h"
#include "absl/time/time.h"

QUARK_BEGIN_NAMESPACE

std::ostream& operator<<(std::ostream& os, const Date& date) {
    fmtlib::print(os, "{}-{:02d}-{:02d}", date.year, date.month, date.day);
    return os;
}

Nanoseconds parse_timestamp(std::string_view ts, std::string_view format) {
    absl::Time tm;
    if (absl::ParseTime(format, ts, &tm, nullptr)) {
        return absl::ToUnixNanos(tm);
    } else {
        QUARK_THROW(QuarkError{fmtlib::format("parse_timestamp failed: {}, format: {}", ts, format)});
    }
}

std::string Date::to_string() const {
    return fmtlib::format("{}-{:02d}-{:02d}", year, month, day);
}

Date Date::utc_today() {
    auto civil_day = absl::ToCivilDay(absl::Now(), absl::UTCTimeZone());
    return Date(civil_day.year(), civil_day.month(), civil_day.day());
}

Nanoseconds Date::to_epoch_timestamp() const {
    absl::CivilDay civil_day(year, month, day);
    return absl::ToUnixNanos(absl::FromCivil(civil_day, absl::UTCTimeZone()));
}

Date Date::next_day() const {
    absl::CivilDay next_day(year, month, day);
    next_day += 1;
    return Date(next_day.year(), next_day.month(), next_day.day());
}

QUARK_END_NAMESPACE
