#include "quark/utils/DateTime.h"
#include "quark/utils/RdtscClock.h"
#include "gtest/gtest.h"

using quark::Date;
using quark::RdtscClock;
using quark::TradingDay;

namespace {
// seconds since epoch -> nanoseconds
constexpr int64_t ns(int64_t sec) { return sec * quark::kNsPerSecond; }

constexpr long long kSecondsPerDay = quark::kNsPerDay / quark::kNsPerSecond;

// Known UTC midnights (seconds since epoch)
constexpr int64_t kEpoch_19700101 = 0;
constexpr int64_t kSod_20000229 = 951782400;
constexpr int64_t kSod_20200229 = 1582934400;
constexpr int64_t kSod_20200301 = 1583020800;
constexpr int64_t kSod_20210301 = 1614556800;
constexpr int64_t kSod_20211108 = 1636329600;
constexpr int64_t kSod_20211231 = 1640908800;
constexpr int64_t kSod_20220101 = 1640995200;
constexpr int64_t kSod_21000228 = 4107456000;
constexpr int64_t kSod_21000301 = 4107542400;

// Checks that all three dates are consistent with all three start-of-day timestamps.
void expect_consistent(const TradingDay& td, int curr_yyyymmdd) {
    EXPECT_EQ(curr_yyyymmdd, td.get_curr_date().to_int_date());
    EXPECT_EQ(td.get_curr_sod_ns(), TradingDay::to_unix_time_ns(td.get_curr_date().to_int_date()));
    EXPECT_EQ(td.get_next_sod_ns(), TradingDay::to_unix_time_ns(td.get_next_date().to_int_date()));
    EXPECT_EQ(td.get_prev_sod_ns(), TradingDay::to_unix_time_ns(td.get_prev_date().to_int_date()));
    EXPECT_EQ(td.get_curr_sod_ns() + quark::kNsPerDay, td.get_next_sod_ns());
    EXPECT_EQ(td.get_curr_sod_ns() - quark::kNsPerDay, td.get_prev_sod_ns());
}
} // namespace

// =====================================================================
// Date
// =====================================================================
class TestDate : public ::testing::Test {
public:
    static void SetUpTestSuite() { RdtscClock::calibrate(); }
};

TEST_F(TestDate, DefaultConstructedIsZero) {
    Date d;
    EXPECT_EQ(0, d.year);
    EXPECT_EQ(0, d.month);
    EXPECT_EQ(0, d.day);
    EXPECT_EQ(0, d.to_int_date());
}

TEST_F(TestDate, ConstructFromYMD) {
    Date d(2021, 11, 8);
    EXPECT_EQ(2021, d.year);
    EXPECT_EQ(11, d.month);
    EXPECT_EQ(8, d.day);
    EXPECT_EQ(20211108, d.to_int_date());
}

TEST_F(TestDate, ConstructFromInt) {
    Date d(20200229);
    EXPECT_EQ(2020, d.year);
    EXPECT_EQ(2, d.month);
    EXPECT_EQ(29, d.day);
    EXPECT_EQ(20200229, d.to_int_date());
}

TEST_F(TestDate, IntRoundTrip) {
    for (int v : {19700101, 20000229, 20200229, 20211231, 20220101, 20991231}) {
        EXPECT_EQ(v, Date(v).to_int_date()) << v;
    }
}

TEST_F(TestDate, EqualityOperators) {
    // NOTE: operator== / != are declared non-const in the header. They are called on
    // non-const objects here so this compiles; see the note about EXPECT_EQ(Date, Date).
    Date a(2021, 11, 8);
    Date b(20211108);
    Date c(2021, 11, 9);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
    EXPECT_FALSE(Date(2022, 11, 8) == a);  // year differs
    EXPECT_FALSE(Date(2021, 10, 8) == a);  // month differs
}

TEST_F(TestDate, NextDay) {
    EXPECT_EQ(20211109, Date(20211108).next_day().to_int_date());
    EXPECT_EQ(20211201, Date(20211130).next_day().to_int_date());   // 30-day month end
    EXPECT_EQ(20211101, Date(20211031).next_day().to_int_date());   // 31-day month end
    EXPECT_EQ(20220101, Date(20211231).next_day().to_int_date());   // year end
    EXPECT_EQ(20210301, Date(20210228).next_day().to_int_date());   // non-leap Feb
    EXPECT_EQ(20200229, Date(20200228).next_day().to_int_date());   // leap Feb
    EXPECT_EQ(20200301, Date(20200229).next_day().to_int_date());
}

