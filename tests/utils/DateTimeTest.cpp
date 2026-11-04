// parse_timestamp / try_parse_timestamp and Date::utc_today, checked against glibc (timegm,
// gmtime_r, strftime) as an independent implementation.
#include "quark/QuarkError.h"
#include "quark/utils/DateTime.h"

#include <gtest/gtest.h>

#include "QuarkTest.h"

#include <ctime>
#include <random>
#include <string>
#include <type_traits>
#include <utility>

using namespace quark;

namespace {

// seconds since the epoch of a UTC civil time, from glibc
int64_t utc_seconds(int y, int m, int d, int hh = 0, int mm = 0, int ss = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    return static_cast<int64_t>(::timegm(&tm));
}

Nanoseconds ns(int64_t seconds, int64_t frac_ns = 0) { return seconds * kNsPerSecond + frac_ns; }

}  // namespace

TEST(DateTimeParse, Iso8601WithFractionAndZulu) {
    EXPECT_EQ(parse_timestamp("2026-11-04T09:30:00.123456789Z", "%Y-%m-%dT%H:%M:%E*S%z"),
              ns(utc_seconds(2026, 11, 4, 9, 30, 0), 123'456'789));
    EXPECT_EQ(parse_timestamp("2026-11-04 09:30:00", "%F %T"), ns(utc_seconds(2026, 11, 4, 9, 30, 0)));
}

TEST(DateTimeParse, FixSendingTime) {
    EXPECT_EQ(parse_timestamp("20261104-09:30:00.123", "%Y%m%d-%H:%M:%E*S"),
              ns(utc_seconds(2026, 11, 4, 9, 30, 0), 123'000'000));
}

TEST(DateTimeParse, FractionIsOptionalAndTruncatedBeyondNanoseconds) {
    const int64_t base = utc_seconds(2026, 1, 2, 3, 4, 5);
    EXPECT_EQ(parse_timestamp("03:04:05", "%H:%M:%E*S") % kNsPerDay, ns(base) % kNsPerDay);
    EXPECT_EQ(parse_timestamp("2026-01-02 03:04:05.5", "%F %H:%M:%E*S"), ns(base, 500'000'000));
    EXPECT_EQ(parse_timestamp("2026-01-02 03:04:05,25", "%F %H:%M:%E*S"), ns(base, 250'000'000));
    EXPECT_EQ(parse_timestamp("2026-01-02 03:04:05.123456789987", "%F %H:%M:%E*S"), ns(base, 123'456'789));
    EXPECT_FALSE(try_parse_timestamp("2026-01-02 03:04:05.", "%F %H:%M:%E*S"));  // separator, no digits
}

TEST(DateTimeParse, UtcOffsetsGiveUtc) {
    const Nanoseconds utc = ns(utc_seconds(2026, 11, 4, 1, 30, 0));
    EXPECT_EQ(parse_timestamp("2026-11-04 09:30:00+08:00", "%F %T%z"), utc);
    EXPECT_EQ(parse_timestamp("2026-11-04 09:30:00+0800", "%F %T%z"), utc);
    EXPECT_EQ(parse_timestamp("2026-11-04 09:30:00+08", "%F %T%z"), utc);
    EXPECT_EQ(parse_timestamp("2026-11-03 20:00:00-05:30", "%F %T%z"), utc);
    EXPECT_EQ(parse_timestamp("2026-11-04 01:30:00Z", "%F %T%z"), utc);
    EXPECT_FALSE(try_parse_timestamp("2026-11-04 09:30:00+24:00", "%F %T%z"));
    EXPECT_FALSE(try_parse_timestamp("2026-11-04 09:30:00 08:00", "%F %T%z"));
}

TEST(DateTimeParse, MissingFieldsDefaultToTheEpoch) {
    EXPECT_EQ(parse_timestamp("09:30", "%H:%M"), 9 * kNsPerHour + 30 * kNsPerMin);
    EXPECT_EQ(parse_timestamp("2026", "%Y"), ns(utc_seconds(2026, 1, 1)));
    EXPECT_EQ(parse_timestamp("", ""), 0);
}

