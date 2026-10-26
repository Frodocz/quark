#include "quark/utils/TimerQueue.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>
#include <new>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

// Counts heap allocations so tests can check the hot path doesn't allocate.
#if defined(__GNUC__) && !defined(__clang__)
    // GCC flags the malloc/free pairing of the replaced operators below as mismatched
    #pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
static std::atomic<long> g_heap_allocs{0};

// All non-aligned forms are replaced so every new/delete pair agrees on malloc/free (the aligned
// forms keep their default, self-consistent implementation).
static void* counted_malloc(std::size_t n) noexcept {
    g_heap_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}
void* operator new(std::size_t n) {
    if (void* p = counted_malloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = counted_malloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_malloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_malloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

class TestTimerQueue : public ::testing::Test {};

using namespace quark;

TEST_F(TestTimerQueue, TestTimerRepeat) {
    int counta = 0;
    auto fa = [&]() {
        counta++;
        return true;
    };

    int countb = 0;
    auto fb = [&]() {
        countb++;
        return true;
    };

    auto cba = std::function<bool()>(fa);
    auto cbb = std::function<bool()>(fb);

    TimerQueue tq;
    int64_t kNanoSecondInSec = 1000 * 1000 * 1000;
    int64_t nowNs = 1350000000 * kNanoSecondInSec;
    EXPECT_EQ(tq.poll(nowNs), 0);
    EXPECT_EQ(counta, 0);
    EXPECT_EQ(countb, 0);

    tq.add_task("a", cba, nowNs, kNanoSecondInSec);
    nowNs += 1.05 * kNanoSecondInSec;
    tq.poll(nowNs);
    EXPECT_EQ(counta, 1);
    EXPECT_EQ(countb, 0);

    tq.add_task("b", cbb, nowNs, 0.5 * kNanoSecondInSec);
    nowNs += 0.55 * kNanoSecondInSec;
    tq.poll(nowNs);
    EXPECT_EQ(counta, 1);
    EXPECT_EQ(countb, 1);

    nowNs += 0.55 * kNanoSecondInSec;
    tq.poll(nowNs);
    EXPECT_EQ(counta, 2);
    EXPECT_EQ(countb, 2);

    nowNs += 0.55 * kNanoSecondInSec;
    tq.poll(nowNs);
    EXPECT_EQ(counta, 2);
    EXPECT_EQ(countb, 3);
}

TEST_F(TestTimerQueue, TestBadTimerTask) {
    auto f = [&]() { return true; };
    TimerQueue tq;
    int64_t kNanoSecondInSec = 1000 * 1000 * 1000;
    int64_t nowNs = 1350000000 * kNanoSecondInSec;
    // when must > 0
    EXPECT_FALSE(tq.add_task("a", f, -1));
    // repeat interval must >= 2000
    EXPECT_FALSE(tq.add_task("b", f, nowNs, -1));
    EXPECT_FALSE(tq.add_task("c", f, nowNs, 1999));
    EXPECT_TRUE(tq.add_task("d", f, nowNs, 2000));
}

TEST_F(TestTimerQueue, TestTimerSeq) {
    std::vector<std::string> events;

    auto fa = [&]() {
        events.push_back("aaa");
        return true;
    };

    auto fb = [&]() {
        events.push_back("bbb");
        return true;
    };

    auto fc = [&]() {
        events.push_back("ccc");
        return true;
    };

    TimerQueue tq;
    int64_t kNanoSecondInSec = 1000 * 1000 * 1000;
    int64_t nowNs = 1350000000 * kNanoSecondInSec;

    tq.add_task("a", fa, nowNs + 900000);
    tq.add_task("b", fb, nowNs + 500000);
    tq.add_task("c", fc, nowNs + 500001);

    EXPECT_TRUE(events.empty());
    tq.poll(nowNs + 10000000);

    EXPECT_EQ(events.size(), 3);
    EXPECT_EQ(events[0], "bbb");
    EXPECT_EQ(events[1], "ccc");
    EXPECT_EQ(events[2], "aaa");
}

TEST_F(TestTimerQueue, TestRejectEmptyCallback) {
    TimerQueue tq;
    EXPECT_EQ(tq.add_task("a", nullptr, 100), kInvalidTimerId);
    EXPECT_TRUE(tq.empty());
}

TEST_F(TestTimerQueue, TestFifoForSameDeadline) {
    std::vector<int> events;
    TimerQueue tq;
    for (int i = 0; i < 100; ++i) {
        tq.add_task("t", [&events, i]() { events.push_back(i); return true; }, 1000);
    }
    EXPECT_EQ(tq.poll(1000), 100);
    ASSERT_EQ(events.size(), 100u);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(events[i], i);
}

TEST_F(TestTimerQueue, TestNextExpiry) {
    TimerQueue tq;
    EXPECT_EQ(tq.next_expiry_ns(), std::numeric_limits<int64_t>::max());
    auto f = []() { return true; };
    tq.add_task("a", f, 500);
    tq.add_task("b", f, 300);
    EXPECT_EQ(tq.next_expiry_ns(), 300);
    EXPECT_EQ(tq.poll(299), 0);
    EXPECT_EQ(tq.poll(300), 1);
    EXPECT_EQ(tq.next_expiry_ns(), 500);
    EXPECT_EQ(tq.poll(500), 1);
    EXPECT_EQ(tq.next_expiry_ns(), std::numeric_limits<int64_t>::max());
    EXPECT_TRUE(tq.empty());
}

TEST_F(TestTimerQueue, TestFixedRateNoDrift) {
    std::vector<int64_t> fired;
    int64_t now = 0;
    TimerQueue tq;
    tq.add_task("r", [&]() { fired.push_back(now); return true; }, 10'000, 10'000);

    // each poll is late by a different amount, the schedule must stay on the 10us grid
    for (int64_t t : {10'003, 20'500, 30'001, 40'999}) {
        now = t;
        EXPECT_EQ(tq.poll(now), 1);
    }
    EXPECT_EQ(tq.next_expiry_ns(), 50'000);
    EXPECT_EQ(fired.size(), 4u);
}

TEST_F(TestTimerQueue, TestRepeatSkipsMissedTicks) {
    int count = 0;
    TimerQueue tq;
    tq.add_task("r", [&]() { ++count; return true; }, 10'000, 10'000);
    // very late poll: one execution, not a burst for the missed ticks
    EXPECT_EQ(tq.poll(95'000), 1);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(tq.next_expiry_ns(), 100'000);
}

TEST_F(TestTimerQueue, TestRepeatStopsWhenCallbackReturnsFalse) {
    int count = 0;
    TimerQueue tq;
    tq.add_task("r", [&]() { return ++count < 3; }, 10'000, 10'000);
    for (int64_t t = 10'000; t <= 100'000; t += 10'000) tq.poll(t);
    EXPECT_EQ(count, 3);
    EXPECT_TRUE(tq.empty());
}

TEST_F(TestTimerQueue, TestCancel) {
    std::vector<std::string> events;
    TimerQueue tq;
    auto a = tq.add_task("a", [&]() { events.push_back("a"); return true; }, 100);
    auto b = tq.add_task("b", [&]() { events.push_back("b"); return true; }, 200);
    auto c = tq.add_task("c", [&]() { events.push_back("c"); return true; }, 300, 10'000);
    ASSERT_NE(a, kInvalidTimerId);
    EXPECT_EQ(tq.size(), 3u);

    EXPECT_TRUE(tq.cancel(b));
    EXPECT_FALSE(tq.cancel(b));  // already cancelled
    EXPECT_FALSE(tq.cancel(kInvalidTimerId));
    EXPECT_FALSE(tq.cancel(TimerId{12345678, 0}));
    EXPECT_EQ(tq.size(), 2u);

    EXPECT_EQ(tq.poll(1000), 2);
    EXPECT_EQ(events, (std::vector<std::string>{"a", "c"}));
    EXPECT_FALSE(tq.cancel(a));  // one-shot already fired
    EXPECT_TRUE(tq.cancel(c));   // repeating task
    EXPECT_TRUE(tq.empty());
    EXPECT_EQ(tq.poll(1'000'000), 0);
}

TEST_F(TestTimerQueue, TestStaleIdAfterSlotReuse) {
    TimerQueue tq;
    auto f = []() { return true; };
    auto a = tq.add_task("a", f, 100);
    EXPECT_TRUE(tq.cancel(a));
    auto b = tq.add_task("b", f, 100);  // reuses a's slot
    EXPECT_NE(a, b);
    EXPECT_FALSE(tq.cancel(a));
    EXPECT_EQ(tq.size(), 1u);
    EXPECT_TRUE(tq.cancel(b));
}

TEST_F(TestTimerQueue, TestCancelSelfFromCallback) {
    int count = 0;
    TimerQueue tq;
    TimerId self = kInvalidTimerId;
    self = tq.add_task("r", [&]() {
        ++count;
        EXPECT_TRUE(tq.cancel(self));
        return true;
    }, 10'000, 10'000);
    EXPECT_EQ(tq.poll(10'000), 1);
    EXPECT_TRUE(tq.empty());
    EXPECT_EQ(tq.poll(100'000), 0);
    EXPECT_EQ(count, 1);
}

TEST_F(TestTimerQueue, TestCancelOtherFromCallback) {
    std::vector<std::string> events;
    TimerQueue tq;
    TimerId b = kInvalidTimerId;
    tq.add_task("a", [&]() { events.push_back("a"); tq.cancel(b); return true; }, 100);
    b = tq.add_task("b", [&]() { events.push_back("b"); return true; }, 100);
    EXPECT_EQ(tq.poll(100), 1);
    EXPECT_EQ(events, (std::vector<std::string>{"a"}));
}

TEST_F(TestTimerQueue, TestAddFromCallback) {
    std::vector<int> events;
    TimerQueue tq;
    // grow well past one node chunk from inside a callback: running node must stay valid
    tq.add_task("a", [&]() {
        for (int i = 0; i < 1000; ++i) {
            tq.add_task("x", [&events, i]() { events.push_back(i); return true; }, 200 + i);
        }
        events.push_back(-1);
        return true;
    }, 100);
    EXPECT_EQ(tq.poll(100), 1);
    EXPECT_EQ(tq.size(), 1000u);
    EXPECT_EQ(tq.poll(10'000), 1000);
    ASSERT_EQ(events.size(), 1001u);
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(events[i + 1], i);
}

TEST_F(TestTimerQueue, TestRandomAgainstReference) {
    std::mt19937_64 rng(42);
    TimerQueue tq;
    tq.reserve(512);

    // reference: id -> (when, seq); expected fire order is by (when, seq)
    std::map<TimerId, std::pair<int64_t, uint64_t>> live;
    std::vector<TimerId> fired;
    uint64_t seq = 0;
    int64_t now = 0;

    for (int round = 0; round < 2000; ++round) {
        const int op = static_cast<int>(rng() % 10);
        if (op < 6) {
            const int64_t when = now + static_cast<int64_t>(rng() % 5000);
            auto holder = std::make_shared<TimerId>(kInvalidTimerId);
            const TimerId id = tq.add_task("t", [&fired, holder]() { fired.push_back(*holder); return true; }, when);
            ASSERT_NE(id, kInvalidTimerId);
            *holder = id;
            live[id] = {when, seq++};
        } else if (op < 8 && !live.empty()) {
            auto it = live.begin();
            std::advance(it, static_cast<long>(rng() % live.size()));
            EXPECT_TRUE(tq.cancel(it->first));
            live.erase(it);
        } else {
            now += static_cast<int64_t>(rng() % 3000);
            std::vector<std::pair<std::pair<int64_t, uint64_t>, TimerId>> expected;
            for (auto it = live.begin(); it != live.end();) {
                if (it->second.first <= now) {
                    expected.push_back({it->second, it->first});
                    it = live.erase(it);
                } else {
                    ++it;
                }
            }
            std::sort(expected.begin(), expected.end());
            fired.clear();
            EXPECT_EQ(tq.poll(now), static_cast<int>(expected.size()));
            ASSERT_EQ(fired.size(), expected.size());
            for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(fired[i], expected[i].second);
        }
        ASSERT_EQ(tq.size(), live.size());
    }
}

TEST_F(TestTimerQueue, TestStaleIdNeverAliasesUnderHeavySlotReuse) {
    TimerQueue tq;
    auto f = []() { return true; };
    const TimerId stale = tq.add_task("a", f, 100);
    EXPECT_TRUE(tq.cancel(stale));
    // LIFO free list: every add below lands in the same slot as `stale`
    for (int i = 0; i < 1'000'000; ++i) {
        const TimerId id = tq.add_task("b", f, 100);
        ASSERT_EQ(id.slot, stale.slot);
        ASSERT_FALSE(tq.cancel(stale));
        ASSERT_TRUE(tq.cancel(id));
    }
    EXPECT_TRUE(tq.empty());
}

TEST_F(TestTimerQueue, NoHeapAllocationInSteadyState) {
    TimerQueue tq;
    tq.reserve(256);
    std::array<int64_t, 5> ctx{1, 2, 3, 4, 5};  // 40-byte capture: std::function would allocate
    int64_t sum = 0;
    int64_t now = 1'000'000;
    tq.add_task("heartbeat", [&sum] { ++sum; return true; }, now, 10'000);

    // the counter must see real allocations, or the check below would pass vacuously
    const long probe = g_heap_allocs.load();
    int* volatile p = new int(1);
    delete p;
    ASSERT_GT(g_heap_allocs.load(), probe);

    const long before = g_heap_allocs.load();
    for (int i = 0; i < 10'000; ++i) {
        now += 1'000;
        const TimerId id = tq.add_task("order_timeout", [ctx, &sum] { sum += ctx[4]; return true; }, now + 500);
        if (i % 3 == 0) tq.cancel(id);  // e.g. the order filled before its timeout
        tq.poll(now);
    }
    EXPECT_EQ(g_heap_allocs.load() - before, 0);
    EXPECT_GT(sum, 0);
}

TEST_F(TestTimerQueue, RepeatingTaskStopsWhenItsNextSlotWouldOverflow) {
    // poll(INT64_MAX) is the "fire everything due" idiom: the next slot can't be represented
    TimerQueue tq;
    int count = 0;
    tq.add_task("r", [&]() { ++count; return true; }, 10'000, 10'000);
    EXPECT_EQ(tq.poll(std::numeric_limits<int64_t>::max()), 1);
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(tq.empty());
    EXPECT_EQ(tq.next_expiry_ns(), std::numeric_limits<int64_t>::max());

    // the last representable slot still fires, then the task stops
    const int64_t interval = 1'000'000;
    const int64_t when = std::numeric_limits<int64_t>::max() - interval;
    EXPECT_TRUE(tq.add_task("edge", [&]() { ++count; return true; }, when, interval));
    EXPECT_EQ(tq.poll(when), 1);
    EXPECT_EQ(tq.next_expiry_ns(), std::numeric_limits<int64_t>::max());  // when + interval
    EXPECT_EQ(tq.poll(std::numeric_limits<int64_t>::max()), 1);
    EXPECT_TRUE(tq.empty());
    EXPECT_EQ(count, 3);
}

TEST_F(TestTimerQueue, TaskAddedDueFromACallbackRunsInTheNextPoll) {
    // a callback rescheduling itself "now" (retry asap) must not keep poll() from returning
    TimerQueue tq;
    int runs = 0;
    std::function<bool()> retry = [&]() {
        ++runs;
        tq.add_task("retry", retry, 0);
        return false;
    };
    tq.add_task("retry", retry, 100);
    EXPECT_EQ(tq.poll(1'000), 1);
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(tq.poll(1'000), 0);  // deferred to after this poll's now
    EXPECT_EQ(tq.poll(1'001), 1);
    EXPECT_EQ(runs, 2);

    // tasks already due before the callback ran still fire in the same poll, in order
    TimerQueue tq2;
    std::vector<std::string> events;
    tq2.add_task("a", [&]() {
        events.push_back("a");
        tq2.add_task("late", [&]() { events.push_back("late"); return false; }, 0);
        return false;
    }, 100);
    tq2.add_task("b", [&]() { events.push_back("b"); return false; }, 200);
    EXPECT_EQ(tq2.poll(300), 2);
    EXPECT_EQ(events, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(tq2.poll(301), 1);
    EXPECT_EQ(events.back(), "late");

    // poll(INT64_MAX) can't defer past now: it stops at tasks added during it
    TimerQueue tq3;
    int max_runs = 0;
    std::function<bool()> again = [&]() {
        ++max_runs;
        tq3.add_task("again", again, std::numeric_limits<int64_t>::max());
        return false;
    };
    tq3.add_task("again", again, 5);
    EXPECT_EQ(tq3.poll(std::numeric_limits<int64_t>::max()), 1);
    EXPECT_EQ(max_runs, 1);
}

TEST_F(TestTimerQueue, LongNamesDoNotAllocate) {
    TimerQueue tq;
    tq.reserve(64);
    int64_t now = 1'000;
    const long before = g_heap_allocs.load();
    for (int i = 0; i < 1'000; ++i) {
        now += 10;
        (void)tq.add_task("a_timer_name_well_beyond_the_small_string_size_of_std_string",
                          [] { return false; }, now);
        tq.poll(now);
    }
    EXPECT_EQ(g_heap_allocs.load() - before, 0);
}

TEST_F(TestTimerQueue, CancelRejectsForeignAndOutOfRangeIds) {
    TimerQueue a, b;
    const TimerId id = a.add_task("x", [] { return false; }, 100);
    EXPECT_FALSE(b.cancel(id));                                   // not b's
    EXPECT_FALSE(a.cancel(TimerId{id.uid, id.slot + 1'000'000}));  // slot out of range
    EXPECT_FALSE(a.cancel(TimerId{id.uid + 1, id.slot}));         // stale / unknown uid
    EXPECT_TRUE(a.cancel(id));
    EXPECT_FALSE(a.cancel(id));
}