TEST_F(TestDate, PrevDay) {
    EXPECT_EQ(20211107, Date(20211108).prev_day().to_int_date());
    EXPECT_EQ(20211130, Date(20211201).prev_day().to_int_date());
    EXPECT_EQ(20211031, Date(20211101).prev_day().to_int_date());
    EXPECT_EQ(20211231, Date(20220101).prev_day().to_int_date());
    EXPECT_EQ(20210228, Date(20210301).prev_day().to_int_date());
    EXPECT_EQ(20200229, Date(20200301).prev_day().to_int_date());
    EXPECT_EQ(20200228, Date(20200229).prev_day().to_int_date());
}

TEST_F(TestDate, NextDayDoesNotModifyOriginal) {
    Date d(20211108);
    (void)d.next_day();
    (void)d.prev_day();
    EXPECT_EQ(20211108, d.to_int_date());
}

TEST_F(TestDate, PlusEqualsAndMinusEquals) {
    Date d(20211231);
    d += 1;
    EXPECT_EQ(20220101, d.to_int_date());
    d -= 1;
    EXPECT_EQ(20211231, d.to_int_date());
    d += 0;
    EXPECT_EQ(20211231, d.to_int_date());
    d += 365;
    EXPECT_EQ(20221231, d.to_int_date());
    d -= 365;
    EXPECT_EQ(20211231, d.to_int_date());
}

TEST_F(TestDate, PlusEqualsNegativeEqualsMinusEquals) {
    Date a(20200301), b(20200301);
    a += -1;
    b -= 1;
    EXPECT_EQ(b.to_int_date(), a.to_int_date());
    EXPECT_EQ(20200229, a.to_int_date());
}

TEST_F(TestDate, PlusEqualsAcrossLeapYear) {
    Date d(20200228);
    d += 2;
    EXPECT_EQ(20200301, d.to_int_date());
    Date e(20210228);
    e += 2;
    EXPECT_EQ(20210302, e.to_int_date());
    Date f(20200101);
    f += 366;  // 2020 is a leap year
    EXPECT_EQ(20210101, f.to_int_date());
}

TEST_F(TestDate, PlusEqualsReturnsSelf) {
    Date d(20211108);
    Date& r = (d += 1);
    EXPECT_EQ(&d, &r);
    Date& r2 = (d -= 1);
    EXPECT_EQ(&d, &r2);
}

TEST_F(TestDate, ToEpochTimestampns) {
    EXPECT_EQ(static_cast<uint64_t>(ns(kEpoch_19700101)), Date(19700101).to_epoch_timestamp_ns());
    EXPECT_EQ(static_cast<uint64_t>(ns(kSod_20211108)), Date(20211108).to_epoch_timestamp_ns());
    EXPECT_EQ(static_cast<uint64_t>(ns(kSod_20200229)), Date(20200229).to_epoch_timestamp_ns());
    EXPECT_EQ(static_cast<uint64_t>(ns(kSod_21000301)), Date(21000301).to_epoch_timestamp_ns());
}

TEST_F(TestDate, ToStringAndStreamFormat) {
    // Format matches "{}-{:02d}-{:02d}"
    std::ostringstream os;
    os << Date(2021, 1, 5);
    EXPECT_EQ("2021-01-05", os.str());
 
    std::ostringstream os2;
    os2 << Date(2021, 11, 28);
    EXPECT_EQ("2021-11-28", os2.str());
 
    EXPECT_EQ("2020-02-29", Date(20200229).to_string());
    EXPECT_EQ("2021-01-05", Date(2021, 1, 5).to_string());
}
 
TEST_F(TestDate, UtcTodaySanity) {
    Date today = Date::utc_today();
    EXPECT_GE(today.year, 2024);
    EXPECT_GE(today.month, 1);
    EXPECT_LE(today.month, 12);
    EXPECT_GE(today.day, 1);
    EXPECT_LE(today.day, 31);
}

