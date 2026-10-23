#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkLog.h"
#include "quark/utils/InplaceFunction.h"
#include "quark/utils/RdtscClock.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

QUARK_BEGIN_NAMESPACE

/// Opaque timer handle returned by TimerQueue::add_task(). `uid` is unique for the
/// lifetime of the queue (64-bit, never reused), so a stale id can never alias a newer
/// timer that happens to occupy the same slot.
struct TimerId {
    uint64_t uid{0};
    uint32_t slot{0};

    explicit operator bool() const noexcept { return uid != 0; }
    auto operator<=>(const TimerId&) const = default;
};
inline constexpr TimerId kInvalidTimerId{};

/// Single-threaded timer queue meant to be polled from a busy-poll event loop.
///
/// - poll() is a single compare against the cached next expiry when nothing is due.
/// - Indexed binary min-heap of {when, seq, slot}: comparisons never touch the task
///   nodes, and cancel() is O(log n) without leaving tombstones behind.
/// - Task nodes live in fixed-size chunks recycled through a free list: no allocation
///   in steady state (call reserve() up front), and node addresses stay stable so
///   callbacks may add/cancel timers freely.
/// - Tasks due at the same time fire in scheduling order (FIFO).
/// - Repeating tasks are fixed-rate: the next fire time is when + k * interval, the
///   first such slot strictly after now. Ticks missed by a late poll are skipped,
///   not replayed in a burst, and the schedule does not drift.
///
/// Callbacks return true to keep a repeating task alive, false to stop it (ignored for
/// one-shot tasks). Callbacks must not throw. They are stored inline (no allocation): a callable
/// larger than kCallbackCapacity bytes, e.g. a lambda capturing too much by value, fails to compile.
class TimerQueue {
public:
    static constexpr std::size_t kCallbackCapacity = 64;
    using Callback = InplaceFunction<bool(), kCallbackCapacity>;

    static constexpr int64_t MIN_REPEAT_INTERVAL_NS = 2'000;  // 2us

    TimerQueue() {
        // RdtscClock calibrates on first use (~20ms): pay that here, not in the first poll()
        (void)RdtscClock::tscns();
    }

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    void set_slow_task_threshold(int64_t interval_ns) noexcept { slow_task_threshold_ns_ = interval_ns; }

    /// Pre-allocates room for `n` live timers so add_task() does not allocate on the hot path.
    void reserve(uint32_t n) {
        heap_.reserve(n);
        while (slot_capacity() < n) {
            chunks_.push_back(std::make_unique<Node[]>(kChunkSize));
        }
    }

    /// Schedules `cb` at absolute time `when` (ns), repeating every `repeat_interval_ns`
    /// if given. Returns kInvalidTimerId if the arguments are rejected.
    TimerId add_task(std::string name, Callback cb, int64_t when,
                     std::optional<int64_t> repeat_interval_ns = {}) {
        if (when < 0) [[unlikely]] {
            QUARK_LOG_ERROR("bad timeTask:{}, when:{} < 0", name, when);
            return kInvalidTimerId;
        }
        if (!cb) [[unlikely]] {
            QUARK_LOG_ERROR("bad timeTask:{}, empty callback", name);
            return kInvalidTimerId;
        }
        if (repeat_interval_ns) {
            if (*repeat_interval_ns < MIN_REPEAT_INTERVAL_NS) [[unlikely]] {
                QUARK_LOG_ERROR("bad timeTask:{}, repeat_interval_ns:{} < {}", name, *repeat_interval_ns,
                                MIN_REPEAT_INTERVAL_NS);
                return kInvalidTimerId;
            }
            if (when > std::numeric_limits<int64_t>::max() - *repeat_interval_ns) [[unlikely]] {
                QUARK_LOG_ERROR("bad timeTask:{}, when:{} + repeat_interval_ns:{} overflows", name, when,
                                *repeat_interval_ns);
                return kInvalidTimerId;
            }
        }

        const uint32_t slot = alloc_slot();
        Node& n = node(slot);
        n.cb = std::move(cb);
        n.interval_ns = repeat_interval_ns.value_or(0);
        n.state = State::Pending;
        n.name = std::move(name);
        n.uid = next_uid_++;
        heap_push(Entry{when, seq_++, slot});
        return TimerId{n.uid, slot};
    }

    /// Cancels a pending timer. Safe to call from any callback, including the timer's own.
    /// Returns false if the id is unknown, already fired (one-shot) or already cancelled.
    bool cancel(TimerId id) noexcept {
        if (!id || id.slot >= num_slots_) return false;
        Node& n = node(id.slot);
        if (n.uid != id.uid) return false;

        switch (n.state) {
            case State::Pending:
                heap_remove(n.heap_idx);
                free_slot(id.slot);
                return true;
            case State::Running:
                // the callback is executing: poll() frees the node once it returns
                n.state = State::Cancelled;
                return true;
            default:
                return false;
        }
    }

    /// Runs every task due at `now_ns`. Returns the number of callbacks executed.
    QUARK_ALWAYS_INLINE int poll(int64_t now_ns) {
        if (now_ns < next_expiry_ns_) [[likely]] return 0;
        return poll_expired(now_ns);
    }

    /// Fire time of the earliest pending task, INT64_MAX if there is none.
    int64_t next_expiry_ns() const noexcept { return next_expiry_ns_; }

    size_t size() const noexcept { return heap_.size(); }
    bool empty() const noexcept { return heap_.empty(); }

private:
    static constexpr uint32_t kNpos = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t kChunkBits = 8;
    static constexpr uint32_t kChunkSize = 1u << kChunkBits;

    enum class State : uint8_t { Free, Pending, Running, Cancelled };

    struct Entry {
        int64_t when;
        uint64_t seq;  // FIFO tie-break for equal `when`
        uint32_t slot;
    };

