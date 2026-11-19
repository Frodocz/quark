// ScaleInt<SCALE, int128_t>, exact parsing (from_chars / parse) for both representations, mul() / div(),
// try_rescale(). Extremes are taken from the live instrument lists of Binance, Bybit, OKX and Bitget.

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>

#include <gtest/gtest.h>

#include "quark/math/ScaleInt.h"

using namespace quark::math;

using Px = ScaleInt<13, int128_t>;
using Qty = ScaleInt<9, int128_t>;
using Notional = ScaleInt<9, int128_t>;
using E2 = ScaleInt<2, int128_t>;
using E9 = ScaleInt<9, int128_t>;
using E13 = ScaleInt<13, int128_t>;
using E18 = ScaleInt<18, int128_t>;

namespace {

template <class T>
std::string str(T v) {
    return v.to_string().c_str();
}

template <class T>
T must_parse(std::string_view text) {
    const auto v = T::parse(text);
    EXPECT_TRUE(v.has_value()) << text;
    return v.value_or(T{});
}

constexpr int128_t kI128Max = std::numeric_limits<int128_t>::max();
constexpr int128_t kI128Min = std::numeric_limits<int128_t>::min();

}  // namespace

// ---- compile time ------------------------------------------------------------------------------

static_assert(Px::MAX_SCALE == 18 && DoubleE8::MAX_SCALE == 13);
static_assert(Px::SCALE_AS_INT == ipow10<int128_t>(13));
static_assert(DoubleE13::MAX_CHARS == 1 + 19 + 1 + 13);  // unchanged for int64
static_assert(Px::MAX_CHARS == 1 + 39 + 1 + 13);
static_assert(Px::parse("0.0000000000001")->get_raw() == 1);
static_assert(DoubleE8::parse("-12.5")->get_raw() == -1'250'000'000);
static_assert(!DoubleE8::parse("0.000000001").has_value());  // 9 fraction digits at scale 8

// ---- parsing, both representations -------------------------------------------------------------

template <class T>
class ParseTest : public ::testing::Test {};
using ParseTypes = ::testing::Types<ScaleInt<8>, ScaleInt<8, int128_t>>;
TYPED_TEST_SUITE(ParseTest, ParseTypes);

TYPED_TEST(ParseTest, PlainDecimals) {
    using T = TypeParam;
    EXPECT_EQ(must_parse<T>("0").get_raw(), 0);
    EXPECT_EQ(must_parse<T>("1").get_raw(), 100'000'000);
    EXPECT_EQ(must_parse<T>("-1").get_raw(), -100'000'000);
    EXPECT_EQ(must_parse<T>("+1").get_raw(), 100'000'000);
    EXPECT_EQ(must_parse<T>("65432.10").get_raw(), 6'543'210'000'000);
    EXPECT_EQ(must_parse<T>("0.00012340").get_raw(), 12'340);
    EXPECT_EQ(must_parse<T>("0.00000001").get_raw(), 1);
    EXPECT_EQ(must_parse<T>("000123.4500").get_raw(), 12'345'000'000);
    EXPECT_EQ(must_parse<T>(".5").get_raw(), 50'000'000);
    EXPECT_EQ(must_parse<T>("5.").get_raw(), 500'000'000);
    EXPECT_EQ(must_parse<T>("-0.0").get_raw(), 0);
}

TYPED_TEST(ParseTest, TrailingZerosBeyondTheScaleAreFine) {
    using T = TypeParam;
    EXPECT_EQ(must_parse<T>("1.12345678000000000000000000").get_raw(), 112'345'678);
    EXPECT_EQ(must_parse<T>("0.00000000000").get_raw(), 0);
}

TYPED_TEST(ParseTest, DigitsBeyondTheScaleAreRejectedNotRounded) {
    using T = TypeParam;
    T out = T::from_raw(42);
    const std::string_view text = "0.123456789";
    const auto [ptr, ec] = T::from_chars(text.data(), text.data() + text.size(), out);
    EXPECT_EQ(ec, std::errc::result_out_of_range);
    EXPECT_EQ(ptr, text.data() + text.size());  // the number was consumed
    EXPECT_EQ(out.get_raw(), 42);               // untouched
    EXPECT_FALSE(T::parse("1.000000001").has_value());
    EXPECT_FALSE(T::parse("1e-9").has_value());
}

TYPED_TEST(ParseTest, Exponents) {
    using T = TypeParam;
    EXPECT_EQ(must_parse<T>("1E-8").get_raw(), 1);
    EXPECT_EQ(must_parse<T>("1e-8").get_raw(), 1);
    EXPECT_EQ(must_parse<T>("2.5e3").get_raw(), 250'000'000'000);
    EXPECT_EQ(must_parse<T>("2.5E+3").get_raw(), 250'000'000'000);
    EXPECT_EQ(must_parse<T>("12000e-3").get_raw(), 1'200'000'000);
    EXPECT_EQ(must_parse<T>("0e999999").get_raw(), 0);  // zero with any exponent
    EXPECT_FALSE(T::parse("1e99999").has_value());
    EXPECT_FALSE(T::parse("1e-99999").has_value());
}

TYPED_TEST(ParseTest, ExponentWithoutDigitsIsNotPartOfTheNumber) {
    using T = TypeParam;
    T out;
    const std::string_view text = "1.5e";
    const auto [ptr, ec] = T::from_chars(text.data(), text.data() + text.size(), out);
    EXPECT_EQ(ec, std::errc{});
    EXPECT_EQ(ptr, text.data() + 3);
    EXPECT_EQ(out.get_raw(), 150'000'000);
    EXPECT_FALSE(T::parse("1.5e").has_value());  // parse() wants the whole text
    EXPECT_FALSE(T::parse("1.5e+").has_value());
}

TYPED_TEST(ParseTest, InvalidInput) {
    using T = TypeParam;
    for (const std::string_view text : {"", "-", "+", ".", "-.", "e5", "abc", " 1", "--1", "+-1"}) {
        T out = T::from_raw(7);
        const auto [ptr, ec] = T::from_chars(text.data(), text.data() + text.size(), out);
        EXPECT_EQ(ec, std::errc::invalid_argument) << '"' << text << '"';
        EXPECT_EQ(ptr, text.data()) << '"' << text << '"';
        EXPECT_EQ(out.get_raw(), 7);
    }
    EXPECT_FALSE(T::parse("1 ").has_value());
    EXPECT_FALSE(T::parse("1.2.3").has_value());
    EXPECT_FALSE(T::parse("1,5").has_value());
}

TYPED_TEST(ParseTest, StopsAtTheFirstNonNumberCharacter) {
    using T = TypeParam;
    T out;
    const std::string_view text = "123.45\",\"q\":\"1\"";  // a JSON value followed by more JSON
    const auto [ptr, ec] = T::from_chars(text.data(), text.data() + text.size(), out);
    EXPECT_EQ(ec, std::errc{});
    EXPECT_EQ(*ptr, '"');
    EXPECT_EQ(out.get_raw(), 12'345'000'000);
}

TEST(Parse, Int64Limits) {
    using T = ScaleInt<1>;
    EXPECT_EQ(must_parse<T>("922337203685477580.7").get_raw(), std::numeric_limits<int64_t>::max());
    EXPECT_EQ(must_parse<T>("-922337203685477580.8").get_raw(), std::numeric_limits<int64_t>::min());
    EXPECT_FALSE(T::parse("922337203685477580.8").has_value());
    EXPECT_FALSE(T::parse("-922337203685477580.9").has_value());
    EXPECT_FALSE(T::parse("99999999999999999999999999").has_value());
}

TEST(Parse, Int128Limits) {
    using T = ScaleInt<1, int128_t>;
    // 2^127 - 1 = 170141183460469231731687303715884105727
    EXPECT_EQ(must_parse<T>("17014118346046923173168730371588410572.7").get_raw(), kI128Max);
    EXPECT_EQ(must_parse<T>("-17014118346046923173168730371588410572.8").get_raw(), kI128Min);
    EXPECT_FALSE(T::parse("17014118346046923173168730371588410572.8").has_value());
    EXPECT_FALSE(T::parse("1000000000000000000000000000000000000000").has_value());
}

// ---- live venue extremes -----------------------------------------------------------------------

TEST(VenueExtremes, PricesFitPx) {
    EXPECT_EQ(must_parse<Px>("0.0000000000001").get_raw(), 1);         // BABYDOGEUSDT tick, Bybit / Bitget
    EXPECT_EQ(str(must_parse<Px>("2.943E-10")), "0.0000000002943");     // BABYDOGE price
    EXPECT_EQ(str(must_parse<Px>("1341412464.00")), "1341412464");      // Binance BTCIDR
    EXPECT_EQ(str(must_parse<Px>("99822596.00000000")), "99822596");    // Binance BTCNGN
    EXPECT_FALSE(Px::parse("0.00000000000001").has_value());            // a 1e-14 tick would be refused
}

TEST(VenueExtremes, QuantitiesFitQty) {
    EXPECT_EQ(must_parse<Qty>("0.000000001").get_raw(), 1);              // OKX BTC-TRY lot
    EXPECT_EQ(str(must_parse<Qty>("59628000000000.00")), "59628000000000");  // Bitget BABYDOGE book level
    EXPECT_EQ(str(must_parse<Qty>("1680200000000")), "1680200000000");   // Binance BTTCUSDT book level
    EXPECT_EQ(str(must_parse<Qty>("244190000000.00")), "244190000000");  // Binance PEPEUSDT book level
}

TEST(VenueExtremes, NotionalOfTheExtremes) {
    Notional n;
    ASSERT_TRUE(mul(must_parse<Px>("1341412464"), must_parse<Qty>("1000"), n));
    EXPECT_EQ(str(n), "1341412464000");
    ASSERT_TRUE(mul(must_parse<Px>("0.0000000002943"), must_parse<Qty>("60000000000000"), n));
    EXPECT_EQ(str(n), "17658");
    ASSERT_TRUE(mul(must_parse<Px>("0.0000000000001"), must_parse<Qty>("0.000000001"), n, Rounding::HalfAway));
    EXPECT_EQ(n.get_raw(), 0);  // 1e-22 rounds to 0 at scale 9
    ASSERT_TRUE(mul(must_parse<Px>("0.0000000000001"), must_parse<Qty>("0.000000001"), n, Rounding::Ceil));
    EXPECT_EQ(n.get_raw(), 1);
}

// ---- formatting --------------------------------------------------------------------------------

TEST(Format128, LimitsAndRoundTrip) {
    EXPECT_EQ(str(ScaleInt<1, int128_t>::from_raw(kI128Max)), "17014118346046923173168730371588410572.7");
    EXPECT_EQ(str(ScaleInt<1, int128_t>::from_raw(kI128Min)), "-17014118346046923173168730371588410572.8");
    EXPECT_EQ(str(E18::from_raw(kI128Min)), "-170141183460469231731.687303715884105728");
    EXPECT_EQ(str(Px::from_raw(0)), "0");
    EXPECT_EQ(str(Px::from_raw(-1)), "-0.0000000000001");
    char buf[64];
    const char* end = E18::from_raw(kI128Min).to_chars(buf);
    EXPECT_LE(static_cast<size_t>(end - buf), E18::MAX_CHARS);
}

TEST(Format128, ToCharsUint128) {
    char buf[64];
    auto put = [&](uint128_t v) { return std::string(buf, to_chars(buf, v)); };
    EXPECT_EQ(put(0), "0");
    EXPECT_EQ(put(UINT64_MAX), "18446744073709551615");
    EXPECT_EQ(put(static_cast<uint128_t>(UINT64_MAX) + 1), "18446744073709551616");
    EXPECT_EQ(put(static_cast<uint128_t>(10'000'000'000'000'000'000ULL) * 10'000'000'000'000'000'000ULL),
              "100000000000000000000000000000000000000");  // 10^38, inner zeros padded
    EXPECT_EQ(put(~uint128_t{0}), "340282366920938463463374607431768211455");
}

// ---- property: to_chars and parse are inverse ---------------------------------------------------

template <class T>
void check_round_trip(typename T::RepT raw) {
    char buf[64];
    const T v = T::from_raw(raw);
    const std::string_view text(buf, static_cast<size_t>(v.to_chars(buf) - buf));
    const auto back = T::parse(text);
    ASSERT_TRUE(back.has_value()) << text;
    ASSERT_EQ(back->get_raw(), raw) << text;
}

TEST(RoundTrip, RandomValues) {
    std::mt19937_64 rng(20261109);
    for (int i = 0; i < 20000; ++i) {
        const auto a = static_cast<int64_t>(rng());
        const int128_t b = (static_cast<int128_t>(static_cast<int64_t>(rng())) << 64) | static_cast<int128_t>(rng());
        check_round_trip<ScaleInt<1>>(a);
        check_round_trip<ScaleInt<8>>(a);
        check_round_trip<ScaleInt<13>>(a >> (i % 64));
        check_round_trip<Px>(b);
        check_round_trip<Qty>(b >> (i % 128));
        check_round_trip<E18>(b);
    }
    for (const int64_t raw : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), int64_t{0}}) {
        check_round_trip<ScaleInt<8>>(raw);
    }
    for (const int128_t raw : {kI128Min, kI128Max, int128_t{0}, int128_t{-1}}) {
        check_round_trip<Px>(raw);
        check_round_trip<E18>(raw);
    }
}

// ---- arithmetic --------------------------------------------------------------------------------

TEST(Arithmetic128, OperatorsAndTicks) {
    const Px a = must_parse<Px>("100.5");
    const Px b = must_parse<Px>("0.0000000000003");
    EXPECT_EQ(str(a + b), "100.5000000000003");
    EXPECT_EQ(str(a - b), "100.4999999999997");
    EXPECT_EQ(str(-a), "-100.5");
    EXPECT_EQ(str(a * 3), "301.5");
    EXPECT_TRUE(b < a);
    EXPECT_EQ(str((a * 2).abs()), "201");

    using Tick = TickSize<13, int128_t>;
    Tick tick = Tick::from_raw(1);
    ASSERT_TRUE(Tick::try_from_value(must_parse<Px>("0.5"), tick));
    EXPECT_EQ(tick.pow10_exponent(), -1);  // 5 * 10^12: not a power of ten
    Px p = must_parse<Px>("100.7");
    EXPECT_EQ(str(Px(p).floor_to_tick(tick)), "100.5");
    EXPECT_EQ(str(Px(p).ceil_to_tick(tick)), "101");
    EXPECT_EQ(str(Px(p).round_to_tick(tick)), "100.5");

    ASSERT_TRUE(Tick::try_from_value(must_parse<Px>("0.01"), tick));
    EXPECT_EQ(tick.pow10_exponent(), 11);  // compile-time divisor path
    EXPECT_EQ(str(must_parse<Px>("-1.239").floor_to_tick(tick)), "-1.24");
    EXPECT_EQ(str(tick.value()), "0.01");
    using Tick13 = TickSize<13, int128_t>;
    EXPECT_FALSE(Tick13::try_from_value(Px{}, tick));
}

TEST(Arithmetic128, MulRoundingModes) {
    const E2 a = must_parse<E2>("1.25");
    const E2 b = must_parse<E2>("0.5");
    E2 out;
    // exact product 0.625 at scale 2
    ASSERT_TRUE(mul(a, b, out, Rounding::HalfAway));
    EXPECT_EQ(str(out), "0.63");
    ASSERT_TRUE(mul(a, b, out, Rounding::Floor));
    EXPECT_EQ(str(out), "0.62");
    ASSERT_TRUE(mul(a, b, out, Rounding::Ceil));
    EXPECT_EQ(str(out), "0.63");
    ASSERT_TRUE(mul(a, b, out, Rounding::TowardZero));
    EXPECT_EQ(str(out), "0.62");
    ASSERT_TRUE(mul(-a, b, out, Rounding::HalfAway));
    EXPECT_EQ(str(out), "-0.63");
    ASSERT_TRUE(mul(-a, b, out, Rounding::Floor));
    EXPECT_EQ(str(out), "-0.63");
    ASSERT_TRUE(mul(-a, b, out, Rounding::TowardZero));
    EXPECT_EQ(str(out), "-0.62");
    // widening output scale is exact
    ScaleInt<6, int128_t> wide;
    ASSERT_TRUE(mul(a, b, wide));
    EXPECT_EQ(str(wide), "0.625");
}

TEST(Arithmetic128, MulOverflowIsReportedNotWrapped) {
    Notional out = Notional::from_raw(5);
    EXPECT_FALSE(mul(Px::from_raw(kI128Max), Qty::from_raw(2), out));
    EXPECT_EQ(out.get_raw(), 5);  // untouched
    DoubleE8 out64 = DoubleE8::from_raw(5);
    EXPECT_FALSE(mul(DoubleE8::from_raw(std::numeric_limits<int64_t>::max()), DoubleE8::from_raw(2), out64));
    EXPECT_EQ(out64.get_raw(), 5);
}

TEST(Arithmetic128, Div) {
    // average fill price = notional / quantity
    Px avg;
    ASSERT_TRUE(div(must_parse<Notional>("100"), must_parse<Qty>("3"), avg, Rounding::HalfAway));
    EXPECT_EQ(str(avg), "33.3333333333333");
    ASSERT_TRUE(div(must_parse<Notional>("200"), must_parse<Qty>("3"), avg, Rounding::HalfAway));
    EXPECT_EQ(str(avg), "66.6666666666667");
    ASSERT_TRUE(div(must_parse<Notional>("200"), must_parse<Qty>("3"), avg, Rounding::Floor));
    EXPECT_EQ(str(avg), "66.6666666666666");
    ASSERT_TRUE(div(must_parse<Notional>("-200"), must_parse<Qty>("3"), avg, Rounding::Floor));
    EXPECT_EQ(str(avg), "-66.6666666666667");
    ASSERT_TRUE(div(must_parse<Notional>("200"), must_parse<Qty>("-3"), avg, Rounding::Floor));
    EXPECT_EQ(str(avg), "-66.6666666666667");  // a negative divisor rounds the same way
    ASSERT_TRUE(div(must_parse<Notional>("-200"), must_parse<Qty>("-4"), avg));
    EXPECT_EQ(str(avg), "50");

    Px untouched = Px::from_raw(9);
    EXPECT_FALSE(div(must_parse<Notional>("1"), Qty{}, untouched));  // division by zero
    EXPECT_EQ(untouched.get_raw(), 9);

    // the result scale can be below the inputs' (k < 0 branch)
    E2 coarse;
    ASSERT_TRUE(div(must_parse<E9>("10"), must_parse<E13>("4"), coarse));
    EXPECT_EQ(str(coarse), "2.5");
}

TEST(Arithmetic128, MulDivInverse) {
    std::mt19937_64 rng(7);
    for (int i = 0; i < 5000; ++i) {
        const Px px = Px::from_raw(static_cast<int128_t>(rng() % 10'000'000'000'000'000ULL) + 1);  // up to 1e5
        const Qty qty = Qty::from_raw(static_cast<int128_t>(rng() % 1'000'000'000'000'000ULL) + 1);  // up to 1e9
        E18 notional;  // the exact product has scale 22: rounded once here
        ASSERT_TRUE(mul(px, qty, notional, Rounding::HalfAway));
        Px back;
        ASSERT_TRUE(div(notional, qty, back, Rounding::HalfAway));
        // |back - px| <= 1 unit: one rounding in mul (scale 22 -> 18) and one in div
        const int128_t diff = back.get_raw() - px.get_raw();
        ASSERT_LE(diff < 0 ? -diff : diff, 1);
    }
}

TEST(Rescale128, TryRescaleAcrossRepresentations) {
    ScaleInt<8> small;
    ASSERT_TRUE(must_parse<Px>("123.456").try_rescale(small));
    EXPECT_EQ(str(small), "123.456");
    ASSERT_TRUE(must_parse<Px>("0.0000000004").try_rescale(small, Rounding::HalfAway));
    EXPECT_EQ(small.get_raw(), 0);
    ASSERT_TRUE(must_parse<Px>("0.0000000004").try_rescale(small, Rounding::Ceil));
    EXPECT_EQ(small.get_raw(), 1);
    small = ScaleInt<8>::from_raw(3);
    EXPECT_FALSE(must_parse<Px>("1341412464000").try_rescale(small));  // 1.3e12 * 1e8 overflows int64
    EXPECT_EQ(small.get_raw(), 3);

    Px wide;
    ASSERT_TRUE(must_parse<DoubleE8>("-0.00000001").try_rescale(wide));
    EXPECT_EQ(str(wide), "-0.00000001");
    ASSERT_TRUE(DoubleE8::from_raw(std::numeric_limits<int64_t>::min()).try_rescale(wide));
    EXPECT_EQ(wide.get_raw(), static_cast<int128_t>(std::numeric_limits<int64_t>::min()) * 100'000);

    EXPECT_EQ(str(must_parse<Px>("1.23456789").rescale<4>()), "1.2346");  // same Rep, rounds half away
}

TEST(Double128, Conversions) {
    EXPECT_EQ(str(Px(0.5)), "0.5");
    EXPECT_EQ(Px(1e9).get_raw(), static_cast<int128_t>(1'000'000'000) * 10'000'000'000'000);
    Px out;
    EXPECT_TRUE(Px::try_from_double(1e20, out));
    EXPECT_FALSE(Px::try_from_double(1e30, out));  // beyond 2^127 / 1e13
    EXPECT_FALSE(Px::try_from_double(std::numeric_limits<double>::quiet_NaN(), out));
    EXPECT_DOUBLE_EQ(must_parse<Px>("65432.1").as_double(), 65432.1);
}

TEST(Hash128, DistinctValues) {
    std::unordered_set<Px> set;
    for (int i = 0; i < 1000; ++i) {
        set.insert(Px::from_raw(static_cast<int128_t>(i) << 64));
        set.insert(Px::from_raw(i));
    }
    EXPECT_EQ(set.size(), 1999u);  // i = 0 inserted twice
}