// =====================================================================
// TradingDay
// =====================================================================
class TestTradingDay : public ::testing::Test {
public:
    static void SetUpTestSuite() { RdtscClock::calibrate(); }
};

TEST_F(TestTradingDay, DefaultConstructedIsZero) {
    TradingDay td;
    EXPECT_EQ(0, td.get_curr_sod_ns());
    EXPECT_EQ(0, td.get_next_sod_ns());
    EXPECT_EQ(0, td.get_prev_sod_ns());
    EXPECT_EQ(0, td.get_curr_date().to_int_date());
    EXPECT_EQ(0, td.get_next_date().to_int_date());
    EXPECT_EQ(0, td.get_prev_date().to_int_date());
}

TEST_F(TestTradingDay, ConstructorInitializes) {
    TradingDay td(ns(1636354109));
    EXPECT_EQ(20211108, td.get_curr_date().to_int_date());
    EXPECT_EQ(20211109, td.get_next_date().to_int_date());
    EXPECT_EQ(20211107, td.get_prev_date().to_int_date());
    EXPECT_EQ(ns(kSod_20211108), td.get_curr_sod_ns());
}

TEST_F(TestTradingDay, ConstructorMatchesInit) {
    TradingDay a(ns(1636354109));
    TradingDay b;
    ASSERT_TRUE(b.init(ns(1636354109)));
    EXPECT_EQ(a.get_curr_sod_ns(), b.get_curr_sod_ns());
    EXPECT_EQ(a.get_next_sod_ns(), b.get_next_sod_ns());
    EXPECT_EQ(a.get_prev_sod_ns(), b.get_prev_sod_ns());
    EXPECT_EQ(a.get_curr_date().to_int_date(), b.get_curr_date().to_int_date());
}

TEST_F(TestTradingDay, ReInitOverwritesState) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(1636354109)));
    expect_consistent(td, 20211108);
    ASSERT_TRUE(td.init(ns(kSod_20200229 + 100)));
    expect_consistent(td, 20200229);
}

// ---- Boundaries within a day ----------------------------------------
TEST_F(TestTradingDay, StartOfDayBoundary) {
    // exactly 00:00:00 UTC belongs to the new day
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20211108)));
    expect_consistent(td, 20211108);
    EXPECT_EQ(ns(kSod_20211108), td.get_curr_sod_ns());
}
 
TEST_F(TestTradingDay, OneNanosecondBeforeMidnightIsPreviousDay) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20211108) - 1));
    expect_consistent(td, 20211107);
}
 
TEST_F(TestTradingDay, LastNanosecondOfDay) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20211108) + quark::kNsPerDay - 1));
    expect_consistent(td, 20211108);
}
 
TEST_F(TestTradingDay, FirstNanosecondOfNextDay) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20211108) + quark::kNsPerDay));
    expect_consistent(td, 20211109);
}
 
TEST_F(TestTradingDay, MidDayTimestamps) {
    for (int64_t hour : {0, 1, 6, 12, 18, 23}) {
        TradingDay td;
        ASSERT_TRUE(td.init(ns(kSod_20211108 + hour * 3600 + 59 * 60 + 59)));
        expect_consistent(td, 20211108);
    }
}

// ---- Calendar boundaries --------------------------------------------
TEST_F(TestTradingDay, YearEnd) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(1640995199)));  // 2021-12-31 23:59:59
    expect_consistent(td, 20211231);
    EXPECT_EQ(20220101, td.get_next_date().to_int_date());
    EXPECT_EQ(20211230, td.get_prev_date().to_int_date());
    EXPECT_EQ(ns(kSod_20220101), td.get_next_sod_ns());
}
 
TEST_F(TestTradingDay, YearStart) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20220101)));
    expect_consistent(td, 20220101);
    EXPECT_EQ(20211231, td.get_prev_date().to_int_date());
    EXPECT_EQ(ns(kSod_20211231), td.get_prev_sod_ns());
}
 
TEST_F(TestTradingDay, NonLeapFebruaryToMarch) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20210301 + 10)));
    expect_consistent(td, 20210301);
    EXPECT_EQ(20210228, td.get_prev_date().to_int_date());
}
 
