#include <cmath>
#include <compare>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "quark/QuarkLog.h"
#include "quark/utils/RdtscClock.h"
#include "quark/math/ScaleInt.h"
#include "gtest/gtest.h"

using namespace quark;
using namespace quark::math;
class TestScaleInt : public ::testing::Test {};

TEST_F(TestScaleInt, TestE13) {
    using MyDouble = ScaleInt<13>;
    EXPECT_EQ(MyDouble::SCALE_AS_INT, 10000000000000);

    // the smallest representable value must not be swallowed by a zero tolerance
    EXPECT_EQ(MyDouble(1e-13).get_raw(), 1);
    EXPECT_EQ(MyDouble(0.6e-13).get_raw(), 1);
    EXPECT_EQ(MyDouble(0.4e-13).get_raw(), 0);
    EXPECT_EQ(MyDouble(-1e-13).get_raw(), -1);

    EXPECT_EQ(MyDouble(123.456e-13).get_raw(), 123);
    EXPECT_EQ(MyDouble(0.0000000123456).get_raw(), 123456);

    EXPECT_EQ(MyDouble(-123.456e-13).get_raw(), -123);
    EXPECT_EQ(MyDouble(-0.0000000123456).get_raw(), -123456);

    {
        MyDouble x;
        x = 1234.56789;
        EXPECT_DOUBLE_EQ(x.as_double(), 1234.56789);
        EXPECT_EQ(x.get_raw(), 12345678900000000);
    }

    {
        MyDouble x;
        x = 0.0029999999999999996;
        EXPECT_DOUBLE_EQ(x.as_double(), 0.003);
        EXPECT_EQ(x.get_raw(), 30000000000);
    }

    {
        MyDouble x;
        x = -0.0029999999999999996;
        EXPECT_DOUBLE_EQ(x.as_double(), -0.003);
        EXPECT_EQ(x.get_raw(), -30000000000);
    }

    {
        MyDouble x;
        x = 0.0030000000000000002;
        EXPECT_DOUBLE_EQ(x.as_double(), 0.003);
        EXPECT_EQ(x.get_raw(), 30000000000);
    }

    std::vector<std::pair<double, std::string> > test_cases{
        // clang-format off
        {0,                         "0"},
        {0.00001,                   "0.00001"},
        {0.000012,                  "0.000012"},
        {0.0000123,                 "0.0000123"},
        {0.00001234,                "0.00001234"},
        {0.000012345,               "0.000012345"},
        {0.0000123456,              "0.0000123456"},
        {0.00001234567,             "0.00001234567"},
        {0.000012345678,            "0.000012345678"},
        {0.0000123456789,           "0.0000123456789"},
        {0.1234567890123,           "0.1234567890123"},
        {0.000000000009,            "0.000000000009"},
        {0.00000000009,             "0.00000000009"},
        {0.0000000009,              "0.0000000009"},
        {0.000000009,               "0.000000009"},
        {0.00000009,                "0.00000009"},
        {0.0000009,                 "0.0000009"},
        {0.000009,                  "0.000009"},
        {0.00009,                   "0.00009"},
        {0.0009,                    "0.0009"},
        {123,                       "123"},
        {1234.00001,                "1234.00001"},
        {1234.000012,               "1234.000012"},
        {1234.0000123,              "1234.0000123"},
        {1234.00001234,             "1234.00001234"},
        {1234.000012345,            "1234.000012345"},
        {1234.0000123456,           "1234.0000123456"},
        {1234.00001234567,          "1234.00001234567"},
        {1234.000012345678,         "1234.000012345678"},
        {1234.0000012345678,        "1234.0000012345678"},
        {1234.000000000009,         "1234.000000000009"},
        {1234.00000000009,          "1234.00000000009"},
        {1234.0000000009,           "1234.0000000009"},
        {1234.000000009,            "1234.000000009"},
        {1234.00000009,             "1234.00000009"},
        {1234.0000009,              "1234.0000009"},
        {1234.000009,               "1234.000009"},
        {1234.00009,                "1234.00009"},
        {1234.0009,                 "1234.0009"},
        // clang-format on
    };

    for (auto& test_case : test_cases) {
        MyDouble x(test_case.first);
        EXPECT_EQ(std::string(x.to_string().c_str()), test_case.second);
    }

    // make sure the format works
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    RdtscClock::calibrate();

    // test performance
    const auto start = RdtscClock::rdtsc();
    char buf[64];
    for (int i = 0; i < 10; i++) {
        for (const auto& t : test_cases) {
            MyDouble(t.first).to_string(buf);
        }
    }

    const auto end = RdtscClock::rdtsc();
    QUARK_LOG_INFO("latency per num:{}", (end - start) / (test_cases.size() * 10));
}