TEST(DateTimeParse, BeforeTheEpochIsNegative) {
    EXPECT_EQ(parse_timestamp("1969-12-31 23:59:59.5", "%F %H:%M:%E*S"), -500'000'000);
    EXPECT_EQ(parse_timestamp("1900-01-01", "%F"), ns(utc_seconds(1900, 1, 1)));
}

TEST(DateTimeParse, LiteralsAndPercent) {
    EXPECT_EQ(parse_timestamp("d=2026/11/04 100%", "d=%Y/%m/%d 100%%"), ns(utc_seconds(2026, 11, 4)));
    EXPECT_FALSE(try_parse_timestamp("2026-11-04", "%Y/%m/%d"));
}

TEST(DateTimeParse, CalendarIsChecked) {
    EXPECT_TRUE(try_parse_timestamp("2024-02-29", "%F"));   // leap year
    EXPECT_TRUE(try_parse_timestamp("2000-02-29", "%F"));   // divisible by 400
    EXPECT_FALSE(try_parse_timestamp("2025-02-29", "%F"));  // not a leap year
    EXPECT_FALSE(try_parse_timestamp("1900-02-29", "%F"));  // divisible by 100
    EXPECT_FALSE(try_parse_timestamp("2026-04-31", "%F"));
    EXPECT_FALSE(try_parse_timestamp("2026-13-01", "%F"));
    EXPECT_FALSE(try_parse_timestamp("2026-00-01", "%F"));
    EXPECT_FALSE(try_parse_timestamp("2026-01-00", "%F"));
    EXPECT_FALSE(try_parse_timestamp("24:00:00", "%T"));
    EXPECT_FALSE(try_parse_timestamp("23:60:00", "%T"));
    EXPECT_FALSE(try_parse_timestamp("23:59:60", "%T"));
}

TEST(DateTimeParse, ShapeErrors) {
    EXPECT_FALSE(try_parse_timestamp("2026-11-4", "%F"));      // fixed width
    EXPECT_FALSE(try_parse_timestamp("2026-11-04x", "%F"));    // trailing characters
    EXPECT_FALSE(try_parse_timestamp("2026-11", "%F"));        // input too short
    EXPECT_FALSE(try_parse_timestamp("2O26-11-04", "%F"));     // not a digit
    EXPECT_FALSE(try_parse_timestamp("Wed 2026", "%a %Y"));    // unsupported conversion
    EXPECT_FALSE(try_parse_timestamp("12", "%E3S"));           // only %E*S of the %E family
    EXPECT_FALSE(try_parse_timestamp("2026", "%Y%"));          // format ends with '%'
}

TEST(DateTimeParse, ResultMustFitInt64Nanoseconds) {
    EXPECT_TRUE(try_parse_timestamp("2262-04-11 00:00:00", "%F %T"));
    EXPECT_FALSE(try_parse_timestamp("2262-04-12 00:00:00", "%F %T"));
    EXPECT_TRUE(try_parse_timestamp("1677-09-22 00:00:00", "%F %T"));
    EXPECT_FALSE(try_parse_timestamp("1677-09-21 00:00:00", "%F %T"));
    EXPECT_FALSE(try_parse_timestamp("9999-12-31", "%F"));
}

TEST(DateTimeParse, ParseTimestampReportsTheReason) {
    QUARK_EXPECT_ERROR((void)parse_timestamp("2026-11-04x", "%F"), "trailing characters after the timestamp \\(at offset 10\\)");
    // a field out of range has no position; the death-test message continues with " (file:line)"
    QUARK_EXPECT_ERROR((void)parse_timestamp("2026-02-30", "%F"), "reason: day out of range for the month($| \\(/)");
    QUARK_EXPECT_ERROR((void)parse_timestamp("Wed", "%a"), "unsupported conversion");
}