TEST_F(TestTradingDay, LeapFebruaryToMarch) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20200301 + 10)));
    expect_consistent(td, 20200301);
    EXPECT_EQ(20200229, td.get_prev_date().to_int_date());
}
 
TEST_F(TestTradingDay, LeapDayMiddle) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20200229 + 12 * 3600)));
    expect_consistent(td, 20200229);
    EXPECT_EQ(20200228, td.get_prev_date().to_int_date());
    EXPECT_EQ(20200301, td.get_next_date().to_int_date());
}
 
TEST_F(TestTradingDay, Century2000IsLeap) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_20000229 + 1)));
    expect_consistent(td, 20000229);
}
 
TEST_F(TestTradingDay, Century2100IsNotLeap) {
    TradingDay td;
    ASSERT_TRUE(td.init(ns(kSod_21000228 + 1)));
    expect_consistent(td, 21000228);
    EXPECT_EQ(21000301, td.get_next_date().to_int_date());  // no Feb 29 in 2100
    EXPECT_EQ(ns(kSod_21000301), td.get_next_sod_ns());
}
 
TEST_F(TestTradingDay, PastInt32EpochRollover) {
    // 2038-01-19 03:14:08 UTC == 2^31 seconds, catches 32-bit truncation bugs
    TradingDay td;
    ASSERT_TRUE(td.init(ns(2147483648LL)));
    expect_consistent(td, 20380119);
}
 
TEST_F(TestTradingDay, UnixEpoch) {
    TradingDay td;
    ASSERT_TRUE(td.init(0));
    EXPECT_EQ(19700101, td.get_curr_date().to_int_date());
    EXPECT_EQ(19700102, td.get_next_date().to_int_date());
    EXPECT_EQ(0, td.get_curr_sod_ns());
}


// ---- check_and_update -----------------------------------------------
TEST_F(TestTradingDay, CheckAndUpdateSameDayNoChange) {
    TradingDay td(ns(kSod_20211108 + 100));
    td.check_and_update(ns(kSod_20211108 + 200));
    expect_consistent(td, 20211108);
    td.check_and_update(ns(kSod_20211108) + quark::kNsPerDay - 1);
    expect_consistent(td, 20211108);
}
 
TEST_F(TestTradingDay, CheckAndUpdateExactlyAtNextSod) {
    TradingDay td(ns(kSod_20211108 + 100));
    td.check_and_update(td.get_next_sod_ns());
    expect_consistent(td, 20211109);
}
 
TEST_F(TestTradingDay, CheckAndUpdateNextDay) {
    TradingDay td(ns(kSod_20211108 + 100));
    td.check_and_update(ns(kSod_20211108 + kSecondsPerDay + 5));
    expect_consistent(td, 20211109);
}
 
TEST_F(TestTradingDay, CheckAndUpdateJumpsMultipleDays) {
    TradingDay td(ns(kSod_20211108 + 100));
    td.check_and_update(ns(kSod_20211108 + 10 * kSecondsPerDay + 5));
    expect_consistent(td, 20211118);
}
 
TEST_F(TestTradingDay, CheckAndUpdateAcrossYear) {
    TradingDay td(ns(kSod_20211231 + 100));
    td.check_and_update(ns(kSod_20220101 + 1));
    expect_consistent(td, 20220101);
}
 
TEST_F(TestTradingDay, CheckAndUpdateDoesNotMoveBackwards) {
    // Only rolls forward: an older timestamp must not change the state.
    TradingDay td(ns(kSod_20211108 + 100));
    td.check_and_update(ns(kSod_20211108 - 5));
    expect_consistent(td, 20211108);
}
 
TEST_F(TestTradingDay, CheckAndUpdateRepeatedDaily) {
    TradingDay td(ns(kSod_20211231 + 1));
    int expected[] = {20211231, 20220101, 20220102, 20220103};
    for (int i = 0; i < 4; ++i) {
        td.check_and_update(ns(kSod_20211231 + i * kSecondsPerDay + 1));
        expect_consistent(td, expected[i]);
    }
}

