#pragma once

#include "quark/Attributes.h"
#include "quark/utils/RdtscClock.h"

#include <cassert>
#include <cstdint>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Exponential backoff for reconnects: initial, 2x initial, 4x initial, ... capped at `max_ns`,
/// each spread by a random +/- `jitter` fraction so many clients (or many sessions of one client)
/// that dropped together don't reconnect in lockstep. Delays never exceed `max_ns`: near the cap
/// the spread is [base * (1 - jitter), max_ns].
///
/// Call reset() only once a connection has proven healthy (e.g. it stayed up for a while), not
/// as soon as it opens: a server that accepts and immediately closes would otherwise be retried
/// at the initial delay forever.
class ExpBackoff {
public:
    /// @param jitter fraction in [0, 1): 0.2 spreads each delay uniformly over [0.8x, 1.2x]
    ExpBackoff(int64_t initial_ns, int64_t max_ns, double jitter = 0.2,
               uint64_t seed = static_cast<uint64_t>(RdtscClock::rdtsc())) noexcept
        : initial_ns_{initial_ns}, max_ns_{max_ns}, jitter_{jitter}, rng_state_{seed} {
        assert(initial_ns > 0 && max_ns >= initial_ns && jitter >= 0 && jitter < 1);
    }

    /// Delay to wait before the next attempt; grows the base delay for the one after.
    int64_t next_delay_ns() noexcept {
        base_ns_ = base_ns_ == 0 ? initial_ns_ : (base_ns_ > max_ns_ / 2 ? max_ns_ : base_ns_ * 2);
        ++attempts_;
        if (jitter_ <= 0.0) return base_ns_;
        // uniform in [base * (1 - jitter), min(base * (1 + jitter), max)), computed in double so
        // nothing overflows near INT64_MAX
        const double u = static_cast<double>(next_random() >> 11) * 0x1.0p-53;
        const double base = static_cast<double>(base_ns_);
        const double max = static_cast<double>(max_ns_);
        const double lo = base * (1.0 - jitter_);
        const double hi = base * (1.0 + jitter_) < max ? base * (1.0 + jitter_) : max;
        const double d = lo + (hi - lo) * u;
        // double(max_ns) may round up past it (and past INT64_MAX): compare before converting
        return d >= max ? max_ns_ : (d < 1.0 ? 1 : static_cast<int64_t>(d));
    }

    /// Back to the initial delay.
    void reset() noexcept {
        base_ns_ = 0;
        attempts_ = 0;
    }

    /// Attempts since the last reset().
    [[nodiscard]] uint32_t attempts() const noexcept { return attempts_; }

private:
    int64_t initial_ns_;
    int64_t max_ns_;
    double jitter_;
    uint64_t rng_state_;
    int64_t base_ns_{0};
    uint32_t attempts_{0};

    // SplitMix64: cheap and good enough to decorrelate reconnects
    uint64_t next_random() noexcept {
        uint64_t z = (rng_state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

} // namespace net

QUARK_END_NAMESPACE
