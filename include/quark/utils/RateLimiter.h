#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"
#include <limits>

#include <cassert>
#include <cstdint>
#include <vector>

QUARK_BEGIN_NAMESPACE

/// At most `limit` units per sliding window, e.g. an exchange's "300 orders per 10 s".
/// try_acquire() takes the units if the window still has room: the units taken in the last
/// `window_ns` never exceed `limit`. Time comes from the caller. Single-threaded; never allocates
/// after construction.
class SlidingWindowLimiter {
public:
    /// Throws QuarkError (fatal with QUARK_NO_EXCEPTIONS) for a window <= 0 or a limit of 0 or UINT32_MAX.
    SlidingWindowLimiter(int64_t window_ns, uint32_t limit)
        : window_ns_{checked_window(window_ns)}, limit_{checked_limit(limit)}, ring_(std::size_t{limit} + 1) {}

    /// Takes `units` at `now_ns` if they fit in the window; false (and nothing taken) otherwise.
    /// Taking 0 units always succeeds and takes nothing.
    bool try_acquire(int64_t now_ns, uint32_t units = 1) {
        if (units == 0) return true;
        expire(now_ns);
        // in 64 bits: used_ + units can't wrap past the check (used_ may exceed limit_ after resync())
        if (uint64_t{used_} + units > limit_ || count_ == ring_.size()) return false;
        push(now_ns, units);
        return true;
    }

    /// The exchange reports `used` units in its current window: count at least that many, so a
    /// limit shared with other clients (or a restart) isn't overrun. Never lowers the count.
    void resync(int64_t now_ns, uint32_t used) {
        expire(now_ns);
        if (used > used_ && count_ < ring_.size()) push(now_ns, used - used_);
    }

    /// Units taken in the window ending at `now_ns`.
    [[nodiscard]] uint32_t used(int64_t now_ns) {
        expire(now_ns);
        return used_;
    }

    [[nodiscard]] int64_t window_ns() const noexcept { return window_ns_; }
    [[nodiscard]] uint32_t limit() const noexcept { return limit_; }

private:
    struct Entry {
        int64_t ts;
        uint32_t units;
    };
    int64_t window_ns_;
    uint32_t limit_;
    std::vector<Entry> ring_;  // oldest at head_; every entry holds at least one unit
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    uint32_t used_ = 0;

    static int64_t checked_window(int64_t window_ns) {
        QUARK_ASSERT(window_ns > 0);
        return window_ns;
    }
    static uint32_t checked_limit(uint32_t limit) {
        QUARK_ASSERT(limit > 0 && limit < std::numeric_limits<uint32_t>::max());
        return limit;
    }

    void push(int64_t ts, uint32_t units) {
        ring_[(head_ + count_) % ring_.size()] = {ts, units};
        ++count_;
        used_ += units;
    }

    void expire(int64_t now_ns) {
        while (count_ > 0 && ring_[head_].ts <= now_ns - window_ns_) {
            used_ -= ring_[head_].units;
            head_ = (head_ + 1) % ring_.size();
            --count_;
        }
    }
};

QUARK_END_NAMESPACE