    struct Node {
        Callback cb;
        int64_t interval_ns{0};  // 0 => one-shot
        uint32_t heap_idx{kNpos};
        uint64_t uid{0};  // 0 while free
        uint32_t next_free{kNpos};
        State state{State::Free};
        std::string name;
    };

    // hot: read on every poll()
    int64_t next_expiry_ns_{std::numeric_limits<int64_t>::max()};
    std::vector<Entry> heap_;
    std::vector<std::unique_ptr<Node[]>> chunks_;
    uint64_t seq_{0};
    uint64_t next_uid_{1};
    uint32_t num_slots_{0};
    uint32_t free_head_{kNpos};
    int64_t slow_task_threshold_ns_{200 * 1000};  // 200us
#ifndef NDEBUG
    bool polling_{false};
#endif

    QUARK_NOINLINE int poll_expired(int64_t now_ns) {
#ifndef NDEBUG
        assert(!polling_ && "TimerQueue::poll() is not reentrant");
        polling_ = true;
#endif
        int ret = 0;
        while (!heap_.empty() && heap_[0].when <= now_ns) {
            const Entry top = heap_[0];
            Node& n = node(top.slot);  // stable: chunks never move
            const bool repeat = n.interval_ns > 0;

            if (repeat) {
                // reschedule in place (one sift-down instead of pop + push); the new fire
                // time is > now_ns, so the task cannot run twice in this poll
                heap_[0].when = next_fire_time(top.when, n.interval_ns, now_ns);
                heap_[0].seq = seq_++;
                sift_down(0);
                update_next_expiry();
            } else {
                heap_remove(0);
            }
            n.state = State::Running;

            const int64_t start_ns = RdtscClock::tscns();
            const bool keep = n.cb();
            const int64_t dur_ns = RdtscClock::tscns() - start_ns;
            ++ret;
            if (dur_ns >= slow_task_threshold_ns_) [[unlikely]] {
                QUARK_LOG_WARN("found slow timer task:{}, took:{} ns", n.name, dur_ns);
            }

            if (repeat && keep && n.state == State::Running) [[likely]] {
                n.state = State::Pending;
            } else {
                if (repeat) heap_remove(n.heap_idx);
                free_slot(top.slot);
            }
        }
        update_next_expiry();
#ifndef NDEBUG
        polling_ = false;
#endif
        return ret;
    }

    static int64_t next_fire_time(int64_t when, int64_t interval, int64_t now_ns) noexcept {
        int64_t next = when + interval;
        if (next <= now_ns) [[unlikely]] {
            // late poll: skip the missed ticks, stay on the original phase
            next += ((now_ns - next) / interval + 1) * interval;
        }
        return next;
    }

    // ---------------------------------------------------------------- node pool

    uint32_t slot_capacity() const noexcept { return static_cast<uint32_t>(chunks_.size()) << kChunkBits; }

    Node& node(uint32_t slot) noexcept { return chunks_[slot >> kChunkBits][slot & (kChunkSize - 1)]; }

    uint32_t alloc_slot() {
        if (free_head_ != kNpos) {
            const uint32_t slot = free_head_;
            free_head_ = node(slot).next_free;
            return slot;
        }
        if (num_slots_ == slot_capacity()) {
            chunks_.push_back(std::make_unique<Node[]>(kChunkSize));
        }
        return num_slots_++;
    }

    void free_slot(uint32_t slot) noexcept {
        Node& n = node(slot);
        n.cb = nullptr;  // release captured state now
        n.name.clear();
        n.heap_idx = kNpos;
        n.state = State::Free;
        n.uid = 0;  // invalidate outstanding ids
        n.next_free = free_head_;
        free_head_ = slot;
    }

    // ---------------------------------------------------------------- indexed heap

    static bool less(const Entry& a, const Entry& b) noexcept {
        // seq is 64-bit: at ~1e7 increments/s it would take ~58k years to wrap
        return a.when < b.when || (a.when == b.when && a.seq < b.seq);
    }

    void place(uint32_t i, const Entry& e) noexcept {
        heap_[i] = e;
        node(e.slot).heap_idx = i;
    }

    void update_next_expiry() noexcept {
        next_expiry_ns_ = heap_.empty() ? std::numeric_limits<int64_t>::max() : heap_[0].when;
    }

    void sift_up(uint32_t i) noexcept {
        const Entry e = heap_[i];
        while (i > 0) {
            const uint32_t parent = (i - 1) / 2;
            if (!less(e, heap_[parent])) break;
            place(i, heap_[parent]);
            i = parent;
        }
        place(i, e);
    }

    void sift_down(uint32_t i) noexcept {
        const Entry e = heap_[i];
        const uint32_t size = static_cast<uint32_t>(heap_.size());
        while (true) {
            uint32_t child = 2 * i + 1;
            if (child >= size) break;
            if (child + 1 < size && less(heap_[child + 1], heap_[child])) ++child;
            if (!less(heap_[child], e)) break;
            place(i, heap_[child]);
            i = child;
        }
        place(i, e);
    }

    void heap_push(const Entry& e) {
        heap_.push_back(e);
        sift_up(static_cast<uint32_t>(heap_.size() - 1));
        update_next_expiry();
    }

    void heap_remove(uint32_t i) noexcept {
        assert(i < heap_.size());
        node(heap_[i].slot).heap_idx = kNpos;
        const Entry last = heap_.back();
        heap_.pop_back();
        if (i < heap_.size()) {
            place(i, last);
            if (i > 0 && less(last, heap_[(i - 1) / 2])) {
                sift_up(i);
            } else {
                sift_down(i);
            }
        }
        update_next_expiry();
    }
};

QUARK_END_NAMESPACE
