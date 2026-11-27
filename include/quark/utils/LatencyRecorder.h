#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkLog.h"
#include "quark/utils/LatencyHistogram.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

QUARK_BEGIN_NAMESPACE

/// Latency tracepoints: one histogram per named stage, e.g. {"md_recv_to_strategy",
/// "request_to_wire", "order_to_ack"}. record() is a few instructions and never allocates;
/// report() logs each stage's count and percentiles (in microseconds) and starts over. Single
/// threaded: one recorder per thread, reported from that thread's timer.
template <std::size_t N>
class LatencyRecorder {
public:
    explicit LatencyRecorder(std::array<std::string_view, N> names) : names_{names} {}

    QUARK_ALWAYS_INLINE void record(std::size_t stage, int64_t ns) noexcept { histograms_[stage].record(ns); }

    [[nodiscard]] const LatencyHistogram& histogram(std::size_t stage) const noexcept { return histograms_[stage]; }

    // `who` only goes to the log: unused with QUARK_ENABLE_LOGGER=OFF
    QUARK_ATTRIBUTE_COLD void report([[maybe_unused]] std::string_view who) {
        for (std::size_t i = 0; i < N; ++i) {
            LatencyHistogram& h = histograms_[i];
            if (h.count() == 0) continue;
            QUARK_LOG_INFO("latency {} {}: n={} p50={:.1f} p99={:.1f} p99.9={:.1f} max={:.1f} us", who, names_[i], h.count(),
                           us(h.percentile(50)), us(h.percentile(99)), us(h.percentile(99.9)), us(h.max()));
            if (h.negative_count() != 0) [[unlikely]] {
                QUARK_LOG_WARN("latency {} {}: {} negative samples recorded as 0 (stamps from different clocks?)", who,
                               names_[i], h.negative_count());
            }
            h.reset();
        }
    }

private:
    std::array<std::string_view, N> names_;
    std::array<LatencyHistogram, N> histograms_{};

    static double us(int64_t ns) { return static_cast<double>(ns) / 1000.0; }
};

QUARK_END_NAMESPACE