TEST_F(TestScaleInt, TestArithmeticE9) {
    using MyDouble = ScaleInt<9>;

    EXPECT_EQ(MyDouble::SCALE_AS_DOUBLE, 1'000'000'000);

    EXPECT_EQ(MyDouble(123456.123456789).get_raw(), 123456123456789);

    EXPECT_TRUE(MyDouble(1.12345678) == MyDouble(1.12345678));
    EXPECT_TRUE(MyDouble(1.123456789) != MyDouble(1.12345678));

    EXPECT_TRUE(MyDouble(1.12345679) > MyDouble(1.12345678));
    EXPECT_TRUE(MyDouble(1.12345678) < MyDouble(1.12345679));

    EXPECT_TRUE(MyDouble(1.12345679) >= MyDouble(1.12345678));
    EXPECT_TRUE(MyDouble(1.12345678) <= MyDouble(1.12345679));

    EXPECT_DOUBLE_EQ((MyDouble(1.22334455) - MyDouble(1.12345678)).as_double(), 0.09988777);
    EXPECT_EQ((MyDouble(1.22334455) - MyDouble(1.12345678)).get_raw(), 99887770);

    EXPECT_DOUBLE_EQ((MyDouble(1.22334455) + MyDouble(1.12345678)).as_double(), 2.34680133);
    EXPECT_EQ((MyDouble(1.22334455) + MyDouble(1.12345678)).get_raw(), 2346801330);

    EXPECT_DOUBLE_EQ((MyDouble(-1.22334455) - MyDouble(1.12345678)).as_double(), -2.34680133);
    EXPECT_EQ((MyDouble(-1.22334455) - MyDouble(1.12345678)).get_raw(), -2346801330);
    EXPECT_DOUBLE_EQ((MyDouble(-1.22334455) + MyDouble(1.12345678)).as_double(), -0.09988777);
    EXPECT_EQ((MyDouble(-1.22334455) + MyDouble(1.12345678)).get_raw(), -99887770);

    EXPECT_DOUBLE_EQ(MyDouble(123456.123456789).as_double(), 123456.123456789);
    EXPECT_EQ(MyDouble(123456.123456789).get_raw(), 123456123456789);

    EXPECT_TRUE(MyDouble(1.12345678) == (1.12345678));
    EXPECT_TRUE(MyDouble(1.123456789) != (1.12345678));

    EXPECT_TRUE(MyDouble(1.12345679) > (1.12345678));
    EXPECT_TRUE(MyDouble(1.12345678) < (1.12345679));

    EXPECT_TRUE(MyDouble(1.12345679) >= (1.12345678));
    EXPECT_TRUE(MyDouble(1.12345678) <= (1.12345679));

    EXPECT_DOUBLE_EQ((MyDouble(1.22334455) - (1.12345678)).as_double(), 0.09988777);
    EXPECT_EQ((MyDouble(1.22334455) - (1.12345678)).get_raw(), 99887770);
    EXPECT_DOUBLE_EQ((MyDouble(1.22334455) + (1.12345678)).as_double(), 2.34680133);
    EXPECT_EQ((MyDouble(1.22334455) + (1.12345678)).get_raw(), 2346801330);

    EXPECT_DOUBLE_EQ((MyDouble(-1.22334455) - (1.12345678)).as_double(), -2.34680133);
    EXPECT_EQ((MyDouble(-1.22334455) - (1.12345678)).get_raw(), -2346801330);
    EXPECT_DOUBLE_EQ((MyDouble(-1.22334455) + (1.12345678)).as_double(), -0.09988777);
    EXPECT_EQ((MyDouble(-1.22334455) + (1.12345678)).get_raw(), -99887770);

    {
        MyDouble x(123.123456);
        x += 0.111111;
        EXPECT_DOUBLE_EQ(x.as_double(), 123.234567);
        EXPECT_EQ(x.get_raw(), 123234567000);
    }

    {
        MyDouble x(123.123456);
        x -= 0.111111;
        EXPECT_DOUBLE_EQ(x.as_double(), 123.012345);
        EXPECT_EQ(x.get_raw(), 123012345000);
    }

    {
        MyDouble x(123.123456);
        MyDouble y(0.111111);
        x += y;
        EXPECT_DOUBLE_EQ(x.as_double(), 123.234567);
        EXPECT_EQ(x.get_raw(), 123234567000);
    }

    {
        MyDouble x(123.123456);
        MyDouble y(0.111111);
        x -= y;
        EXPECT_DOUBLE_EQ(x.as_double(), 123.012345);
        EXPECT_EQ(x.get_raw(), 123012345000);
    }

    {
        MyDouble x;
        x = 1234.56789;
        EXPECT_DOUBLE_EQ(x.as_double(), 1234.56789);
        EXPECT_EQ(x.get_raw(), 1234567890000);
    }

    {
        MyDouble x;
        x = 0.0029999999999999996;
        EXPECT_DOUBLE_EQ(x.as_double(), 0.003);
        EXPECT_EQ(x.get_raw(), 3000000);
    }

    {
        MyDouble x;
        x = -0.0029999999999999996;
        EXPECT_DOUBLE_EQ(x.as_double(), -0.003);
        EXPECT_EQ(x.get_raw(), -3000000);
    }

    {
        MyDouble x;
        x = 0.0030000000000000002;
        EXPECT_DOUBLE_EQ(x.as_double(), 0.003);
        EXPECT_EQ(x.get_raw(), 3000000);
    }

    {
        MyDouble x;
        x = -0.0030000000000000002;
        EXPECT_DOUBLE_EQ(x.as_double(), -0.003);
        EXPECT_EQ(x.get_raw(), -3000000);
    }

    {
        MyDouble x(49436.100000000006);
        EXPECT_TRUE(quark::math::equals_rounded(49436.100000000006, x.as_double()));
        EXPECT_TRUE(quark::math::equals(49436.1, x.as_double()));
    }
}

TEST_F(TestScaleInt, TestFormattingE9) {
    using MyDouble = ScaleInt<9>;

    std::vector<std::pair<double, std::string> > testCases{
        // clang-format off
        {0.0,               "0"},
        {0.1,               "0.1"},
        {0.12,              "0.12"},
        {0.123,             "0.123"},
        {0.1234,            "0.1234"},
        {0.12345,           "0.12345"},
        {0.123456,          "0.123456"},
        {0.1234567,         "0.1234567"},
        {0.12345678,        "0.12345678"},
        {0.123456789,       "0.123456789"},
        {0.000000009,       "0.000000009"},
        {0.00000009,        "0.00000009"},
        {0.0000009,         "0.0000009"},
        {0.000009,          "0.000009"},
        {0.00009,           "0.00009"},
        {0.0009,            "0.0009"},
        {0.009,             "0.009"},
        {0.09,              "0.09"},
        {0.9,               "0.9"},
        {1234000.1,         "1234000.1"},
        {1234000.12,        "1234000.12"},
        {1234000.123,       "1234000.123"},
        {1234000.1234,      "1234000.1234"},
        {1234000.12345,     "1234000.12345"},
        {1234000.123456,    "1234000.123456"},
        {1234000.1234567,   "1234000.1234567"},
        {1234000.12345678,  "1234000.12345678"},
        {1234000.123456789, "1234000.123456789"},
        {1234000.000000009, "1234000.000000009"},
        {1234000.00000009,  "1234000.00000009"},
        {1234000.0000009,   "1234000.0000009"},
        {1234000.000009,    "1234000.000009"},
        {1234000.00009,     "1234000.00009"},
        {1234000.0009,      "1234000.0009"},
        {1234000.009,       "1234000.009"},
        {1234000.09,        "1234000.09"},
        {1234000.9,         "1234000.9"},
        {1234567890,        "1234567890"},
        {876543210,         "876543210"},
        {76543210,          "76543210"},
        {6543210,           "6543210"},
        {543210,            "543210"},
        {43210,             "43210"},
        {3210,              "3210"},
        {210,               "210"},
        {10,                "10"},
        {0,                 "0"},

        //negative
        {-0.1,               "-0.1"},
        {-0.12,              "-0.12"},
        {-0.123,             "-0.123"},
        {-0.1234,            "-0.1234"},
        {-0.12345,           "-0.12345"},
        {-0.123456,          "-0.123456"},
        {-0.1234567,         "-0.1234567"},
        {-0.12345678,        "-0.12345678"},
        {-0.123456789,       "-0.123456789"},
        {-0.000000009,       "-0.000000009"},
        {-0.00000009,        "-0.00000009"},
        {-0.0000009,         "-0.0000009"},
        {-0.000009,          "-0.000009"},
        {-0.00009,           "-0.00009"},
        {-0.0009,            "-0.0009"},
        {-0.009,             "-0.009"},
        {-0.09,              "-0.09"},
        {-0.9,               "-0.9"},
        {-1234000.1,         "-1234000.1"},
        {-1234000.12,        "-1234000.12"},
        {-1234000.123,       "-1234000.123"},
        {-1234000.1234,      "-1234000.1234"},
        {-1234000.12345,     "-1234000.12345"},
        {-1234000.123456,    "-1234000.123456"},
        {-1234000.1234567,   "-1234000.1234567"},
        {-1234000.12345678,  "-1234000.12345678"},
        {-1234000.123456789, "-1234000.123456789"},
        {-1234000.000000009, "-1234000.000000009"},
        {-1234000.00000009,  "-1234000.00000009"},
        {-1234000.0000009,   "-1234000.0000009"},
        {-1234000.000009,    "-1234000.000009"},
        {-1234000.00009,     "-1234000.00009"},
        {-1234000.0009,      "-1234000.0009"},
        {-1234000.009,       "-1234000.009"},
        {-1234000.09,        "-1234000.09"},
        {-1234000.9,         "-1234000.9"},
        {-1234567890,        "-1234567890"},
        {-876543210,         "-876543210"},
        {-76543210,          "-76543210"},
        {-6543210,           "-6543210"},
        {-543210,            "-543210"},
        {-43210,             "-43210"},
        {-3210,              "-3210"},
        {-210,               "-210"},
        {-10,                "-10"},
        // clang-format on
    };

    using MyDouble = ScaleInt<9>;
    for (auto& testCase : testCases) {
        MyDouble x(testCase.first);
        EXPECT_EQ(std::string(x.to_string().c_str()), testCase.second);
    }

    // make sure the format works
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    RdtscClock::calibrate();

    // test performance
    const auto start = RdtscClock::rdtsc();
    char buf[64];
    for (int i = 0; i < 10; i++) {
        for (const auto& t : testCases) {
            DoubleE9(t.first).to_string(buf);
        }
    }

    const auto end = RdtscClock::rdtsc();
    QUARK_LOG_INFO("latency per num:{}", (end - start) / (testCases.size() * 10));
}

TEST_F(TestScaleInt, TestRoundingE9) {
    using MyDouble = ScaleInt<9>;
    EXPECT_EQ(std::string(MyDouble(1.123456).round_to_tick(0.1).to_string().c_str()), "1.1");
    EXPECT_EQ(std::string(MyDouble(1.123456).round_to_tick(0.01).to_string().c_str()), "1.12");
    EXPECT_EQ(std::string(MyDouble(1.123456).round_to_tick(0.001).to_string().c_str()), "1.123");
    EXPECT_EQ(std::string(MyDouble(1.123456).round_to_tick(0.0001).to_string().c_str()), "1.1235");
    EXPECT_EQ(std::string(MyDouble(1.123456).round_to_tick(0.0002).to_string().c_str()), "1.1234");

    EXPECT_EQ(std::string(MyDouble(1.123456).round_to_tick(0.000001).to_string().c_str()), "1.123456");
    EXPECT_EQ(std::string(MyDouble(2.9999996).round_to_tick(1).to_string().c_str()), "3");
    EXPECT_EQ(std::string(MyDouble(3.0000002).round_to_tick(1).to_string().c_str()), "3");
    EXPECT_EQ(std::string(MyDouble(2.9999996).round_to_tick(0.001).to_string().c_str()), "3");
    EXPECT_EQ(std::string(MyDouble(3.0000002).round_to_tick(0.001).to_string().c_str()), "3");

    EXPECT_EQ(std::string(MyDouble(0.123456).round_to_tick(0.000001).to_string().c_str()), "0.123456");
    EXPECT_EQ(std::string(MyDouble(0.9999996).round_to_tick(1).to_string().c_str()), "1");
    EXPECT_EQ(std::string(MyDouble(0.0000002).round_to_tick(1).to_string().c_str()), "0");
    EXPECT_EQ(std::string(MyDouble(0.9999996).round_to_tick(0.001).to_string().c_str()), "1");
    EXPECT_EQ(std::string(MyDouble(0.0000002).round_to_tick(0.001).to_string().c_str()), "0");

    EXPECT_EQ(std::string(MyDouble(-0.0001).to_string().c_str()), "-0.0001");
}

TEST_F(TestScaleInt, TsDoubleToString) {
    using MyDouble = ScaleInt<8>;
    std::vector<double> nums = {637348.63, 4059203.34, 3643706.53, 584436.89, 47737.13,
                                48045.75,  20820.67,   38298.41,   48032.07,  99618.21};
    double numInit = 0.0;
    MyDouble num(0);
    for (auto numRaw : nums) {
        num = MyDouble(numInit);
        num += numRaw;
        numInit = num.as_double();
    }

    EXPECT_DOUBLE_EQ(numInit, 9227247.63);
    EXPECT_EQ(std::string(num.to_string().c_str()), "9227247.63");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.01).c_str()), "4362709580.01");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.39).c_str()), "4362709580.39");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.51).c_str()), "4362709580.51");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.99).c_str()), "4362709580.99");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.001).c_str()), "4362709580.001");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.039).c_str()), "4362709580.039");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.051).c_str()), "4362709580.051");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.099).c_str()), "4362709580.099");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.901).c_str()), "4362709580.901");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.939).c_str()), "4362709580.939");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.951).c_str()), "4362709580.951");
    EXPECT_EQ(std::string(quark::math::double_to_string(4362709580.999).c_str()), "4362709580.999");
}

