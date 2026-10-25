#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkError.h"
#include "quark/net/Poller.h"
#include "quark/utils/InplaceFunction.h"
#include "quark/utils/RdtscClock.h"
#include "quark/utils/TimerQueue.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(QUARK_USE_FSTACK)
    #include "quark/net/FStack.h"

    #include <ff_api.h>
#endif

QUARK_BEGIN_NAMESPACE

namespace net {

/// Something the loop drives: a connection registers while it has an open socket.
class QUARK_API Pollable {
public:
    virtual ~Pollable() = default;
    [[nodiscard]] virtual int fd() const noexcept = 0;
    /// Busy-poll mode: called every step with flags 0, the callee tries its nonblocking I/O.
    /// Poller mode: called on readiness edges with ReadyFlags; the callee must drain its socket.
    virtual void on_ready(uint32_t flags) = 0;
};

struct EventLoopConfig {
    /// true: every step polls every registered connection (lowest latency, one recv() per
    /// connection per step on kernel sockets); false: one epoll_wait() per step reports the
    /// ready ones (cheaper with many mostly idle connections).
    bool busy_poll{true};
    int max_events{64};          ///< poller mode: events handled per step
    int64_t poll_timeout_ns{0};  ///< poller mode: how long a step may block, 0 == spin
    uint32_t max_timers{256};    ///< reserved TimerQueue capacity
};

/// Single-threaded reactor: one per core, nothing shared, no locks. Everything registered with
/// it, and every call into it, runs on the thread that calls step()/run().
///
/// Each step(): runs the step hook (e.g. drains a queue of work posted by other threads), drives
/// the connections, then fires due timers. With kernel sockets step() can also be called from an
/// existing loop instead of run().
///
/// F-Stack builds (QUARK_USE_FSTACK): packets are only processed inside F-Stack's own loop, so
/// run() is the entry point (it calls step() once per F-Stack iteration) and application work
/// belongs in the step hook. F-Stack tears DPDK down when that loop ends: run() works once per
/// process, on the thread that called init_fstack().
class EventLoop {
public:
    using StepHook = InplaceFunction<void(), 64>;

    explicit EventLoop(const EventLoopConfig& cfg = {}) : cfg_{cfg} {
        if (!cfg_.busy_poll && !poller_.valid()) {
            QUARK_THROW(QuarkError{"EventLoop: epoll_create1 failed"});
        }
        timers_.reserve(cfg_.max_timers);
        now_ns_ = RdtscClock::tscns();
    }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /// Starts driving `p` (its fd must be open). Not a hot-path call.
    bool add(Pollable& p, bool want_write) {
        if (!cfg_.busy_poll && !poller_.add(p.fd(), &p, want_write)) return false;
        pollables_.push_back(&p);
        return true;
    }

    /// Stops driving `p`; call it before closing p's fd. Safe from inside p.on_ready().
    void remove(Pollable& p) noexcept {
        if (!cfg_.busy_poll) {
            poller_.remove(p.fd());
            // drop events of `p` still pending in the batch being dispatched
            for (int j = dispatch_idx_ + 1; j < dispatch_count_; ++j) {
                if (events_[j].token == &p) events_[j].token = nullptr;
            }
        }
        for (auto& q : pollables_) {
            if (q == &p) {
                q = nullptr;  // swept after the step, keeps dispatch indexes valid
                removed_ = true;
            }
        }
    }

    /// Poller mode: (un)subscribes `p` from writable edges. Callers track the current state
    /// and only call this when it changes. No-op in busy-poll mode.
    void set_write_interest(Pollable& p, bool want_write) noexcept {
        if (!cfg_.busy_poll) poller_.modify(p.fd(), &p, want_write);
    }

    /// Timers run on the loop thread at the end of each step, against now_ns().
    [[nodiscard]] TimerQueue& timers() noexcept { return timers_; }

    /// TSC time (ns) taken at the start of the current step: cheap "now" for timeouts.
    [[nodiscard]] int64_t now_ns() const noexcept { return now_ns_; }

    void set_step_hook(StepHook hook) { step_hook_ = std::move(hook); }

    void step() {
        now_ns_ = RdtscClock::tscns();
        if (step_hook_) step_hook_();
        if (cfg_.busy_poll) {
            for (std::size_t i = 0; i < pollables_.size(); ++i) {  // may grow while iterating
                if (Pollable* p = pollables_[i]) p->on_ready(0);
            }
        } else {
            dispatch_count_ = poller_.wait(events_, std::min(cfg_.max_events, Poller::kMaxBatch), cfg_.poll_timeout_ns);
            for (dispatch_idx_ = 0; dispatch_idx_ < dispatch_count_; ++dispatch_idx_) {
                if (auto* p = static_cast<Pollable*>(events_[dispatch_idx_].token)) p->on_ready(events_[dispatch_idx_].flags);
            }
            dispatch_idx_ = -1;
            dispatch_count_ = 0;
        }
        if (removed_) [[unlikely]] {
            std::erase(pollables_, nullptr);
            removed_ = false;
        }
        timers_.poll(now_ns_);  // at most one step late, saves a clock read per step
    }

    /// Calls step() until stop().
    void run() {
        running_.store(true, std::memory_order_relaxed);
#if defined(QUARK_USE_FSTACK)
        ::ff_run(&EventLoop::fstack_iteration, this);
        detail::set_fstack_active(false);  // DPDK is gone: sockets closed from now on skip F-Stack
#else
        while (running_.load(std::memory_order_relaxed)) step();
#endif
    }

    /// Makes run() return after the current step. Callable from any thread.
    void stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    [[nodiscard]] bool busy_poll() const noexcept { return cfg_.busy_poll; }
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(std::count_if(pollables_.begin(), pollables_.end(), [](auto* p) { return p != nullptr; }));
    }

private:
#if defined(QUARK_USE_FSTACK)
    static int fstack_iteration(void* arg) {
        auto* self = static_cast<EventLoop*>(arg);
        if (!self->running_.load(std::memory_order_relaxed)) {
            ::ff_stop_run();
            return 0;
        }
        self->step();
        return 0;
    }
#endif

    EventLoopConfig cfg_;
    Poller poller_;
    std::vector<Pollable*> pollables_;
    ReadyEvent events_[Poller::kMaxBatch];
    int dispatch_idx_{-1};
    int dispatch_count_{0};
    TimerQueue timers_;
    StepHook step_hook_;
    int64_t now_ns_{0};
    bool removed_{false};
    std::atomic<bool> running_{false};
};

} // namespace net

QUARK_END_NAMESPACE