// ---- operator+= / operator-= ----------------------------------------
TEST_F(TestTradingDay, PlusEqualsOne) {
    TradingDay td(ns(kSod_20211108 + 100));
    td += 1;
    expect_consistent(td, 20211109);
}
 
TEST_F(TestTradingDay, MinusEqualsOne) {
    TradingDay td(ns(kSod_20211108 + 100));
    td -= 1;
    expect_consistent(td, 20211107);
}
 
TEST_F(TestTradingDay, PlusMinusZeroIsNoOp) {
    TradingDay td(ns(kSod_20211108 + 100));
    td += 0;
    expect_consistent(td, 20211108);
    td -= 0;
    expect_consistent(td, 20211108);
}
 
TEST_F(TestTradingDay, PlusThenMinusRestores) {
    TradingDay td(ns(kSod_20211108 + 100));
    td += 7;
    expect_consistent(td, 20211115);
    td -= 7;
    expect_consistent(td, 20211108);
}
 
TEST_F(TestTradingDay, NegativeGapEquivalence) {
    TradingDay a(ns(kSod_20211108 + 100));
    TradingDay b(ns(kSod_20211108 + 100));
    a += -3;
    b -= 3;
    EXPECT_EQ(b.get_curr_sod_ns(), a.get_curr_sod_ns());
    EXPECT_EQ(b.get_curr_date().to_int_date(), a.get_curr_date().to_int_date());
    expect_consistent(a, 20211105);
}
 
TEST_F(TestTradingDay, MinusEqualsAcrossMonthBoundary) {
    TradingDay td(ns(kSod_20211108 + 100));
    td -= 8;
    expect_consistent(td, 20211031);
    td -= 1;
    expect_consistent(td, 20211030);
}
 
TEST_F(TestTradingDay, MinusEqualsAcrossYearBoundary) {
    TradingDay td(ns(kSod_20220101 + 100));
    td -= 1;
    expect_consistent(td, 20211231);
    td -= 365;
    expect_consistent(td, 20201231);
}
 
TEST_F(TestTradingDay, PlusEqualsAcrossLeapDay) {
    TradingDay td(ns(kSod_20200229 - kSecondsPerDay + 100));  // 2020-02-28
    td += 1;
    expect_consistent(td, 20200229);
    td += 1;
    expect_consistent(td, 20200301);
}
 
TEST_F(TestTradingDay, PlusEqualsLargeGap) {
    TradingDay td(ns(kSod_20211108 + 100));
    td += 1000;
    int64_t expected_sod = ns(kSod_20211108 + 1000 * kSecondsPerDay);
    EXPECT_EQ(expected_sod, td.get_curr_sod_ns());
    expect_consistent(td, td.get_curr_date().to_int_date());
    EXPECT_EQ(TradingDay::get_date(expected_sod).to_int_date(), td.get_curr_date().to_int_date());
}
 
TEST_F(TestTradingDay, OperatorsReturnSelf) {
    TradingDay td(ns(kSod_20211108 + 100));
    TradingDay& r1 = (td += 1);
    EXPECT_EQ(&td, &r1);
    TradingDay& r2 = (td -= 1);
    EXPECT_EQ(&td, &r2);
    // chaining
    (td += 2) -= 1;
    expect_consistent(td, 20211109);
}

// ---- static helpers ---------------------------------------------------
TEST_F(TestTradingDay, GetDateStatic) {
    EXPECT_EQ(19700101, TradingDay::get_date(0).to_int_date());
    EXPECT_EQ(19700101, TradingDay::get_date(ns(86399)).to_int_date());
    EXPECT_EQ(19700102, TradingDay::get_date(ns(86400)).to_int_date());
    EXPECT_EQ(20211108, TradingDay::get_date(ns(1636354109)).to_int_date());
    EXPECT_EQ(20200229, TradingDay::get_date(ns(kSod_20200229 + 1)).to_int_date());
    EXPECT_EQ(20211231, TradingDay::get_date(ns(1640995199)).to_int_date());
    EXPECT_EQ(20220101, TradingDay::get_date(ns(kSod_20220101)).to_int_date());
    EXPECT_EQ(21000301, TradingDay::get_date(ns(kSod_21000301)).to_int_date());
    EXPECT_EQ(20380119, TradingDay::get_date(ns(2147483648LL)).to_int_date());
}
 
