#include "quark/utils/RdtscClock.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>

using namespace quark;

namespace {

// Synthetic clocks for SteadyTscClock: 1 TSC tick = 1 ns, a sample per second. `real` is the
// system clock (it may step), `mono` is CLOCK_MONOTONIC (it never steps).
struct Sim {
    detail::SteadyTscClock clock;
    int64_t tsc{1'000'000};
    int64_t real{1'764'000'000'000'000'000};  // 2025-11-24
    int64_t mono{5'000'000'000};

    Sim() { clock.reset(tsc, real, mono, 1.0); }

    // advances every clock by `ns` (mono at `mono_rate` ns per tick) and feeds one sample
    void second(int64_t ns = 1'000'000'000, double mono_rate = 1.0) {
        tsc += ns;
        real += static_cast<int64_t>(static_cast<double>(ns) * mono_rate);
        mono += static_cast<int64_t>(static_cast<double>(ns) * mono_rate);
        clock.update(tsc, real, mono);
    }
    int64_t now() const { return clock.to_ns(tsc); }
};

}  // namespace

TEST(TestSteadyTscClock, TracksTheWallClock) {
    Sim s;
    EXPECT_EQ(s.now(), s.real);
    for (int i = 0; i < 60; ++i) s.second();
    EXPECT_LE(std::llabs(s.now() - s.real), 1'000);
}

// TIME-01 / NET-03: tscns() follows a system clock step within a calibration, and so did every
// EventLoop deadline (mass reconnects forward, blind liveness checks backward). The steady clock
// never jumps: it catches up at most kMaxSlew faster or slower.
TEST(TestSteadyTscClock, ForwardStepIsCaughtUpGradually) {
    Sim s;
    for (int i = 0; i < 10; ++i) s.second();
    s.real += 30'000'000'000;  // the system clock steps 30 s forward
    int64_t prev = s.now();
    int seconds = 0;
    while (std::llabs(s.now() - s.real) > 1'000 && seconds < 100'000) {
        const int64_t before = s.clock.to_ns(s.tsc);
        s.second();
        ++seconds;
        const int64_t elapsed = s.now() - prev;
        // a second of real time never measures more than (1 + kMaxSlew) s: no jump
        EXPECT_LE(elapsed, static_cast<int64_t>(1e9 * (1 + detail::SteadyTscClock::kMaxSlew)) + 2) << seconds;
        EXPECT_GE(elapsed, 1'000'000'000 - 2) << seconds;  // and catching up never slows it down
        EXPECT_GE(s.now(), before);
        prev = s.now();
    }
    // ~29.95 s at 0.5% (5990 s), then the offset decays by 1/10 per second: ~100 s more to 1 us
    EXPECT_GE(seconds, 5990);
    EXPECT_LT(seconds, 6200);
}

TEST(TestSteadyTscClock, BackwardStepNeverGoesBack) {
    Sim s;
    for (int i = 0; i < 10; ++i) s.second();
    s.real -= 30'000'000'000;  // 30 s back
    int64_t prev = s.now();
    int seconds = 0;
    while (std::llabs(s.now() - s.real) > 1'000 && seconds < 100'000) {
        s.second();
        ++seconds;
        const int64_t elapsed = s.now() - prev;
        EXPECT_GE(elapsed, static_cast<int64_t>(1e9 * (1 - detail::SteadyTscClock::kMaxSlew)) - 2) << seconds;
        EXPECT_LE(elapsed, 1'000'000'000 + 2) << seconds;
        prev = s.now();
    }
    EXPECT_GE(seconds, 5990);
    EXPECT_LT(seconds, 6200);
}

// The rate comes from CLOCK_MONOTONIC: an NTP frequency correction (slew) is followed, like the system
// clock's rate, without any offset building up
TEST(TestSteadyTscClock, FollowsTheMonotonicRate) {
    Sim s;
    for (int i = 0; i < 200; ++i) s.second(1'000'000'000, 1.0002);  // the clock runs 200 ppm fast
    EXPECT_NEAR(s.clock.ns_per_tsc(), 1.0002, 1e-6);
    EXPECT_LE(std::llabs(s.now() - s.real), 1'000);
}

// A sample from a backend thread descheduled while it sampled (or a broken clock) must not bend the rate
TEST(TestSteadyTscClock, ImplausibleRateSamplesAreIgnored) {
    Sim s;
    for (int i = 0; i < 5; ++i) s.second();
    const double rate = s.clock.ns_per_tsc();
    s.tsc += 1'000'000'000;
    s.real += 1'100'000'000;  // monotonic says 10% more time passed than the TSC: not a plausible rate
    s.mono += 1'100'000'000;
    s.clock.update(s.tsc, s.real, s.mono);
    EXPECT_NEAR(s.clock.ns_per_tsc(), rate, 1e-3);
}

TEST(TestRdtscClock, SteadyClockIsMonotonicAndCloseToTheWallClock) {
    int64_t prev = RdtscClock::steady_ns();
    for (int i = 0; i < 1'000'000; ++i) {
        const int64_t now = RdtscClock::steady_ns();
        ASSERT_GE(now, prev);
        prev = now;
    }
    EXPECT_LE(std::llabs(RdtscClock::steady_ns() - RdtscClock::sysns()), 1'000'000);  // within 1 ms of the system clock
}

// One thread calibrates (the log backend's role), others read: the steady clock's parameters are published
// through atomics (TSan checks it), and readers never see it go back.
TEST(TestRdtscClock, CalibrationWhileReadingFromOtherThreads) {
    std::atomic<bool> stop{false};
    std::thread calibrator([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            RdtscClock::calibrate();
            std::this_thread::yield();
        }
    });
    std::atomic<int> backwards{0};
    auto reader = [&] {
        int64_t prev = RdtscClock::steady_ns();
        for (int i = 0; i < 500'000; ++i) {
            const int64_t now = RdtscClock::steady_ns();
            if (now < prev) backwards.fetch_add(1, std::memory_order_relaxed);
            prev = now;
        }
    };
    std::thread r1(reader), r2(reader);
    r1.join();
    r2.join();
    stop.store(true, std::memory_order_relaxed);
    calibrator.join();
    EXPECT_EQ(backwards.load(), 0);
}
