#pragma once

#include "quark/Attributes.h"
#include "quark/third_party/tscns/tscns.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>

QUARK_BEGIN_NAMESPACE

namespace detail {

/// TSC ticks -> nanoseconds since the epoch, on a timebase that never jumps: its rate follows
/// CLOCK_MONOTONIC (which never steps), and its offset from the system clock is worked off at most
/// kMaxSlew faster or slower (over about kConvergeNs once small). Normally within a fraction of a
/// microsecond of the system clock; after a system clock step it converges instead of jumping.
///
/// update() from one thread (the calibrating one); to_ns() from any thread.
class SteadyTscClock {
public:
    static constexpr double kMaxSlew = 0.005;               ///< at most 0.5% fast or slow while catching up
    static constexpr int64_t kConvergeNs = 10'000'000'000;  ///< a small offset is removed over ~10 s
    static constexpr double kMaxRateChange = 0.01;          ///< samples implying a larger rate change are ignored

    /// Starts at `real_ns` for `tsc`, at `ns_per_tsc` until the first update().
    void reset(int64_t tsc, int64_t real_ns, int64_t mono_ns, double ns_per_tsc) noexcept {
        last_tsc_ = tsc;
        last_mono_ = mono_ns;
        mono_ns_per_tsc_ = ns_per_tsc;
        publish(tsc, real_ns, ns_per_tsc);
    }

    /// A (TSC, system clock, CLOCK_MONOTONIC) sample, taken together.
    void update(int64_t tsc, int64_t real_ns, int64_t mono_ns) noexcept {
        if (tsc <= last_tsc_) return;
        const double rate = static_cast<double>(mono_ns - last_mono_) / static_cast<double>(tsc - last_tsc_);
        last_tsc_ = tsc;
        last_mono_ = mono_ns;
        // a thread descheduled between the samples, or a clock gone wrong: keep the previous rate
        if (!(std::fabs(rate / mono_ns_per_tsc_ - 1.0) < kMaxRateChange)) return;
        mono_ns_per_tsc_ = rate;
        const int64_t now = to_ns(tsc);  // rebased here: continuous
        offset_ns_ = now - real_ns;      // > 0: ahead of the system clock
        const double correction = std::clamp(-static_cast<double>(offset_ns_) / static_cast<double>(kConvergeNs),
                                             -kMaxSlew, kMaxSlew);
        publish(tsc, now, rate * (1.0 + correction));
    }

    [[nodiscard]] QUARK_ALWAYS_INLINE int64_t to_ns(int64_t tsc) const noexcept {
        for (;;) {
            const uint32_t before = seq_.load(std::memory_order_acquire);
            const int64_t base_tsc = base_tsc_.load(std::memory_order_relaxed);
            const int64_t base_ns = base_ns_.load(std::memory_order_relaxed);
            const double ns_per_tsc = ns_per_tsc_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((before & 1u) == 0 && seq_.load(std::memory_order_relaxed) == before) [[likely]] {
                return base_ns + static_cast<int64_t>(static_cast<double>(tsc - base_tsc) * ns_per_tsc);
            }
        }
    }

    /// The rate in use (ns per tick).
    [[nodiscard]] double ns_per_tsc() const noexcept { return ns_per_tsc_.load(std::memory_order_relaxed); }
    /// Offset from the system clock at the last update (the updating thread's view).
    [[nodiscard]] int64_t offset_ns() const noexcept { return offset_ns_; }

private:
    // seqlock: odd while the writer updates (atomics throughout, so it is race-free, not just on x86)
    std::atomic<uint32_t> seq_{0};
    std::atomic<int64_t> base_tsc_{0};
    std::atomic<int64_t> base_ns_{0};
    std::atomic<double> ns_per_tsc_{1.0};
    // updating thread only
    int64_t last_tsc_{0};
    int64_t last_mono_{0};
    double mono_ns_per_tsc_{1.0};
    int64_t offset_ns_{0};

