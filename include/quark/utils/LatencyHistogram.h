#pragma once

#include "quark/Attributes.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

QUARK_BEGIN_NAMESPACE

/// Fixed-size log-linear histogram of non-negative int64 samples, e.g. latencies as TSC deltas.
/// Each power of two is split into 2^SubBucketBits equal buckets; a percentile reports the upper
/// edge of the bucket holding the sample, so it is never below the true value and at most
/// 1/2^SubBucketBits above it (relative), and exact for small values:
///
///     SubBucketBits | worst relative error | exact below | memory
///     5 (default)   | 3.1%                 | 64          | 15 KiB
///     7             | 0.78%                | 256         | 58 KiB   e.g. per tick-to-trade stage
///     10            | 0.098%               | 2048        | 440 KiB
///
/// Choose the precision your timestamps can resolve: TSC stamps carry ~10 ns of noise, so 7 bits
/// already resolves below that up to ~1 us. record() is a few instructions and never allocates.
/// Single-threaded: keep one per thread (and per stage) and merge() them for reports.
template <int SubBucketBits>
    requires(SubBucketBits >= 2 && SubBucketBits <= 10)
class BasicLatencyHistogram {
public:
    static constexpr int kSubBucketBits = SubBucketBits;
    static constexpr int kSubBuckets = 1 << kSubBucketBits;
    static constexpr int kBuckets = (64 - kSubBucketBits) * kSubBuckets + kSubBuckets;

    void record(int64_t value) noexcept {
        if (value < 0) value = 0;
        ++counts_[index_of(static_cast<uint64_t>(value))];
        ++count_;
        sum_ += static_cast<double>(value);
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
    }

    /// The smallest bucket upper bound below which `p` (0..100) percent of the samples fall,
    /// clamped to the exact maximum. 0 when empty.
    [[nodiscard]] int64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        if (p <= 0) return min_;
        const auto rank = static_cast<uint64_t>(p >= 100 ? static_cast<double>(count_)
                                                         : static_cast<double>(count_) * p / 100.0 + 0.5);
        uint64_t seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank && counts_[i] != 0) {
                const int64_t upper = upper_bound_of(i);
                return upper < max_ ? upper : max_;
            }
        }
        return max_;
    }

    [[nodiscard]] uint64_t count() const noexcept { return count_; }
    [[nodiscard]] int64_t min() const noexcept { return count_ ? min_ : 0; }
    [[nodiscard]] int64_t max() const noexcept { return count_ ? max_ : 0; }
    [[nodiscard]] double mean() const noexcept { return count_ ? sum_ / static_cast<double>(count_) : 0; }

    void merge(const BasicLatencyHistogram& other) noexcept {
        for (int i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
        count_ += other.count_;
        sum_ += other.sum_;
        if (other.count_) {
            if (other.min_ < min_) min_ = other.min_;
            if (other.max_ > max_) max_ = other.max_;
        }
    }

    void reset() noexcept { *this = BasicLatencyHistogram{}; }

private:
    std::array<uint64_t, kBuckets> counts_{};
    uint64_t count_{0};
    double sum_{0};
    int64_t min_{std::numeric_limits<int64_t>::max()};
    int64_t max_{0};

    // values below 2 * kSubBuckets map 1:1; above, the top kSubBucketBits + 1 bits select the bucket
    static constexpr int index_of(uint64_t v) noexcept {
        if (v < 2 * kSubBuckets) return static_cast<int>(v);
        const int shift = std::bit_width(v) - 1 - kSubBucketBits;  // >= 1
        return (shift + 1) * kSubBuckets + static_cast<int>((v >> shift) & (kSubBuckets - 1));
    }

    static constexpr int64_t upper_bound_of(int index) noexcept {
        if (index < 2 * kSubBuckets) return index;
        const int shift = index / kSubBuckets - 1;
        const uint64_t base = (static_cast<uint64_t>(kSubBuckets) + static_cast<uint64_t>(index % kSubBuckets)) << shift;
        const uint64_t upper = base + (uint64_t{1} << shift) - 1;
        return upper > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? std::numeric_limits<int64_t>::max()
                                                                                    : static_cast<int64_t>(upper);
    }
};

/// 3.1% precision, 15 KiB: see BasicLatencyHistogram for finer settings.
using LatencyHistogram = BasicLatencyHistogram<5>;

QUARK_END_NAMESPACE
