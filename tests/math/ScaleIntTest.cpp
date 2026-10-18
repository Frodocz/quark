#include <thread>
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
    const double EPSILION = std::numeric_limits<double>::epsilon();
    EXPECT_EQ(MyDouble::SCALE_AS_INT, 10000000000000);

    EXPECT_EQ(MyDouble(1e-13).get_raw(), 0);

    EXPECT_EQ(MyDouble(123.456e-13).get_raw(), 123);
    EXPECT_EQ(MyDouble(0.0000000123456).get_raw(), 123456);

    EXPECT_EQ(MyDouble(-123.456e-13).get_raw(), -123);
    EXPECT_EQ(MyDouble(-0.0000000123456).get_raw(), -123456);

    {
        MyDouble x;
        x = 1234.56789;
        EXPECT_NEAR(x.as_double(), 1234.56789, EPSILION);
        EXPECT_EQ(x.get_raw(), 12345678900000000);
    }

    {
        MyDouble x;
        x = 0.0029999999999999996;
        EXPECT_NEAR(x.as_double(), 0.003, EPSILION);
        EXPECT_EQ(x.get_raw(), 30000000000);
    }

    {
        MyDouble x;
        x = -0.0029999999999999996;
        EXPECT_NEAR(x.as_double(), -0.003, EPSILION);
        EXPECT_EQ(x.get_raw(), -30000000000);
    }

    {
        MyDouble x;
        x = 0.0030000000000000002;
        EXPECT_NEAR(x.as_double(), 0.003, EPSILION);
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
            DoubleE9(t.first).to_string(buf);
        }
    }

    const auto end = RdtscClock::rdtsc();
    QUARK_LOG_INFO("latency per num:{}", (end - start) / (test_cases.size() * 10));
}