TEST_F(TestScaleInt, TestRoundingError) {
    EXPECT_TRUE(strcmp("0.0000001", quark::math::double_to_string(0.0000001).c_str()) == 0);
    EXPECT_TRUE(strcmp("0.001", quark::math::double_to_string(0.001).c_str()) == 0);
    EXPECT_TRUE(strcmp("100.001", quark::math::double_to_string(100.001).c_str()) == 0);
    EXPECT_TRUE(strcmp("1000000.001", quark::math::double_to_string(1000000.001).c_str()) == 0);
    EXPECT_TRUE(strcmp("17753820.58", quark::math::double_to_string(17753820.58).c_str()) == 0);
    EXPECT_TRUE(strcmp("10000000000.001", quark::math::double_to_string(10000000000.001).c_str()) == 0);
    EXPECT_TRUE(strcmp("7.5", quark::math::double_to_string(7.5).c_str()) == 0);

    // 15285012 - 0.11 -  15285011.7 - 0.19
    double origQty = 15285012;
    double qty = origQty;
    qty -= 0.11;
    EXPECT_DOUBLE_EQ(round_double(origQty, qty), 15285011.89);
    qty -= 15285011.7;
    EXPECT_DOUBLE_EQ(round_double(origQty, qty), 0.19);
    qty -= 0.19;
    EXPECT_DOUBLE_EQ(round_double(origQty, qty), 0.0);

    EXPECT_TRUE(equals_rounded(9999.9999999999, 10000.0000000001));
    EXPECT_FALSE(equals_rounded(9999.9999, 10000));
}

TEST_F(TestScaleInt, TestEqualsRounded) {
    EXPECT_FALSE(equals_rounded(1.2e-12, 0));
    EXPECT_FALSE(equals_rounded(1.000000000002e6, 1e6));
    EXPECT_TRUE(equals_rounded(1.00000000000002e6, 1e6));
    EXPECT_TRUE(equals(1.2e-12, 0.0, {.rel = 0.0, .abs = 1e-8}));
    EXPECT_TRUE(equals_rounded(948.8000000000001, 948.8));
}

TEST_F(TestScaleInt, TestOverflowCrash) {
    // special floating point values go through std::to_chars
    EXPECT_EQ(std::string(double_to_string(std::numeric_limits<double>::quiet_NaN()).c_str()), "nan");
    EXPECT_EQ(std::string(double_to_string(std::numeric_limits<double>::infinity()).c_str()), "inf");
    EXPECT_EQ(std::string(double_to_string(-std::numeric_limits<double>::infinity()).c_str()), "-inf");

    // values just below and above E13_PIVOT (10)
    EXPECT_EQ(std::string(double_to_string(9.99999999999999).c_str()), "10");
    EXPECT_EQ(std::string(double_to_string(10.00000000000001).c_str()), "10");

    EXPECT_EQ(std::string(double_to_string(std::numeric_limits<double>::max()).c_str()), "1.7976931348623157e+308");
}