TEST_F(TestTradingDay, ToUnixTimeNsStatic) {
    EXPECT_EQ(0, TradingDay::to_unix_time_ns(19700101));
    EXPECT_EQ(ns(kSod_20211108), TradingDay::to_unix_time_ns(20211108));
    EXPECT_EQ(ns(kSod_20200229), TradingDay::to_unix_time_ns(20200229));
    EXPECT_EQ(ns(kSod_20220101), TradingDay::to_unix_time_ns(20220101));
    EXPECT_EQ(ns(kSod_21000301), TradingDay::to_unix_time_ns(21000301));
}

TEST_F(TestTradingDay, GetDateAndToUnixTimeRoundTrip) {
    for (int v : {19700101, 19991231, 20000229, 20200229, 20210301, 20211231, 20220101, 21000228, 21000301}) {
        EXPECT_EQ(v, TradingDay::get_date(TradingDay::to_unix_time_ns(v)).to_int_date()) << v;
    }
}

// ---- Property / sweep tests -------------------------------------------
TEST_F(TestTradingDay, SweepEveryDayOverSeveralYears) {
    // Walk ~6 years of days (covers 2020 and 2024 leap years and 2021-2023 non-leap),
    // checking init() at several offsets within each day.
    const int64_t start = kSod_20200229 - 60 * kSecondsPerDay;
    const int64_t days = 6 * 366;
    for (int64_t d = 0; d < days; ++d) {
        const int64_t sod = start + d * kSecondsPerDay;
        for (int64_t off : {0LL, 1LL, 29LL, 30LL, 31LL, 43200LL, kSecondsPerDay - 1}) {
            TradingDay td;
            ASSERT_TRUE(td.init(ns(sod + off))) << "sod=" << sod << " off=" << off;
            ASSERT_EQ(ns(sod), td.get_curr_sod_ns()) << "sod=" << sod << " off=" << off;
            ASSERT_EQ(ns(sod) + quark::kNsPerDay, td.get_next_sod_ns());
            ASSERT_EQ(ns(sod) - quark::kNsPerDay, td.get_prev_sod_ns());
            ASSERT_EQ(TradingDay::get_date(ns(sod)).to_int_date(), td.get_curr_date().to_int_date());
            ASSERT_EQ(TradingDay::get_date(ns(sod) + quark::kNsPerDay).to_int_date(), td.get_next_date().to_int_date());
            ASSERT_EQ(TradingDay::get_date(ns(sod) - quark::kNsPerDay).to_int_date(), td.get_prev_date().to_int_date());
        }
    }
}
 
TEST_F(TestTradingDay, SweepPlusMinusRoundTrip) {
    for (int gap : {1, 2, 7, 28, 29, 30, 31, 59, 60, 365, 366, 1461}) {
        TradingDay td(ns(kSod_20200229 + 12345));
        td += gap;
        td -= gap;
        expect_consistent(td, 20200229);
    }
}
 
TEST_F(TestTradingDay, IncrementalPlusMatchesDirectInit) {
    TradingDay walker(ns(kSod_20211231 + 1));
    for (int i = 1; i <= 800; ++i) {
        walker += 1;
        TradingDay direct(ns(kSod_20211231 + 1 + static_cast<int64_t>(i) * kSecondsPerDay));
        ASSERT_EQ(direct.get_curr_sod_ns(), walker.get_curr_sod_ns()) << i;
        ASSERT_EQ(direct.get_curr_date().to_int_date(), walker.get_curr_date().to_int_date()) << i;
        ASSERT_EQ(direct.get_next_date().to_int_date(), walker.get_next_date().to_int_date()) << i;
        ASSERT_EQ(direct.get_prev_date().to_int_date(), walker.get_prev_date().to_int_date()) << i;
    }
}
 
TEST_F(TestTradingDay, StructLayout) {
    EXPECT_EQ(sizeof(Date), 4u);
    EXPECT_EQ(sizeof(TradingDay), 40u);
}