    void publish(int64_t tsc, int64_t ns, double ns_per_tsc) noexcept {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        base_tsc_.store(tsc, std::memory_order_relaxed);
        base_ns_.store(ns, std::memory_order_relaxed);
        ns_per_tsc_.store(ns_per_tsc, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }
};

}  // namespace detail

// rdtsc(): raw TSC ticks. tscns(): nanoseconds since the epoch from the TSC, ~10x cheaper than
// the system clock; the first call blocks ~20 ms to calibrate. sysns(): the system clock itself
// (std::chrono::system_clock), the source of truth for wall time.
//
// steady_ns(): like tscns() (nanoseconds since the epoch, same cost), but it never jumps: a step of the
// system clock (NTP, date -s) is caught up gradually, at most 0.5% faster or slower, while tscns()
// follows it within a calibration. Use it for deadlines, timeouts and intervals (EventLoop::now_ns()
// is steady_ns()); use tscns() for timestamps that must match wall time to the microsecond (exchange
// request times, latency against exchange timestamps).
//
// Both track the system clock only if calibrate() runs regularly, from a single thread. quark's log
// backend does it (quill's backend thread, or the thread running LogPoller) once init_log() has run; a
// process without quark logging must call calibrate() itself, e.g. once a second from a housekeeping
// thread, and never from two threads. Most calls return after one rdtsc(): tscns() recalibrates every
// 3 s, steady_ns() every second, each in well under a microsecond.
class QUARK_API RdtscClock
{
public:
    RdtscClock(const RdtscClock&) = delete;
    RdtscClock& operator=(const RdtscClock&) = delete;

    static QUARK_ALWAYS_INLINE int64_t rdtsc() {
        return TSCNS::rdtsc();
    }

    static QUARK_ALWAYS_INLINE int64_t tscns() {
        return instance().tsc_clock_.rdns();
    }

    static QUARK_ALWAYS_INLINE int64_t steady_ns() {
        return instance().steady_.to_ns(TSCNS::rdtsc());
    }

    static int64_t sysns() {
        return TSCNS::rdsysns();
    }

    static void calibrate() {
        RdtscClock& c = instance();
        c.tsc_clock_.calibrate();
        if (TSCNS::rdtsc() >= c.next_steady_tsc_) c.calibrate_steady();
    }
private:
    static constexpr int64_t kSteadyIntervalNs = 1'000'000'000;

    TSCNS tsc_clock_;
    detail::SteadyTscClock steady_;
    int64_t next_steady_tsc_{0};  // calibrating thread only
    double ticks_per_ns_{1.0};

    RdtscClock() {
        tsc_clock_.init();
        ticks_per_ns_ = tsc_clock_.getTscGhz();
        int64_t tsc = 0, real = 0, mono = 0;
        sample(tsc, real, mono);
        steady_.reset(tsc, real, mono, 1.0 / ticks_per_ns_);
        next_steady_tsc_ = tsc + static_cast<int64_t>(static_cast<double>(kSteadyIntervalNs) * ticks_per_ns_);
    }

    void calibrate_steady() noexcept {
        int64_t tsc = 0, real = 0, mono = 0;
        sample(tsc, real, mono);
        steady_.update(tsc, real, mono);
        next_steady_tsc_ = tsc + static_cast<int64_t>(static_cast<double>(kSteadyIntervalNs) * ticks_per_ns_);
    }

    // the TSC with both clocks read between two rdtsc(): the tightest of three tries
    static void sample(int64_t& tsc, int64_t& real, int64_t& mono) noexcept {
        int64_t best = std::numeric_limits<int64_t>::max();
        for (int i = 0; i < 3; ++i) {
            timespec r{}, m{};
            const int64_t t0 = TSCNS::rdtsc();
            ::clock_gettime(CLOCK_REALTIME, &r);
            ::clock_gettime(CLOCK_MONOTONIC, &m);
            const int64_t t1 = TSCNS::rdtsc();
            if (t1 - t0 < best) {
                best = t1 - t0;
                tsc = t0 + (t1 - t0) / 2;
                real = r.tv_sec * 1'000'000'000LL + r.tv_nsec;
                mono = m.tv_sec * 1'000'000'000LL + m.tv_nsec;
            }
        }
    }

    static RdtscClock& instance() {
        static RdtscClock inst;
        return inst;
    }
};

QUARK_END_NAMESPACE