// ---- layout and constant evaluation -----------------------------------------------
static_assert(sizeof(DoubleE8) == 8);
static_assert(alignof(DoubleE8) == alignof(int64_t));
static_assert(std::is_trivially_copyable_v<DoubleE8>);
static_assert(std::is_standard_layout_v<DoubleE8>);
static_assert(DoubleE8::SCALE_AS_INT == 100'000'000);
static_assert(DoubleE13::SCALE_AS_INT == 10'000'000'000'000);
static_assert(DoubleE8::get_scale() == 8);
static_assert(DoubleE8(1.5).get_raw() == 150'000'000);
static_assert(DoubleE9(-2.9999996).get_raw() == -2'999'999'600);
static_assert(DoubleE8::from_raw(42).get_raw() == 42);
static_assert((-DoubleE4(1.0)).get_raw() == -10000);
static_assert(DoubleE4::from_raw(-15).rescale<3>().get_raw() == -2);
static_assert(DoubleE4::from_raw(-7).floor_to_tick_raw(5).get_raw() == -10);
static_assert(DoubleE4::from_raw(-7).ceil_to_tick_raw(5).get_raw() == -5);
static_assert(DoubleE8::tick_to_raw(0.01) == 1000000);
static_assert(DoubleE9(-2.9999996).round_to_tick(1).get_raw() == -3'000'000'000);
static_assert(equals(round_double(0.1 + 0.2), 0.3, {.rel = 0.0}));
static_assert(equals_rounded(9.99999999999999, 10.0));

// Exact decimals with |raw| < 1e15 (integer digits + SCALE <= 15) must convert from
// double to exactly the decimal's integer representation.
template <int S>
static void check_round_trip(int64_t max_raw, int n) {
    std::mt19937_64 rng(20261019 + S);
    std::uniform_int_distribution<int64_t> dist(-max_raw, max_raw);
    for (int i = 0; i < n; ++i) {
        const int64_t raw = dist(rng);
        const uint64_t abs_raw = raw < 0 ? 0 - static_cast<uint64_t>(raw) : static_cast<uint64_t>(raw);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%llu.%0*llu", raw < 0 ? "-" : "",
                 static_cast<unsigned long long>(abs_raw / ScaleInt<S>::SCALE_AS_UINT), S,
                 static_cast<unsigned long long>(abs_raw % ScaleInt<S>::SCALE_AS_UINT));
        const double v = strtod(buf, nullptr);
        ASSERT_EQ(ScaleInt<S>(v).get_raw(), raw) << "E" << S << " " << buf;
        // formatting must give back the same decimal (without trailing zeros)
        std::string expected(buf);
        expected.erase(expected.find_last_not_of('0') + 1);
        if (expected.back() == '.') {
            expected.pop_back();
        }
        if (expected == "-0") {
            expected = "0";
        }
        ASSERT_EQ(std::string(ScaleInt<S>(v).to_string().c_str()), expected);
    }
}

template <class F, int... Is>
static void for_each_scale(F&& f, std::integer_sequence<int, Is...>) {
    (f(std::integral_constant<int, Is + 1>{}), ...);
}

// calls f(std::integral_constant<int, S>) for S = 1 .. 13
template <class F>
static void for_each_scale_1_to_13(F&& f) {
    for_each_scale(f, std::make_integer_sequence<int, 13>{});
}

TEST_F(TestScaleInt, TestRoundTripExactDecimal) {
    for_each_scale_1_to_13([](auto sc) {
        constexpr int S = decltype(sc)::value;
        check_round_trip<S>(999'999'999'999'999, 50'000);
        // small magnitudes, where the zero tolerance used to bite
        check_round_trip<S>(1'000, 2'000);
    });
}

TEST_F(TestScaleInt, TestRoundToTickNegative) {
    using MyDouble = ScaleInt<9>;
    EXPECT_EQ(std::string(MyDouble(-1.123456).round_to_tick(0.1).to_string().c_str()), "-1.1");
    EXPECT_EQ(std::string(MyDouble(-1.123456).round_to_tick(0.0001).to_string().c_str()), "-1.1235");
    EXPECT_EQ(std::string(MyDouble(-1.123456).round_to_tick(0.0002).to_string().c_str()), "-1.1234");
    EXPECT_EQ(std::string(MyDouble(-2.9999996).round_to_tick(1).to_string().c_str()), "-3");
    EXPECT_EQ(std::string(MyDouble(-3.0000002).round_to_tick(1).to_string().c_str()), "-3");
    EXPECT_EQ(std::string(MyDouble(-0.9999996).round_to_tick(0.001).to_string().c_str()), "-1");
    EXPECT_EQ(std::string(MyDouble(-0.0000002).round_to_tick(0.001).to_string().c_str()), "0");

    // rounding is symmetric around zero
    for (double x : {0.05, 0.15, 1.25, 2.5, 3.14159, 7.77777, 123.456789}) {
        for (double tick : {0.01, 0.1, 0.5, 1.0}) {
            EXPECT_EQ(MyDouble(-x).round_to_tick(tick).get_raw(), -MyDouble(x).round_to_tick(tick).get_raw())
                << x << " tick " << tick;
        }
    }
}

TEST_F(TestScaleInt, TestRoundToTickHalfway) {
    // exactly half a tick rounds away from zero
    EXPECT_EQ(DoubleE4(0.5).round_to_tick(1).get_raw(), 10'000);
    EXPECT_EQ(DoubleE4(-0.5).round_to_tick(1).get_raw(), -10'000);
    EXPECT_EQ(DoubleE4(2.5).round_to_tick(1).get_raw(), 30'000);
    EXPECT_EQ(DoubleE4(0.4999).round_to_tick(1).get_raw(), 0);
    EXPECT_EQ(DoubleE9(1.15).round_to_tick(0.1).get_raw(), 1'200'000'000);
    EXPECT_EQ(DoubleE9(-1.15).round_to_tick(0.1).get_raw(), -1'200'000'000);
    // odd tick in raw units: 0.0003 at E4 is 3, half tick is 1
    EXPECT_EQ(DoubleE4(0.0001).round_to_tick(0.0003).get_raw(), 0);
    EXPECT_EQ(DoubleE4(0.0002).round_to_tick(0.0003).get_raw(), 3);
    EXPECT_EQ(DoubleE4(0.0005).round_to_tick(0.0003).get_raw(), 6);
}

TEST_F(TestScaleInt, TestRoundToTickInexactTick) {
    // ticks such as 2.01 or 0.57 are not exact doubles: tick * scale must be rounded, not truncated
    for (int k = 1; k < 1000; ++k) {
        const double tick = k / 100.0;
        EXPECT_EQ(DoubleE9(tick).round_to_tick(tick).get_raw(), k * 10'000'000LL) << tick;
        EXPECT_EQ(DoubleE4(tick).round_to_tick(tick).get_raw(), k * 100LL) << tick;
        EXPECT_EQ(DoubleE4(3 * tick).round_to_tick(tick).get_raw(), 3 * k * 100LL) << tick;
    }
    EXPECT_EQ(std::string(DoubleE4(1.0).round_to_tick(0.57).to_string().c_str()), "1.14");
    EXPECT_EQ(std::string(DoubleE9(5.0).round_to_tick(2.01).to_string().c_str()), "4.02");
    EXPECT_DOUBLE_EQ(round_to_tick(5.0, 2.01), 4.02);
}

TEST_F(TestScaleInt, TestRoundToTickInvalidTick) {
    // a tick finer than the resolution (or not positive) leaves the value unchanged, no SIGFPE
    EXPECT_EQ(DoubleE4(1.2345).round_to_tick(1e-5).get_raw(), 12'345);
    EXPECT_EQ(DoubleE4(1.2345).round_to_tick(0).get_raw(), 12'345);
    EXPECT_EQ(DoubleE4(1.2345).round_to_tick(-0.01).get_raw(), 12'345);
    EXPECT_DOUBLE_EQ(round_to_tick(2e9 + 0.25, 1e-5), 2e9 + 0.25);  // E4 band
    EXPECT_DOUBLE_EQ(round_to_tick(2e9 + 0.25, 0.5), 2e9 + 0.5);
}

TEST_F(TestScaleInt, TestFormattingOtherScales) {
    const std::vector<std::pair<double, std::string>> e4_cases{
        {0.0, "0"}, {0.0001, "0.0001"}, {-0.0001, "-0.0001"}, {1.5, "1.5"}, {-1.5, "-1.5"},
        {123456789.1234, "123456789.1234"}, {0.00004, "0"}, {0.00005, "0.0001"}, {10, "10"}, {-1000, "-1000"},
    };
    for (const auto& [v, s] : e4_cases) {
        EXPECT_EQ(std::string(DoubleE4(v).to_string().c_str()), s) << v;
    }

    const std::vector<std::pair<double, std::string>> e6_cases{
        {0.000001, "0.000001"}, {-0.000001, "-0.000001"}, {0.1, "0.1"}, {999999999.999999, "999999999.999999"},
        {-12.3456, "-12.3456"}, {100, "100"},
    };
    for (const auto& [v, s] : e6_cases) {
        EXPECT_EQ(std::string(DoubleE6(v).to_string().c_str()), s) << v;
    }

    // Binance style: 8 decimals
    const std::vector<std::pair<double, std::string>> e8_cases{
        {0.00000001, "0.00000001"}, {-0.00000001, "-0.00000001"}, {65000.1, "65000.1"}, {0.00001, "0.00001"},
        {1234567.12345678, "1234567.12345678"}, {-0.5, "-0.5"}, {0.000000005, "0.00000001"},
    };
    for (const auto& [v, s] : e8_cases) {
        EXPECT_EQ(std::string(DoubleE8(v).to_string().c_str()), s) << v;
    }

    EXPECT_EQ(std::string(ScaleInt<1>(12.3).to_string().c_str()), "12.3");
    EXPECT_EQ(std::string(ScaleInt<1>(-12.34).to_string().c_str()), "-12.3");
    EXPECT_EQ(std::string(ScaleInt<1>(12.35).to_string().c_str()), "12.4");
    EXPECT_EQ(std::string(ScaleInt<12>(0.000000000001).to_string().c_str()), "0.000000000001");
    EXPECT_EQ(std::string(ScaleInt<12>(-123.000000000001).to_string().c_str()), "-123.000000000001");
}

TEST_F(TestScaleInt, TestRawExtremes) {
    EXPECT_EQ(std::string(DoubleE8::from_raw(std::numeric_limits<int64_t>::max()).to_string().c_str()),
              "92233720368.54775807");
    EXPECT_EQ(std::string(DoubleE8::from_raw(std::numeric_limits<int64_t>::min()).to_string().c_str()),
              "-92233720368.54775808");
    EXPECT_EQ(std::string(DoubleE13::from_raw(std::numeric_limits<int64_t>::min()).to_string().c_str()),
              "-922337.2036854775808");
    EXPECT_EQ(std::string(DoubleE13::from_raw(std::numeric_limits<int64_t>::max()).to_string().c_str()),
              "922337.2036854775807");
    EXPECT_EQ(std::string(ScaleInt<1>::from_raw(std::numeric_limits<int64_t>::max()).to_string().c_str()),
              "922337203685477580.7");
    EXPECT_EQ(std::string(ScaleInt<1>::from_raw(std::numeric_limits<int64_t>::min()).to_string().c_str()),
              "-922337203685477580.8");
    EXPECT_EQ(std::string(DoubleE4::from_raw(-1).to_string().c_str()), "-0.0001");

    DoubleE8 x;
    x.set_raw(-5);
    EXPECT_EQ(std::string(x.to_string().c_str()), "-0.00000005");
}

TEST_F(TestScaleInt, TestNegativeZero) {
    EXPECT_EQ(DoubleE8(-0.0).get_raw(), 0);
    EXPECT_EQ(std::string(DoubleE8(-0.0).to_string().c_str()), "0");
    EXPECT_EQ(std::string(DoubleE8(-0.000000001).to_string().c_str()), "0");
    EXPECT_EQ(std::string(double_to_string(-0.0).c_str()), "0");
    EXPECT_TRUE(DoubleE8(-0.0) == DoubleE8(0.0));
    EXPECT_TRUE(equals_rounded(-0.0, 0.0));
}

TEST_F(TestScaleInt, TestCompoundOpsWithNegativeDouble) {
    using MyDouble = ScaleInt<9>;
    {
        MyDouble x(123.123456);
        x += -0.111111;
        EXPECT_EQ(x.get_raw(), 123'012'345'000);
    }
    {
        MyDouble x(123.123456);
        x -= -0.111111;
        EXPECT_EQ(x.get_raw(), 123'234'567'000);
    }
    {
        MyDouble x(0.1);
        x -= 0.3;
        EXPECT_EQ(x.get_raw(), -200'000'000);
        x += 0.2;
        EXPECT_EQ(x.get_raw(), 0);
    }
    {
        // the smallest unit is not dropped by += / -=
        DoubleE13 x;
        x += 1e-13;
        EXPECT_EQ(x.get_raw(), 1);
        x -= 2e-13;
        EXPECT_EQ(x.get_raw(), -1);
    }
    // accumulating 0.1 ten times is exact, unlike double
    MyDouble acc;
    for (int i = 0; i < 10; ++i) {
        acc += 0.1;
    }
    EXPECT_TRUE(acc == MyDouble(1.0));
}

TEST_F(TestScaleInt, TestMixedComparisonWithDouble) {
    using MyDouble = ScaleInt<8>;
    EXPECT_TRUE(MyDouble(-1.5) < -1.4);
    EXPECT_TRUE(MyDouble(-1.5) > -1.6);
    EXPECT_TRUE(MyDouble(-1.5) <= -1.5);
    EXPECT_TRUE(MyDouble(-1.5) >= -1.5);
    EXPECT_TRUE(MyDouble(-1.5) == -1.5);
    EXPECT_TRUE(MyDouble(0.1 + 0.2) == 0.3);
    EXPECT_TRUE((MyDouble(1.0) <=> 1.00000001) == std::strong_ordering::less);
    EXPECT_TRUE((MyDouble(1.0) <=> 1.000000001) == std::strong_ordering::equal);  // below resolution
    // reversed operands use the C++20 rewritten candidates
    EXPECT_TRUE(1.4 > MyDouble(-1.5));
    EXPECT_TRUE(-1.5 == MyDouble(-1.5));
    EXPECT_TRUE(0.3 == MyDouble(0.1 + 0.2));
}

TEST_F(TestScaleInt, TestPivotBoundaries) {
    // equals_rounded must not depend on which side of a pivot each value lands
    EXPECT_TRUE(equals_rounded(9.99999999999999, 10.0));
    EXPECT_TRUE(equals_rounded(10.0, 9.99999999999999));
    EXPECT_TRUE(equals_rounded(999999.9999999999, 1e6));
    EXPECT_TRUE(equals_rounded(1e6, 999999.9999999999));
    EXPECT_TRUE(equals_rounded(999999999.9999999, 1e9));
    EXPECT_FALSE(equals_rounded(9.999999999, 10.0));
    EXPECT_FALSE(equals_rounded(999999.999, 1e6));

    EXPECT_DOUBLE_EQ(round_double(9.99999999999999), 10.0);
    EXPECT_EQ(round_double_get_raw(9.99).second, 13);
    EXPECT_EQ(round_double_get_raw(10.0).second, 9);
    EXPECT_EQ(round_double_get_raw(999999.0).second, 9);
    EXPECT_EQ(round_double_get_raw(1e6).second, 6);
    EXPECT_EQ(round_double_get_raw(1e9).second, 4);

    // round_double(base, x): scale follows base, x crossing a pivot must not change that
    EXPECT_DOUBLE_EQ(round_double(15285012.0, 0.19000000000000128), 0.19);
    EXPECT_DOUBLE_EQ(round_double(5.0, 0.1234567890123), 0.1234567890123);
    EXPECT_DOUBLE_EQ(round_double(5e6, 0.1234567890123), 0.123457);

    // double_to_string: E4 is exact only below 1e11, above that the shortest round trip is used
    EXPECT_EQ(std::string(double_to_string(99999999999.9999).c_str()), "99999999999.9999");
    EXPECT_EQ(std::string(double_to_string(123456789012.3456).c_str()), "123456789012.3456");
    EXPECT_EQ(std::string(double_to_string(459279959318170.19).c_str()), "459279959318170.2");
    EXPECT_EQ(std::string(double_to_string(-459279959318170.19).c_str()), "-459279959318170.2");
}

TEST_F(TestScaleInt, TestNonFiniteAndHugeValues) {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInf = std::numeric_limits<double>::infinity();

    EXPECT_TRUE(std::isnan(round_double(kNaN)));
    EXPECT_EQ(round_double(kInf), kInf);
    EXPECT_EQ(round_double(-kInf), -kInf);
    EXPECT_EQ(round_double(1e300), 1e300);

    EXPECT_TRUE(std::isnan(round_double(1.0, kNaN)));
    EXPECT_EQ(round_double(1.0, kInf), kInf);
    EXPECT_EQ(round_double(1.0, 1e300), 1e300);
    // x much larger than base must not overflow the fine scale chosen by base
    EXPECT_DOUBLE_EQ(round_double(1e-3, 1e6 + 0.1), 1e6 + 0.1);
    EXPECT_DOUBLE_EQ(round_double(kNaN, 1.5), 1.5);

    EXPECT_TRUE(std::isnan(round_to_tick(kNaN, 0.01)));
    EXPECT_EQ(round_to_tick(kInf, 0.01), kInf);
    EXPECT_EQ(round_to_tick(1e300, 0.01), 1e300);

    EXPECT_FALSE(equals_rounded(kNaN, kNaN));
    EXPECT_FALSE(equals_rounded(1.0, kNaN));
    EXPECT_FALSE(equals_rounded(kNaN, 1.0));
    EXPECT_TRUE(equals_rounded(kInf, kInf));
    EXPECT_FALSE(equals_rounded(kInf, -kInf));
    EXPECT_FALSE(equals_rounded(kInf, 1e300));
    EXPECT_TRUE(equals_rounded(1e300, 1e300));
    EXPECT_FALSE(equals_rounded(1e300, std::nextafter(1e300, kInf)));
}

TEST_F(TestScaleInt, TestAbsoluteEpsilonViaEquals) {
    EXPECT_TRUE(equals(1.0, 1.0, {.rel = 0.0, .abs = 0.0}));
    EXPECT_TRUE(equals(1.0, 1.5, {.rel = 0.0, .abs = 0.5}));
    EXPECT_TRUE(equals(1.5, 1.0, {.rel = 0.0, .abs = 0.5}));
    EXPECT_FALSE(equals(1.0, 1.5000001, {.rel = 0.0, .abs = 0.5}));
    EXPECT_FALSE(equals(std::numeric_limits<double>::quiet_NaN(), 1.0, {.rel = 0.0, .abs = 1.0}));
}

TEST_F(TestScaleInt, TestRoundsHalfAwayFromZero) {
    // 0.25, 0.75, 0.125 are exact binary fractions, so these are true ties
    EXPECT_EQ(ScaleInt<1>(0.25).get_raw(), 3);
    EXPECT_EQ(ScaleInt<1>(-0.25).get_raw(), -3);
    EXPECT_EQ(ScaleInt<1>(0.75).get_raw(), 8);
    EXPECT_EQ(ScaleInt<1>(-0.75).get_raw(), -8);
    EXPECT_EQ(ScaleInt<2>(0.125).get_raw(), 13);
    EXPECT_EQ(ScaleInt<2>(-0.125).get_raw(), -13);
    EXPECT_EQ(ScaleInt<1>(0.24).get_raw(), 2);
    EXPECT_EQ(ScaleInt<1>(-0.24).get_raw(), -2);
    // products that land just off the integer
    EXPECT_EQ(ScaleInt<2>(0.07).get_raw(), 7);    // 0.07 * 100 == 7.000000000000001
    EXPECT_EQ(ScaleInt<2>(0.29).get_raw(), 29);   // 0.29 * 100 == 28.999999999999996
}

TEST_F(TestScaleInt, TestTryFromDouble) {
    DoubleE6 x;
    EXPECT_TRUE(DoubleE6::try_from_double(1.5, x));
    EXPECT_EQ(x.get_raw(), 1'500'000);
    EXPECT_FALSE(DoubleE6::try_from_double(std::numeric_limits<double>::quiet_NaN(), x));
    EXPECT_FALSE(DoubleE6::try_from_double(std::numeric_limits<double>::infinity(), x));
    EXPECT_FALSE(DoubleE6::try_from_double(-std::numeric_limits<double>::infinity(), x));
    EXPECT_FALSE(DoubleE6::try_from_double(1e300, x));
    EXPECT_FALSE(DoubleE6::try_from_double(DoubleE6::MAX_ABS_DOUBLE, x));
    EXPECT_EQ(x.get_raw(), 1'500'000);  // untouched on failure
    EXPECT_TRUE(DoubleE6::try_from_double(DoubleE6::MAX_ABS_DOUBLE * 0.999, x));
    EXPECT_TRUE(DoubleE6::try_from_double(-DoubleE6::MAX_ABS_DOUBLE * 0.999, x));
    EXPECT_LT(x.get_raw(), 0);
}

TEST_F(TestScaleInt, TestIntegerArithmetic) {
    const DoubleE4 a(1.5), b(0.25);
    EXPECT_EQ((-a).get_raw(), -15'000);
    EXPECT_EQ((-a).abs().get_raw(), 15'000);
    EXPECT_EQ(a.abs().get_raw(), 15'000);
    EXPECT_EQ((a * 3).get_raw(), 45'000);
    EXPECT_EQ((-3 * a).get_raw(), -45'000);
    EXPECT_EQ((b * 0).get_raw(), 0);
    EXPECT_TRUE(DoubleE4().is_zero());
    EXPECT_TRUE((a - a).is_zero());
    EXPECT_FALSE(b.is_zero());
}

TEST_F(TestScaleInt, TestHash) {
    std::unordered_set<DoubleE6> s;
    s.insert(DoubleE6(1.5));
    s.insert(DoubleE6(1.5));
    s.insert(DoubleE6(0.1 + 0.2));
    s.insert(DoubleE6(0.3));
    EXPECT_EQ(s.size(), 2u);
    EXPECT_EQ(std::hash<DoubleE6>{}(DoubleE6(1.5)), std::hash<int64_t>{}(1'500'000));
}

TEST_F(TestScaleInt, TestRescale) {
    EXPECT_EQ(DoubleE4(1.5).rescale<4>().get_raw(), 15'000);
    EXPECT_EQ(DoubleE4(1.5).rescale<9>().get_raw(), 1'500'000'000);
    EXPECT_EQ(DoubleE4(-1.5).rescale<13>().get_raw(), -15'000'000'000'000);
    EXPECT_EQ(DoubleE4(1.5).rescale<2>().get_raw(), 150);
    // narrowing rounds half away from zero, symmetric
    EXPECT_EQ(DoubleE4::from_raw(15).rescale<3>().get_raw(), 2);
    EXPECT_EQ(DoubleE4::from_raw(-15).rescale<3>().get_raw(), -2);
    EXPECT_EQ(DoubleE4::from_raw(14).rescale<3>().get_raw(), 1);
    EXPECT_EQ(DoubleE4::from_raw(-14).rescale<3>().get_raw(), -1);
    EXPECT_EQ(DoubleE4::from_raw(4).rescale<3>().get_raw(), 0);
    EXPECT_EQ(DoubleE4::from_raw(-4).rescale<3>().get_raw(), 0);
    EXPECT_EQ(DoubleE13::from_raw(std::numeric_limits<int64_t>::max()).rescale<1>().get_raw(), 9'223'372);
    EXPECT_EQ(DoubleE13::from_raw(std::numeric_limits<int64_t>::min()).rescale<1>().get_raw(), -9'223'372);
    EXPECT_EQ(DoubleE4::from_raw(std::numeric_limits<int64_t>::max()).round_to_tick_raw(100).get_raw(),
              std::numeric_limits<int64_t>::max() - 7);  // ...5807 rounds down to ...5800
    EXPECT_EQ(DoubleE4::from_raw(std::numeric_limits<int64_t>::min()).round_to_tick_raw(100).get_raw(),
              std::numeric_limits<int64_t>::min() + 8);  // ...5808 rounds toward zero to ...5800
    // widen then narrow is the identity
    for (int64_t raw : {-123456789, -1, 0, 1, 987654321}) {
        EXPECT_EQ(DoubleE4::from_raw(raw).rescale<9>().rescale<4>().get_raw(), raw);
    }
}

TEST_F(TestScaleInt, TestTickToRaw) {
    // 0.0003 * 10^4 == 2.9999999999999996, must round to 3
    EXPECT_EQ(DoubleE4::tick_to_raw(0.0003), 3);
    EXPECT_EQ(ScaleInt<5>::tick_to_raw(0.0003), 30);
    EXPECT_EQ(DoubleE8::tick_to_raw(0.0003), 30'000);
    EXPECT_EQ(DoubleE13::tick_to_raw(0.0003), 3'000'000'000);
    EXPECT_EQ(ScaleInt<2>::tick_to_raw(0.05), 5);
    EXPECT_EQ(DoubleE8::tick_to_raw(0.00000001), 1);  // Binance spot minimum tick
}

TEST_F(TestScaleInt, TestRoundToTickRawMatchesReference) {
    for (int64_t tick : {1, 2, 3, 4, 5, 7, 10, 25, 100, 1000}) {
        for (int64_t raw = -2000; raw <= 2000; ++raw) {
            DoubleE4 x = DoubleE4::from_raw(raw);
            x.round_to_tick_raw(tick);
            // llround is half away from zero; the ratios are exact enough for these magnitudes
            const int64_t expected = std::llround(static_cast<double>(raw) / static_cast<double>(tick)) * tick;
            ASSERT_EQ(x.get_raw(), expected) << "raw " << raw << " tick " << tick;
        }
    }
}

TEST_F(TestScaleInt, TestFloorAndCeilToTickRawMatchReference) {
    for (int64_t tick : {1, 2, 3, 5, 7, 10, 25}) {
        for (int64_t raw = -500; raw <= 500; ++raw) {
            DoubleE4 f = DoubleE4::from_raw(raw);
            DoubleE4 c = DoubleE4::from_raw(raw);
            f.floor_to_tick_raw(tick);
            c.ceil_to_tick_raw(tick);
            const double ratio = static_cast<double>(raw) / static_cast<double>(tick);
            ASSERT_EQ(f.get_raw(), static_cast<int64_t>(std::floor(ratio)) * tick) << raw << "/" << tick;
            ASSERT_EQ(c.get_raw(), static_cast<int64_t>(std::ceil(ratio)) * tick) << raw << "/" << tick;
            ASSERT_TRUE(f.is_multiple_of_tick_raw(tick));
            ASSERT_TRUE(c.is_multiple_of_tick_raw(tick));
        }
    }
}

TEST_F(TestScaleInt, TestTickHelpersWithRealisticTicks) {
    // buy rounds down, sell rounds up, mid rounds to nearest
    const int64_t tick = DoubleE8::tick_to_raw(0.0003);
    DoubleE8 buy(0.00061), sell(0.00061), mid(0.00061);
    buy.floor_to_tick_raw(tick);
    sell.ceil_to_tick_raw(tick);
    mid.round_to_tick_raw(tick);
    EXPECT_EQ(buy.get_raw(), 60'000);
    EXPECT_EQ(sell.get_raw(), 90'000);
    EXPECT_EQ(mid.get_raw(), 60'000);
    EXPECT_TRUE(buy.is_multiple_of_tick_raw(tick));
    EXPECT_FALSE(DoubleE8(0.00061).is_multiple_of_tick_raw(tick));

    // double tick overloads give the same results
    EXPECT_EQ(DoubleE8(0.00061).floor_to_tick(0.0003).get_raw(), 60'000);
    EXPECT_EQ(DoubleE8(0.00061).ceil_to_tick(0.0003).get_raw(), 90'000);
    EXPECT_EQ(DoubleE8(-0.00061).floor_to_tick(0.0003).get_raw(), -90'000);
    EXPECT_EQ(DoubleE8(-0.00061).ceil_to_tick(0.0003).get_raw(), -60'000);

    // exact multiples are unchanged by all three
    EXPECT_EQ(ScaleInt<2>(1.25).floor_to_tick(0.05).get_raw(), 125);
    EXPECT_EQ(ScaleInt<2>(1.25).ceil_to_tick(0.05).get_raw(), 125);
    EXPECT_EQ(ScaleInt<2>(1.25).round_to_tick(0.05).get_raw(), 125);

    // a tick finer than the resolution leaves the value unchanged
    EXPECT_EQ(DoubleE4(1.2345).floor_to_tick(1e-5).get_raw(), 12'345);
    EXPECT_EQ(DoubleE4(1.2345).ceil_to_tick(0).get_raw(), 12'345);
}

TEST_F(TestScaleInt, TestToCharsWritesNoNulAndReturnsEnd) {
    char buf[64];
    std::memset(buf, 'x', sizeof(buf));
    char* end = DoubleE4::from_raw(-12345).to_chars(buf);
    EXPECT_EQ(end - buf, 7);  // "-1.2345"
    EXPECT_EQ(*end, 'x');
    EXPECT_EQ(std::string(buf, end), "-1.2345");

    // the longest output fits in MAX_CHARS
    end = DoubleE13::from_raw(std::numeric_limits<int64_t>::min()).to_chars(buf);
    EXPECT_LE(static_cast<size_t>(end - buf), DoubleE13::MAX_CHARS);
    end = ScaleInt<1>::from_raw(std::numeric_limits<int64_t>::min()).to_chars(buf);
    EXPECT_LE(static_cast<size_t>(end - buf), ScaleInt<1>::MAX_CHARS);
}

// ---- INT64_MIN and overflow --------------------------------------------------------------------
static_assert((-DoubleE8::from_raw(std::numeric_limits<int64_t>::max())).get_raw() == std::numeric_limits<int64_t>::min() + 1);
static_assert(DoubleE8::from_raw(std::numeric_limits<int64_t>::min() + 1).abs().get_raw() == std::numeric_limits<int64_t>::max());

TEST_F(TestScaleInt, TestNegationAndAbsOfInt64Min) {
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    EXPECT_EQ((-DoubleE8::from_raw(kMax)).get_raw(), kMin + 1);
    EXPECT_EQ(DoubleE8::from_raw(kMin + 1).abs().get_raw(), kMax);
    // -INT64_MIN and abs(INT64_MIN) do not fit: assert in debug builds, wrap (no UB) in release
    EXPECT_DEBUG_DEATH((void)(-DoubleE8::from_raw(kMin)), "ScaleInt overflow");
    EXPECT_DEBUG_DEATH((void)DoubleE8::from_raw(kMin).abs(), "abs\\(min\\) overflows");
    EXPECT_DEBUG_DEATH((void)(DoubleE8::from_raw(kMax) + DoubleE8::from_raw(1)), "ScaleInt overflow");
    EXPECT_DEBUG_DEATH((void)(DoubleE8::from_raw(kMin) - DoubleE8::from_raw(1)), "ScaleInt overflow");
    EXPECT_DEBUG_DEATH((void)(DoubleE8::from_raw(kMin) * -1), "ScaleInt overflow");
    EXPECT_DEBUG_DEATH((void)DoubleE4::from_raw(kMax / 10).rescale<6>(), "ScaleInt overflow");
#ifdef NDEBUG
    EXPECT_EQ((-DoubleE8::from_raw(kMin)).get_raw(), kMin);
    EXPECT_EQ(DoubleE8::from_raw(kMin).abs().get_raw(), kMin);
    EXPECT_EQ((DoubleE8::from_raw(kMax) + DoubleE8::from_raw(1)).get_raw(), kMin);
#endif
    // formatting INT64_MIN goes through the unsigned magnitude
    EXPECT_EQ(std::string(DoubleE8::from_raw(kMin).to_string().c_str()), "-92233720368.54775808");
}

TEST_F(TestScaleInt, TestMaxAbsDoubleBoundaryAllScales) {
    for_each_scale_1_to_13([](auto sc) {
        constexpr int S = decltype(sc)::value;
        using T = ScaleInt<S>;
        const double below = std::nextafter(T::MAX_ABS_DOUBLE, 0.0);
        // the largest accepted magnitude scales to below 2^63 and converts without overflow
        ASSERT_LT(below * T::SCALE_AS_DOUBLE + 0.5, TWO_POW_63) << "E" << S;
        T x;
        ASSERT_TRUE(T::try_from_double(below, x)) << "E" << S;
        EXPECT_GT(x.get_raw(), 9'200'000'000'000'000'000) << "E" << S;  // uses the full range, not 9.2e18
        ASSERT_TRUE(T::try_from_double(-below, x)) << "E" << S;
        EXPECT_LT(x.get_raw(), -9'200'000'000'000'000'000) << "E" << S;
        // the bound itself is rejected (it may scale to exactly 2^63)
        EXPECT_FALSE(T::try_from_double(T::MAX_ABS_DOUBLE, x)) << "E" << S;
        EXPECT_FALSE(T::try_from_double(-T::MAX_ABS_DOUBLE, x)) << "E" << S;
    });
    EXPECT_DOUBLE_EQ(MAX_SUPPORT_DOUBLE, TWO_POW_63 / 1e4);
}

// ---- tick rounding near the limits ------------------------------------------------------------
TEST_F(TestScaleInt, TestTickRoundingNearInt64Limits) {
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    // the exact result is outside int64: nearest representable multiple toward zero, no overflow
    EXPECT_EQ(DoubleE4::from_raw(kMax).round_to_tick_raw(10).get_raw(), kMax - 7);
    EXPECT_EQ(DoubleE4::from_raw(kMin).round_to_tick_raw(10).get_raw(), kMin + 8);
    EXPECT_EQ(DoubleE4::from_raw(kMin).floor_to_tick_raw(10).get_raw(), kMin + 8);
    EXPECT_EQ(DoubleE4::from_raw(kMax).ceil_to_tick_raw(10).get_raw(), kMax - 7);
    EXPECT_EQ(DoubleE4::from_raw(kMin).floor_to_tick<10>().get_raw(), kMin + 8);
    EXPECT_EQ(DoubleE4::from_raw(kMax).ceil_to_tick<10>().get_raw(), kMax - 7);
    EXPECT_EQ(DoubleE4::from_raw(kMin).floor_to_tick(TickSize<4>::from_raw(10)).get_raw(), kMin + 8);
    EXPECT_EQ(DoubleE4::from_raw(kMax).ceil_to_tick(TickSize<4>::from_raw(10)).get_raw(), kMax - 7);
    // representable results stay exact
    EXPECT_EQ(DoubleE4::from_raw(kMax).floor_to_tick_raw(10).get_raw(), kMax - 7);
    EXPECT_EQ(DoubleE4::from_raw(kMin).ceil_to_tick_raw(10).get_raw(), kMin + 8);
    EXPECT_EQ(DoubleE4::from_raw(kMin).floor_to_tick_raw(1).get_raw(), kMin);
    EXPECT_EQ(DoubleE4::from_raw(kMax).ceil_to_tick_raw(1).get_raw(), kMax);
}

// ---- compile time and pre-converted ticks ------------------------------------------------------
static_assert(DoubleE8::from_raw(123'456'789).floor_to_tick<1'000'000>().get_raw() == 123'000'000);
static_assert(DoubleE8::from_raw(-123'456'789).floor_to_tick<1'000'000>().get_raw() == -124'000'000);
static_assert(DoubleE8::from_raw(123'456'789).ceil_to_tick<1'000'000>().get_raw() == 124'000'000);
static_assert(DoubleE8::from_raw(123'500'000).round_to_tick<1'000'000>().get_raw() == 124'000'000);
static_assert(DoubleE8::from_raw(123'000'000).is_multiple_of_tick<1'000'000>());
static_assert(TickSize<8>(0.01).raw() == 1'000'000 && TickSize<8>(0.01).pow10_exponent() == 6);
static_assert(TickSize<8>(0.00000001).pow10_exponent() == 0);
static_assert(TickSize<8>(0.05).pow10_exponent() == -1 && TickSize<8>(0.05).raw() == 5'000'000);
static_assert(DoubleE8::from_raw(-123'456'789).floor_to_tick(TickSize<8>(0.01)).get_raw() == -124'000'000);
static_assert(DoubleE8::from_raw(-123'456'789).floor_to_tick(TickSize<8>(0.05)).get_raw() == -125'000'000);

TEST_F(TestScaleInt, TestTickSize) {
    EXPECT_EQ(TickSize<8>(0.1).pow10_exponent(), 7);
    EXPECT_EQ(TickSize<8>(1.0).pow10_exponent(), 8);
    EXPECT_EQ(TickSize<8>(100.0).pow10_exponent(), 10);
    EXPECT_EQ(TickSize<8>(0.5).pow10_exponent(), -1);
    EXPECT_EQ(TickSize<8>(0.25).pow10_exponent(), -1);
    EXPECT_EQ(TickSize<8>(0.0003).raw(), 30'000);  // rounded, not truncated
    EXPECT_EQ(TickSize<4>::from_raw(1'000'000'000'000'000'000).pow10_exponent(), 18);
    EXPECT_EQ(TickSize<4>::from_raw(7).pow10_exponent(), -1);
}

// Every tick API must agree with the plain runtime division, for power of ten ticks (constant
// divisor) and others (idiv).
TEST_F(TestScaleInt, TestTickApisAgree) {
    std::mt19937_64 rng(99);
    std::uniform_int_distribution<int64_t> any(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
    std::uniform_int_distribution<int64_t> small(-10'000'000'000, 10'000'000'000);
    std::vector<int64_t> raws = {0, 1, -1, 499'999, 500'000, -500'000, 1'500'000, -1'500'000,
                                 std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    for (int i = 0; i < 2'000; ++i) {
        raws.push_back(any(rng));
        raws.push_back(small(rng));
    }
    std::vector<int64_t> ticks = {3, 5, 25, 30'000, 5'000'000, 999'999'937};
    for (int k = 0; k <= 18; ++k) {
        ticks.push_back(ipow10<int64_t>(k));
    }
    for (int64_t tick_raw : ticks) {
        const TickSize<8> tick = TickSize<8>::from_raw(tick_raw);
        for (int64_t raw : raws) {
            const DoubleE8 x = DoubleE8::from_raw(raw);
            ASSERT_EQ(DoubleE8(x).round_to_tick(tick).get_raw(), DoubleE8(x).round_to_tick_raw(tick_raw).get_raw()) << raw << " " << tick_raw;
            ASSERT_EQ(DoubleE8(x).floor_to_tick(tick).get_raw(), DoubleE8(x).floor_to_tick_raw(tick_raw).get_raw()) << raw << " " << tick_raw;
            ASSERT_EQ(DoubleE8(x).ceil_to_tick(tick).get_raw(), DoubleE8(x).ceil_to_tick_raw(tick_raw).get_raw()) << raw << " " << tick_raw;
            ASSERT_EQ(x.is_multiple_of_tick(tick), x.is_multiple_of_tick_raw(tick_raw)) << raw << " " << tick_raw;
        }
    }
    for (int64_t raw : raws) {
        const DoubleE8 x = DoubleE8::from_raw(raw);
        ASSERT_EQ(DoubleE8(x).floor_to_tick<1'000'000>().get_raw(), DoubleE8(x).floor_to_tick_raw(1'000'000).get_raw()) << raw;
        ASSERT_EQ(DoubleE8(x).ceil_to_tick<5'000'000>().get_raw(), DoubleE8(x).ceil_to_tick_raw(5'000'000).get_raw()) << raw;
        ASSERT_EQ(DoubleE8(x).round_to_tick<25>().get_raw(), DoubleE8(x).round_to_tick_raw(25).get_raw()) << raw;
        ASSERT_EQ(DoubleE8(x).round_to_tick<1024>().get_raw(), DoubleE8(x).round_to_tick_raw(1024).get_raw()) << raw;
        ASSERT_EQ(x.is_multiple_of_tick<100>(), x.is_multiple_of_tick_raw(100)) << raw;
    }
}

TEST_F(TestScaleInt, TestEqualsRoundedVsEquals) {
    // grid equality is transitive within a scale, tolerance equality is not
    EXPECT_TRUE(equals_rounded(1.00000000000004, 1.0));
    EXPECT_TRUE(equals_rounded(1.00000000000004, 1.00000000000001));
    EXPECT_FALSE(equals(1.00000000000004, 1.0));  // 4 ulp relative tolerance
    // absolute epsilon is math::equals with rel = 0
    EXPECT_TRUE(equals(100.0, 100.004, {.rel = 0.0, .abs = 0.005}));
    EXPECT_FALSE(equals_rounded(100.0, 100.004));
}

// to_raw() is llround(v * 10^SCALE): no double rounding just below a half, and exact for scaled
// values in [2^52, 2^53) where adding 0.5 would round an odd integer up
TEST_F(TestScaleInt, TestToRawMatchesLlround) {
    EXPECT_EQ(ScaleInt<1>(std::nextafter(0.5, 0.0) / 10).get_raw(), 0);
    EXPECT_EQ(DoubleE13(std::nextafter(0.5, 0.0) / 1e13).get_raw(), 0);
    EXPECT_EQ(DoubleE8(45035996.273704968).get_raw(), 4503599627370497);
    EXPECT_EQ(DoubleE13(500.0000000000001).get_raw(), 5000000000000001);
    EXPECT_EQ(DoubleE4(-0.00005).get_raw(), -1);  // ties still away from zero
    EXPECT_EQ(DoubleE4(0.00005).get_raw(), 1);

    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> exponent(-16, 18);
    for (int i = 0; i < 1'000'000; ++i) {
        double v = std::pow(10.0, exponent(rng)) / DoubleE8::SCALE_AS_DOUBLE;
        if (i & 1) v = -v;
        if (!(std::abs(v) < DoubleE8::MAX_ABS_DOUBLE)) continue;
        ASSERT_EQ(DoubleE8::to_raw(v), std::llround(v * DoubleE8::SCALE_AS_DOUBLE)) << std::setprecision(17) << v;
    }
}

// px * 1.5 would silently truncate the factor to 1
static_assert(!std::is_invocable_v<std::multiplies<>, DoubleE8, double>);
static_assert(!std::is_invocable_v<std::multiplies<>, double, DoubleE8>);
static_assert(!std::is_invocable_v<std::multiplies<>, DoubleE8, float>);
static_assert(std::is_invocable_v<std::multiplies<>, DoubleE8, int64_t>);
static_assert(std::is_invocable_v<std::multiplies<>, int, DoubleE8>);

TEST_F(TestScaleInt, TestTickSizeTryFromDouble) {
    TickSize<8> t = TickSize<8>::from_raw(1);
    EXPECT_TRUE(TickSize<8>::try_from_double(0.01, t));
    EXPECT_EQ(t.raw(), 1'000'000);
    EXPECT_TRUE(TickSize<8>::try_from_double(0.0005, t));
    EXPECT_EQ(t.raw(), 50'000);
    EXPECT_TRUE(TickSize<8>::try_from_double(1e-8, t));
    EXPECT_EQ(t.raw(), 1);

    const TickSize<8> before = t;
    for (double bad : {1e-10, 1.5e-8, 0.0, -0.01, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(), 1e300}) {
        EXPECT_FALSE(TickSize<8>::try_from_double(bad, t)) << bad;
        EXPECT_EQ(t.raw(), before.raw());  // untouched
    }
    TickSize<4> t4 = TickSize<4>::from_raw(1);
    EXPECT_FALSE(TickSize<4>::try_from_double(0.00005, t4));  // half a unit at E4
}

TEST_F(TestScaleInt, TestPerCallTickOutOfRangeLeavesValueUnchanged) {
    for (double bad : {1e6 * 1e300, 1e300, -1e300, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(), 1e-12}) {
        DoubleE8 x(1.25);
        x.floor_to_tick(bad);
        x.ceil_to_tick(bad);
        x.round_to_tick(bad);
        EXPECT_EQ(x.get_raw(), 125'000'000) << bad;
    }
    // a valid but large lot size still rounds
    EXPECT_EQ(DoubleE8(3.0).floor_to_tick(1e6).get_raw(), 0);
    EXPECT_EQ(math::round_to_tick(3.0, 1e300), 3.0);
    EXPECT_EQ(math::round_to_tick(0.0, 1e6), 0.0);
}

#ifdef NDEBUG
// release builds: a tick finer than one unit rounds nothing instead of dividing by zero
TEST_F(TestScaleInt, TestTickSizeBelowOneUnitIsHarmlessInRelease) {
    const TickSize<8> t(1e-10);
    EXPECT_EQ(t.raw(), 1);
    DoubleE8 x(1.23456789);
    x.floor_to_tick(t);
    EXPECT_EQ(x.get_raw(), 123'456'789);
}
#endif