// ---- init() contract: negative timestamps are rejected, 0 is valid ------
TEST_F(TestTradingDay, InitRejectsNegativeTimestamp) {
    TradingDay td;
    EXPECT_FALSE(td.init(-1));
    EXPECT_FALSE(td.init(std::numeric_limits<int64_t>::min()));
    EXPECT_TRUE(td.init(0));
}
 
TEST_F(TestTradingDay, FailedInitLeavesStateUnchanged) {
    TradingDay td(ns(kSod_20211108 + 100));
    EXPECT_FALSE(td.init(-5));
    expect_consistent(td, 20211108);
}
 
TEST_F(TestTradingDay, FailedConstructorInitLeavesDefaultState) {
    TradingDay td(-1);
    EXPECT_EQ(0, td.get_curr_sod_ns());
    EXPECT_EQ(0, td.get_curr_date().to_int_date());
}

// ---- Pre-1970 handling (reachable via -= and the static helpers) --------
TEST_F(TestTradingDay, GetDateNegativeNanosUsesFloor) {
    EXPECT_EQ(19691231, TradingDay::get_date(-1).to_int_date());
    EXPECT_EQ(19691231, TradingDay::get_date(-quark::kNsPerDay).to_int_date());
    EXPECT_EQ(19691230, TradingDay::get_date(-quark::kNsPerDay - 1).to_int_date());
}
 
TEST_F(TestTradingDay, ToUnixTimeNsBeforeEpoch) {
    EXPECT_EQ(-quark::kNsPerDay, TradingDay::to_unix_time_ns(19691231));
    EXPECT_EQ(-2 * quark::kNsPerDay, TradingDay::to_unix_time_ns(19691230));
}
 
TEST_F(TestTradingDay, MinusEqualsBelowEpoch) {
    TradingDay td(ns(kSecondsPerDay + 5));  // 1970-01-02
    td -= 3;
    expect_consistent(td, 19691230);
    EXPECT_LT(td.get_curr_sod_ns(), 0);
}
 
TEST_F(TestTradingDay, PrevSodAtEpochIsNegative) {
    TradingDay td;
    ASSERT_TRUE(td.init(0));
    EXPECT_EQ(-quark::kNsPerDay, td.get_prev_sod_ns());
    EXPECT_EQ(19691231, td.get_prev_date().to_int_date());
}

TEST_F(TestTradingDay, TestUTCOriginal) {
    {
        TradingDay td;
        EXPECT_TRUE(td.init(static_cast<int64_t>(1636354109) * 1000 * 1000 * 1000));
        EXPECT_EQ(20211108, td.get_curr_date().to_int_date());
        EXPECT_EQ(20211109, td.get_next_date().to_int_date());
        EXPECT_EQ(20211107, td.get_prev_date().to_int_date());
        EXPECT_EQ(static_cast<int64_t>(1636329600) * 1000 * 1000 * 1000, td.get_curr_sod_ns());
        EXPECT_EQ(static_cast<int64_t>(1636329600 + 24 * 60 * 60) * 1000 * 1000 * 1000, td.get_next_sod_ns());
        EXPECT_EQ(static_cast<int64_t>(1636329600 - 24 * 60 * 60) * 1000 * 1000 * 1000, td.get_prev_sod_ns());
    }

    {
        // test operator -=
        TradingDay td;
        EXPECT_TRUE(td.init(static_cast<int64_t>(1582934401) * 1000 * 1000 * 1000));
        td -= 3;
        EXPECT_EQ(20200226, td.get_curr_date().to_int_date());
        EXPECT_EQ(20200227, td.get_next_date().to_int_date());
        EXPECT_EQ(20200225, td.get_prev_date().to_int_date());
    }

    {
        // test leap year
        TradingDay td;
        EXPECT_TRUE(td.init(static_cast<int64_t>(1582934401) * 1000 * 1000 * 1000));
        EXPECT_EQ(20200229, td.get_curr_date().to_int_date());
        EXPECT_EQ(20200301, td.get_next_date().to_int_date());
        EXPECT_EQ(static_cast<int64_t>(1582934400) * 1000 * 1000 * 1000, td.get_curr_sod_ns());
        EXPECT_EQ(static_cast<int64_t>(1582934400 + 24 * 60 * 60) * 1000 * 1000 * 1000, td.get_next_sod_ns());
    }
}
