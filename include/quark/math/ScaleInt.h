#pragma once

#include "quark/Attributes.h"
#include "quark/math/DoubleOps.h"
#include "quark/QuarkFormat.h"

#include <cassert>
#include <compare>
#include <cstdint>
#include <cstdlib>

QUARK_BEGIN_NAMESPACE

namespace math {

struct SmallStrBuf {
    constexpr SmallStrBuf() { data_[0] = '\0'; }
    const char *c_str() const noexcept { return data_; }
    char data_[64];
};

/*
 * ScaleInt assumes the double is normal and its value after scaled and rounded is
 * still within the range of int64_t
 */
template <int SCALE>
struct ScaleInt {
public:
    static_assert(SCALE >= 1 && SCALE <= 13, "ScaleInt only support SCALE in range [1, 13]");

    // clang-format off
    static constexpr double kScale2DoubleTbl[14] = {
        1,
        10,
        100,
        1'000,
        10'000,
        100'000,
        1'000'000,
        10'000'000,
        100'000'000,
        1'000'000'000,
        10'000'000'000,
        100'000'000'000,
        1'000'000'000'000,
        10'000'000'000'000,
    };
    // clang-format on

    using SelfT = ScaleInt<SCALE>;

    static int get_scale() noexcept { return SCALE; }

    static constexpr double SCALE_AS_DOUBLE = kScale2DoubleTbl[SCALE];
    static constexpr double TICK = 1.0 / SCALE_AS_DOUBLE;
    static constexpr int64_t SCALE_AS_INT = kScale2DoubleTbl[SCALE];
    static constexpr math::Tolerance<double> EPSILION{.abs = 1e-13};

    ScaleInt() = default;

    constexpr explicit ScaleInt(double v) noexcept {
        if (math::is_zero(v, EPSILION)) {
            v_ = 0;
        } else if (math::greater(v, 0.0, EPSILION)) {
            v_ = static_cast<int64_t>(v * SCALE_AS_DOUBLE + 0.5);
        } else {
            v_ = static_cast<int64_t>(v * SCALE_AS_DOUBLE - 0.5);
        }
    }

    constexpr double as_double() const noexcept { return static_cast<double>(v_) / SCALE_AS_DOUBLE; }

    constexpr int64_t get_raw() const noexcept { return v_; }

    void set_raw(int64_t raw) noexcept { v_ = raw; }

    SelfT& operator=(double v) noexcept {
        *this = ScaleInt(v);
        return *this;
    }

    SelfT& round_to_tick(double tick) noexcept {
        const int64_t tick_raw = tick * SCALE_AS_INT;
        const int64_t half_tick_raw = tick_raw / 2;
        v_ += half_tick_raw;
        v_ -= v_ % tick_raw;
        return *this;
    }

    constexpr bool operator==(const SelfT& rhs) const noexcept { return v_ == rhs.v_; }
    constexpr auto operator<=>(const SelfT&) const noexcept = default;

    constexpr SelfT operator+(const SelfT rhs) const noexcept {
        SelfT ret;
        ret.v_ = v_ + rhs.v_;
        return ret;
    }

    constexpr SelfT operator-(const SelfT rhs) const noexcept {
        SelfT ret;
        ret.v_ = v_ - rhs.v_;
        return ret;
    }

    constexpr SelfT& operator+=(const SelfT rhs) noexcept {
        v_ += rhs.v_;
        return *this;
    }

    constexpr SelfT& operator-=(const SelfT rhs) noexcept {
        v_ -= rhs.v_;
        return *this;
    }

    constexpr bool operator==(double rhs) const noexcept { return v_ == SelfT(rhs).v_; }
    constexpr auto operator<=>(double rhs) const noexcept { return v_ <=> SelfT(rhs).v_; }

    constexpr SelfT operator+(double rhs) const noexcept { return *this + SelfT(rhs); }
    constexpr SelfT operator-(double rhs) const noexcept { return *this - SelfT(rhs); }

    constexpr SelfT& operator+=(double rhs) noexcept {
        if (math::greater(rhs, 0.0, EPSILION)) {
            v_ += static_cast<int64_t>(rhs * SCALE_AS_DOUBLE + 0.5);
        } else if (math::less_than(rhs, 0.0, EPSILION)) {
            v_ += static_cast<int64_t>(rhs * SCALE_AS_DOUBLE - 0.5);
        }
        // else rhs == 0, do nothing
        return *this;
    }

    constexpr SelfT& operator-=(double rhs) noexcept {
        if (math::greater(rhs, 0.0, EPSILION)) {
            v_ -= static_cast<int64_t>(rhs * SCALE_AS_DOUBLE + 0.5);
        } else if (math::less_than(rhs, 0.0, EPSILION)) {
            v_ -= static_cast<int64_t>(rhs * SCALE_AS_DOUBLE - 0.5);
        }
        return *this;
    }

