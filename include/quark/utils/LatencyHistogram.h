#pragma once

#include "quark/Attributes.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

QUARK_BEGIN_NAMESPACE

/// Fixed-size log-linear histogram of non-negative int64 samples, e.g. latencies in ns from TSC
/// deltas. Each power of two is split into 32 equal sub-buckets, so a percentile is reported
/// within ~3% of the true sample value (exact below 64). record() is a few instructions, never
/// allocates; 16 KiB. Single-threaded: keep one per thread and merge() them for reports.
class LatencyHistogram {
public:
    static constexpr int kSubBucketBits = 5;
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

    void merge(const LatencyHistogram& other) noexcept {
        for (int i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
        count_ += other.count_;
        sum_ += other.sum_;
        if (other.count_) {
            if (other.min_ < min_) min_ = other.min_;
            if (other.max_ > max_) max_ = other.max_;
        }
    }

    void reset() noexcept { *this = LatencyHistogram{}; }

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

QUARK_END_NAMESPACE