TEST_F(TestScaleInt, TestArithmeticE9) {
    using MyDouble = ScaleInt<9>;
    const double EPSILION = std::numeric_limits<double>::epsilon();

    EXPECT_EQ(MyDouble::SCALE_AS_DOUBLE, 1'000'000'000);

    EXPECT_EQ(MyDouble(123456.123456789).get_raw(), 123456123456789);

    EXPECT_TRUE(MyDouble(1.12345678) == MyDouble(1.12345678));
    EXPECT_TRUE(MyDouble(1.123456789) != MyDouble(1.12345678));

    EXPECT_TRUE(MyDouble(1.12345679) > MyDouble(1.12345678));
    EXPECT_TRUE(MyDouble(1.12345678) < MyDouble(1.12345679));

    EXPECT_TRUE(MyDouble(1.12345679) >= MyDouble(1.12345678));
    EXPECT_TRUE(MyDouble(1.12345678) <= MyDouble(1.12345679));

    EXPECT_NEAR((MyDouble(1.22334455) - MyDouble(1.12345678)).as_double(), 0.09988777, EPSILION);
    EXPECT_EQ((MyDouble(1.22334455) - MyDouble(1.12345678)).get_raw(), 99887770);

    EXPECT_NEAR((MyDouble(1.22334455) + MyDouble(1.12345678)).as_double(), 2.34680133, EPSILION);
    EXPECT_EQ((MyDouble(1.22334455) + MyDouble(1.12345678)).get_raw(), 2346801330);

    EXPECT_NEAR((MyDouble(-1.22334455) - MyDouble(1.12345678)).as_double(), -2.34680133, EPSILION);
    EXPECT_EQ((MyDouble(-1.22334455) - MyDouble(1.12345678)).get_raw(), -2346801330);
    EXPECT_NEAR((MyDouble(-1.22334455) + MyDouble(1.12345678)).as_double(), -0.09988777, EPSILION);
    EXPECT_EQ((MyDouble(-1.22334455) + MyDouble(1.12345678)).get_raw(), -99887770);

    EXPECT_NEAR(MyDouble(123456.123456789).as_double(), 123456.123456789, EPSILION);
    EXPECT_EQ(MyDouble(123456.123456789).get_raw(), 123456123456789);

    EXPECT_TRUE(MyDouble(1.12345678) == (1.12345678));
    EXPECT_TRUE(MyDouble(1.123456789) != (1.12345678));

    EXPECT_TRUE(MyDouble(1.12345679) > (1.12345678));
    EXPECT_TRUE(MyDouble(1.12345678) < (1.12345679));

    EXPECT_TRUE(MyDouble(1.12345679) >= (1.12345678));
    EXPECT_TRUE(MyDouble(1.12345678) <= (1.12345679));

    EXPECT_NEAR((MyDouble(1.22334455) - (1.12345678)).as_double(), 0.09988777, EPSILION);
    EXPECT_EQ((MyDouble(1.22334455) - (1.12345678)).get_raw(), 99887770);
    EXPECT_NEAR((MyDouble(1.22334455) + (1.12345678)).as_double(), 2.34680133, EPSILION);
    EXPECT_EQ((MyDouble(1.22334455) + (1.12345678)).get_raw(), 2346801330);

    EXPECT_NEAR((MyDouble(-1.22334455) - (1.12345678)).as_double(), -2.34680133, EPSILION);
    EXPECT_EQ((MyDouble(-1.22334455) - (1.12345678)).get_raw(), -2346801330);
    EXPECT_NEAR((MyDouble(-1.22334455) + (1.12345678)).as_double(), -0.09988777, EPSILION);
    EXPECT_EQ((MyDouble(-1.22334455) + (1.12345678)).get_raw(), -99887770);

    {
        MyDouble x(123.123456);
        x += 0.111111;
        EXPECT_NEAR(x.as_double(), 123.234567, EPSILION);
        EXPECT_EQ(x.get_raw(), 123234567000);
    }

    {
        MyDouble x(123.123456);
        x -= 0.111111;
        EXPECT_NEAR(x.as_double(), 123.012345, EPSILION);
        EXPECT_EQ(x.get_raw(), 123012345000);
    }

    {
        MyDouble x(123.123456);
        MyDouble y(0.111111);
        x += y;
        EXPECT_NEAR(x.as_double(), 123.234567, EPSILION);
        EXPECT_EQ(x.get_raw(), 123234567000);
    }

    {
        MyDouble x(123.123456);
        MyDouble y(0.111111);
        x -= y;
        EXPECT_NEAR(x.as_double(), 123.012345, EPSILION);
        EXPECT_EQ(x.get_raw(), 123012345000);
    }

    {
        MyDouble x;
        x = 1234.56789;
        EXPECT_NEAR(x.as_double(), 1234.56789, EPSILION);
        EXPECT_EQ(x.get_raw(), 1234567890000);
    }

    {
        MyDouble x;
        x = 0.0029999999999999996;
        EXPECT_NEAR(x.as_double(), 0.003, EPSILION);
        EXPECT_EQ(x.get_raw(), 3000000);
    }

    {
        MyDouble x;
        x = -0.0029999999999999996;
        EXPECT_NEAR(x.as_double(), -0.003, EPSILION);
        EXPECT_EQ(x.get_raw(), -3000000);
    }

    {
        MyDouble x;
        x = 0.0030000000000000002;
        EXPECT_NEAR(x.as_double(), 0.003, EPSILION);
        EXPECT_EQ(x.get_raw(), 3000000);
    }

    {
        MyDouble x;
        x = -0.0030000000000000002;
        EXPECT_NEAR(x.as_double(), -0.003, EPSILION);
        EXPECT_EQ(x.get_raw(), -3000000);
    }

    {
        MyDouble x(49436.100000000006);
        EXPECT_TRUE(quark::math::is_equal(49436.100000000006, x.as_double()));
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

    std::cout << MyDouble(-0.0001).to_string().c_str() << std::endl;
}

TEST_F(TestScaleInt, TsDoubleToString) {
    using MyDouble = ScaleInt<8>;
    const double EPSILION = std::numeric_limits<double>::epsilon();
    std::vector<double> nums = {637348.63, 4059203.34, 3643706.53, 584436.89, 47737.13,
                                48045.75,  20820.67,   38298.41,   48032.07,  99618.21};
    double numInit = 0.0;
    MyDouble num(0);
    for (auto numRaw : nums) {
        num = MyDouble(numInit);
        num += numRaw;
        numInit = num.as_double();
    }

    EXPECT_NEAR(numInit, 9227247.63, EPSILION);
    std::cout << num.to_string().c_str() << std::endl;
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

    EXPECT_TRUE(is_equal(9999.9999999999, 10000.0000000001));
    EXPECT_FALSE(is_equal(9999.9999, 10000));
}

TEST_F(TestScaleInt, TestIsEqual) {
    EXPECT_FALSE(is_equal(1.2e-12, 0));
    EXPECT_FALSE(is_equal(1.000000000002e6, 1e6));
    EXPECT_TRUE(is_equal(1.00000000000002e6, 1e6));
    EXPECT_TRUE(is_equal(1.2e-12, 0, 1e-8));
    EXPECT_TRUE(is_equal(948.8000000000001, 948.8));
}

TEST_F(TestScaleInt, TestOverflowCrash) {
    // Test special floating point values
    std::cout << "Testing special floating point values:" << std::endl;

    // Test NaN
    std::cout << "Testing NaN: ";
    double nanValue = std::numeric_limits<double>::quiet_NaN();
    auto resultNaN = quark::math::double_to_string(nanValue);
    std::cout << "Result: '" << resultNaN.c_str() << "'" << std::endl;
    EXPECT_EQ(std::string(resultNaN.data_), "nan");

    // Test Infinity
    double infValue = std::numeric_limits<double>::infinity();
    std::cout << "Testing +Infinity: ";
    auto resultInf = quark::math::double_to_string(infValue);
    std::cout << "Result: '" << resultInf.c_str() << "'" << std::endl;
    EXPECT_EQ(std::string(resultInf.data_), "inf");

    // Test -Infinity
    double negInfValue = -std::numeric_limits<double>::infinity();
    std::cout << "Testing -Infinity: ";
    auto resultNegInf = quark::math::double_to_string(negInfValue);
    std::cout << "Result: '" << resultNegInf.c_str() << "'" << std::endl;
    EXPECT_EQ(std::string(resultNegInf.data_), "-inf");

    // Test very large values that might cause issues
    std::cout << "\nTesting large values near pivot boundaries:" << std::endl;

    // Values just below and above E13PIVOT (10)
    double val1 = 9.99999999999999;
    double val2 = 10.00000000000001;
    std::cout << "Value " << val1 << " -> '" << quark::math::double_to_string(val1).c_str() << "'" << std::endl;
    std::cout << "Value " << val2 << " -> '" << quark::math::double_to_string(val2).c_str() << "'" << std::endl;

    // Test maximum double value
    double maxDouble = std::numeric_limits<double>::max();
    std::cout << "\nTesting max double value: " << maxDouble << std::endl;
    try {
        auto resultMax = quark::math::double_to_string(maxDouble);
        std::cout << "Result: '" << resultMax.c_str() << "'" << std::endl;
        EXPECT_EQ(std::string(resultMax.data_), "1.7976931348623157e+308");
    } catch (...) {
        std::cout << "Exception with max double!" << std::endl;
    }
}