    void to_string(char (&dst)[64]) const noexcept {
        const int64_t abs_v = std::abs(v_);
        const int64_t rem = abs_v % SCALE_AS_INT;
        const int64_t quot = abs_v / SCALE_AS_INT;

        const auto quote_str = fmtlib::format_int(quot);
        size_t out_sz = 0;

        assert(quote_str.size() < sizeof(dst));

        if (v_ >= 0) {
            memcpy(dst, quote_str.c_str(), quote_str.size());
            out_sz += quote_str.size();
        } else {
            dst[0] = '-';
            out_sz++;
            memcpy(dst + out_sz, quote_str.c_str(), quote_str.size());
            out_sz += quote_str.size();
        }

        if (rem) {
            dst[out_sz++] = '.';
            const auto rem_str = fmtlib::format_int(rem);
            const int pad_zero_cnt = SCALE - rem_str.size();
            for (int i = 1; i <= pad_zero_cnt; ++i) {
                dst[out_sz++] = '0';
            }
            memcpy(dst + out_sz, rem_str.c_str(), rem_str.size());
            out_sz += rem_str.size();
            while (dst[out_sz - 1] == '0' && out_sz > 0) {
                out_sz--;
            }
        }
        dst[out_sz] = '\0';
    }

    SmallStrBuf to_string() const noexcept {
        SmallStrBuf buffer;
        to_string(buffer.data_);
        return buffer;
    }
private:
    int64_t v_ = 0;
} QUARK_PACKED;

using DoubleE4 = ScaleInt<4>;
using DoubleE6 = ScaleInt<6>;
using DoubleE8 = ScaleInt<8>;
using DoubleE9 = ScaleInt<9>;
using DoubleE13 = ScaleInt<13>;

constexpr double E13_PIVOT = 10.0;
constexpr double E9_PIVOT = 1.0e6;
constexpr double E6_PIVOT = 1.0e9;
constexpr double MAX_SUPPORT_DOUBLE = std::numeric_limits<int64_t>::max() / DoubleE4::SCALE_AS_DOUBLE;

QUARK_ALWAYS_INLINE SmallStrBuf double_to_string(double x) noexcept {
    double abs_x = (x > 0) ? x : -x;
    if (abs_x < E13_PIVOT) {
        return DoubleE13(x).to_string();
    } else if (abs_x < E9_PIVOT) {
        return DoubleE9(x).to_string();
    } else if (abs_x < E6_PIVOT) {
        return DoubleE6(x).to_string();
    } else if (abs_x < MAX_SUPPORT_DOUBLE) {
        return DoubleE4(x).to_string();
    } else {
        // try the best to use general conversion
        SmallStrBuf buf;
        auto buf_size = sizeof(buf.data_);
        auto result = std::to_chars(buf.data_, buf.data_ + buf_size - 1, x);
        if (result.ec == std::errc()) {
            *result.ptr = '\0';  // Null-terminate the string
        } else {
            // Handle conversion error
            std::strncpy(buf.data_, "NOT_SUPPORT", buf_size);
        }
        return buf;
    }
}

QUARK_ALWAYS_INLINE constexpr double round_double(double x) noexcept {
    double abs_x = (x > 0) ? x : -x;
    if (abs_x < E13_PIVOT) {
        return DoubleE13(x).as_double();
    } else if (abs_x < E9_PIVOT) {
        return DoubleE9(x).as_double();
    } else if (abs_x < E6_PIVOT) {
        return DoubleE6(x).as_double();
    } else {
        return DoubleE4(x).as_double();
    }
}

QUARK_ALWAYS_INLINE constexpr double round_double(double base_val, double x) noexcept {
    double abs_base_val = (base_val > 0) ? base_val : -base_val;
    if (abs_base_val < E13_PIVOT) {
        return DoubleE13(x).as_double();
    } else if (abs_base_val < E9_PIVOT) {
        return DoubleE9(x).as_double();
    } else if (abs_base_val < E6_PIVOT) {
        return DoubleE6(x).as_double();
    } else {
        return DoubleE4(x).as_double();
    }
}

QUARK_ALWAYS_INLINE double round_to_tick(double x, double tick) noexcept {
    double abs_x = (x > 0) ? x : -x;
    if (abs_x < E13_PIVOT) {
        return DoubleE13(x).round_to_tick(tick).as_double();
    } else if (abs_x < E9_PIVOT) {
        return DoubleE9(x).round_to_tick(tick).as_double();
    } else if (abs_x < E6_PIVOT) {
        return DoubleE6(x).round_to_tick(tick).as_double();
    } else {
        return DoubleE4(x).round_to_tick(tick).as_double();
    }
}

// {raw_value, SCALE}
QUARK_ALWAYS_INLINE std::pair<int64_t, int> round_double_get_raw(double x) noexcept {
    double abs_x = (x > 0) ? x : -x;
    if (abs_x < E13_PIVOT) {
        return std::make_pair(DoubleE13(x).get_raw(), DoubleE13::get_scale());
    } else if (abs_x < E9_PIVOT) {
        return std::make_pair(DoubleE9(x).get_raw(), DoubleE9::get_scale());
    } else if (abs_x < E6_PIVOT) {
        return std::make_pair(DoubleE6(x).get_raw(), DoubleE6::get_scale());
    } else {
        return std::make_pair(DoubleE4(x).get_raw(), DoubleE4::get_scale());
    }
}

// this is very strict, for small numbers, will look into 13th after decimal.
QUARK_ALWAYS_INLINE bool is_equal(double x, double y) noexcept {
    return round_double_get_raw(x) == round_double_get_raw(y);
}

QUARK_ALWAYS_INLINE constexpr bool is_equal(double x, double y, double epsilon) noexcept {
    return ((x - y) < epsilon) && ((y - x) < epsilon);
}

} // namespace math

QUARK_END_NAMESPACE