TEST(TimestampFormat, BadFormatsFailAtConstruction) {
    QUARK_EXPECT_ERROR(TimestampFormat{"%Y-%m-%d %a"}, "TimestampFormat '%Y-%m-%d %a': unsupported conversion");
    QUARK_EXPECT_ERROR(TimestampFormat{"%Y%"}, "format ends with '%'");
    QUARK_EXPECT_ERROR(TimestampFormat{std::string(TimestampFormat::kMaxSteps + 1, '-')}, "format too long");
    EXPECT_FALSE(TimestampFormat::create("%E3S"));
    EXPECT_FALSE(TimestampFormat::create("%"));
    EXPECT_TRUE(TimestampFormat::create(std::string(TimestampFormat::kMaxSteps, '-')));
    EXPECT_FALSE(try_parse_timestamp("x", "%q"));  // try_parse_timestamp never throws on a bad format
}

TEST(TimestampFormat, ParsesLikeTryParseTimestamp) {
    const char* const formats[] = {"%Y-%m-%dT%H:%M:%E*S%z", "%Y%m%d-%H:%M:%E*S", "%F %T", "%H:%M"};
    for (const char* f : formats) {
        const TimestampFormat fmt{f};
        for (const char* ts : {"2026-11-04T09:30:00.123456789Z", "2026-11-04T09:30:00+08:00", "20261104-09:30:00.5",
                               "2026-11-04 09:30:00", "09:30", "garbage", ""}) {
            EXPECT_EQ(fmt.parse(ts), try_parse_timestamp(ts, f)) << f << " / " << ts;
        }
    }
    const TimestampFormat iso{"%Y-%m-%dT%H:%M:%E*S%z"};
    EXPECT_EQ(iso.parse("2026-11-04T09:30:00.123456789Z"), ns(utc_seconds(2026, 11, 4, 9, 30, 0), 123'456'789));
    EXPECT_FALSE(iso.parse("2026-11-04T09:30:00.123456789"));  // missing offset: nullopt, no throw
}

TEST(TimestampFormat, IsAValueTypeWithANoexceptParse) {
    static_assert(std::is_trivially_copyable_v<TimestampFormat>);
    static_assert(noexcept(std::declval<const TimestampFormat&>().parse(std::string_view{})));
    static_assert(noexcept(try_parse_timestamp(std::string_view{}, std::string_view{})));
    const TimestampFormat a{"%F"};
    const TimestampFormat b = a;  // copies keep working
    EXPECT_EQ(b.parse("2026-11-04"), ns(utc_seconds(2026, 11, 4)));
}

// 200k random instants in the representable range, formatted by glibc, parsed back
TEST(DateTimeParse, MatchesGlibcOnRandomInstants) {
    std::mt19937_64 rng{20261104};
    const int64_t lo = utc_seconds(1678, 1, 1);
    const int64_t hi = utc_seconds(2262, 1, 1);
    std::uniform_int_distribution<int64_t> secs{lo, hi};
    std::uniform_int_distribution<int64_t> frac{0, kNsPerSecond - 1};
    char buf[64];
    for (int i = 0; i < 200'000; ++i) {
        const auto t = static_cast<std::time_t>(secs(rng));
        std::tm tm{};
        ASSERT_NE(::gmtime_r(&t, &tm), nullptr);
        const std::size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        const int64_t f = frac(rng);
        const std::string ts = std::string(buf, n) + "." + std::to_string(kNsPerSecond + f).substr(1) + "Z";
        const auto parsed = try_parse_timestamp(ts, "%Y-%m-%dT%H:%M:%E*S%z");
        ASSERT_TRUE(parsed.has_value()) << ts;
        ASSERT_EQ(*parsed, ns(static_cast<int64_t>(t), f)) << ts;
    }
}

TEST(DateTimeParse, UtcTodayMatchesTheSystemClock) {
    const std::time_t before = std::time(nullptr);
    const Date today = Date::utc_today();
    const std::time_t after = std::time(nullptr);
    std::tm tm{};
    // retry-free: if the day changed between the two reads, accept either
    const auto as_date = [&tm](std::time_t t) {
        ::gmtime_r(&t, &tm);
        return Date(static_cast<int16_t>(tm.tm_year + 1900), static_cast<int8_t>(tm.tm_mon + 1),
                    static_cast<int8_t>(tm.tm_mday));
    };
    EXPECT_TRUE(today == as_date(before) || today == as_date(after)) << today.to_string();
}
