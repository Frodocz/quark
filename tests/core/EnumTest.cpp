#include <cstdint>

#include <gtest/gtest.h>

#include "quark/QuarkEnum.h"

namespace {

enum class Side : int8_t { Unknown = 0, Buy = 1, Sell = 2 };

// sparse, with values beyond magic_enum's default range [-128, 127]
enum class ErrorCode : uint16_t { Ok = 0, NotReady = 112, InvalidSid = 201, MktReject = 321, MaxPosition = 350 };

} // namespace

template <>
struct magic_enum::customize::enum_range<ErrorCode> {
    static constexpr int min = 0;
    static constexpr int max = 400;
};

using namespace quark;

// usable in constant expressions
static_assert(enum_name(Side::Buy) == "Buy");
static_assert(enum_count_v<Side> == 3);
static_assert(enum_from_name<Side>("Sell") == Side::Sell);

TEST(QuarkEnum, Name) {
    EXPECT_EQ(enum_name(Side::Sell), "Sell");
    EXPECT_EQ(enum_name(static_cast<Side>(7)), "");  // not an enumerator
}

TEST(QuarkEnum, FromName) {
    EXPECT_EQ(enum_from_name<Side>("Buy"), Side::Buy);
    EXPECT_FALSE(enum_from_name<Side>("buy").has_value());  // case-sensitive
    EXPECT_FALSE(enum_from_name<Side>("").has_value());
}

TEST(QuarkEnum, FromValueRejectsUnknownValues) {
    EXPECT_EQ(enum_from_value<Side>(2), Side::Sell);
    EXPECT_FALSE(enum_from_value<Side>(3).has_value());
    EXPECT_FALSE(enum_from_value<Side>(-1).has_value());
}

TEST(QuarkEnum, CustomRangeBeyond128) {
    EXPECT_EQ(enum_name(ErrorCode::MktReject), "MktReject");
    EXPECT_EQ(enum_from_value<ErrorCode>(350), ErrorCode::MaxPosition);
    EXPECT_FALSE(enum_from_value<ErrorCode>(322).has_value());
    EXPECT_EQ(enum_count_v<ErrorCode>, 5u);

    constexpr auto values = enum_values<ErrorCode>();
    static_assert(values.front() == ErrorCode::Ok && values.back() == ErrorCode::MaxPosition);
}

TEST(QuarkEnum, DefaultRangeLimit) {
    // a value outside [-128, 127] without a customize::enum_range has no name: documented, not a bug
    enum class Wide : uint16_t { Small = 1, Large = 300 };
    EXPECT_EQ(enum_name(Wide::Small), "Small");
    EXPECT_EQ(enum_name(Wide::Large), "");
}
