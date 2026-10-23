#include "quark/utils/DateTime.h"
#include "quark/utils/RdtscClock.h"
#include "quark/utils/SimpleObjectPool.h"
#include "gtest/gtest.h"

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace quark;

struct TestSimpleObjectPool : public ::testing::Test {
    static void SetUpTestSuite() { RdtscClock::calibrate(); }
};

struct Order {
    enum class Side : int8_t { Unknown, Buy, Sell, NUM };
    enum class OrderStatus : int8_t {
        Unknown,
        PendingNew,
        PendingCancel,
        New,
        PartiallyFilled,
        Filled,
        Cancelled,
        Rejected,
        InternalRejected,
        Error
    };
    enum class Offset : int8_t { Unknown, Open, Close };
    enum class TimeInForce : int8_t { GTC, IOC, FOK, PO, RPI, MAX };
    
    Order() { std::memset(reinterpret_cast<void*>(this), 0, sizeof(Order)); }
    uint32_t account_id;
    uint32_t sid;
    Side side;
    OrderStatus status;
    Offset offset;
    TimeInForce time_in_force;
    char reserve[4];
    double order_price;
    double qty;
    double exec_price;
    double exec_qty;
    double remain_qty;
    double fee;
    double cum_exec_qty;
    double avg_exec_price;
    char order_id[64];
    char trade_id[64];
    int64_t exec_ts_ns;
};
static_assert(sizeof(Order) == 216);

TEST_F(TestSimpleObjectPool, Basic) {
    std::set<Order*> distinct_addr;
    std::vector<Order*> tmp;
    tmp.reserve(1024);
    SimpleObjectPool<Order> pool(128, 16);

    const char zero_buf[sizeof(Order)] = {0};

    for (int i = 0; i < 1024; i++) {
        auto* o = pool.alloc();
        EXPECT_EQ(0, memcmp(o, zero_buf, sizeof(Order)));
        o->account_id = i;
        o->qty = 1.0;
        tmp.push_back(o);
        distinct_addr.insert(o);
    }

    EXPECT_EQ(distinct_addr.size(), tmp.size());
    EXPECT_EQ(pool.get_capacity(), 1024);
    EXPECT_EQ(pool.get_size_avail(), 0);
    EXPECT_EQ(pool.get_size_used(), 1024);

    for (auto* o : tmp) {
        pool.free(o);
    }

    EXPECT_EQ(distinct_addr.size(), 1024);
    EXPECT_EQ(pool.get_capacity(), 1024);
    EXPECT_EQ(pool.get_size_avail(), 1024);
    EXPECT_EQ(pool.get_size_used(), 0);

    // since we have free all used objects, they will be resued
    for (int i = 0; i < 1024; i++) {
        auto* o = pool.alloc();
        distinct_addr.insert(o);
        EXPECT_EQ(0, memcmp(o, zero_buf, sizeof(Order)));
    }

    EXPECT_EQ(distinct_addr.size(), 1024);
    EXPECT_EQ(pool.get_capacity(), 1024);
    EXPECT_EQ(pool.get_size_avail(), 0);
    EXPECT_EQ(pool.get_size_used(), 1024);
}

TEST_F(TestSimpleObjectPool, Benchmark) {
    SimpleObjectPool<Order> pool(128, 16);

    int iter_cnt = 10;
    int batch_cnt = 1000;

    std::vector<Order*> tmp;
    tmp.reserve(1024);

    auto start = RdtscClock::tscns();
    for (int i = 0; i < iter_cnt; i++) {
        for (int j = 0; j < batch_cnt; j++) {
            tmp.push_back(pool.alloc());
        }
        for (auto* p : tmp) {
            pool.free(p);
        }
        tmp.clear();
    }
    auto end = RdtscClock::tscns();
    std::cout << "(alloc+free) latency:" << (end - start) / (iter_cnt * batch_cnt) << "ns" << std::endl;
}
TEST_F(TestSimpleObjectPool, PointersStableAcrossGrowth) {
    SimpleObjectPool<Order> pool(4, 4);
    std::vector<Order*> objs;
    for (int i = 0; i < 10'000; i++) {
        auto* o = pool.alloc();
        o->account_id = static_cast<uint32_t>(i);
        o->qty = i * 0.5;
        objs.push_back(o);
    }
    // every earlier pointer must still be valid after 2500 chunk allocations
    for (int i = 0; i < 10'000; i++) {
        ASSERT_EQ(objs[i]->account_id, static_cast<uint32_t>(i));
        ASSERT_EQ(objs[i]->qty, i * 0.5);
    }
    for (auto* o : objs) pool.free(o);
    EXPECT_EQ(pool.get_size_used(), 0u);
}

namespace {
struct Tracked {
    static inline int ctor = 0;
    static inline int dtor = 0;
    std::string payload;
    int value;
    Tracked() : payload(64, 'x'), value(-1) { ++ctor; }
    Tracked(std::string p, int v) : payload(std::move(p)), value(v) { ++ctor; }
    ~Tracked() { ++dtor; }
};

struct Throwing {
    explicit Throwing(bool fail) {
        if (fail) throw std::runtime_error("ctor failed");
    }
};
}  // namespace

TEST_F(TestSimpleObjectPool, ExactlyOneCtorDtorPerObject) {
    Tracked::ctor = Tracked::dtor = 0;
    {
        SimpleObjectPool<Tracked> pool(8, 8);
        // preallocation must not construct anything
        EXPECT_EQ(Tracked::ctor, 0);
        std::vector<Tracked*> objs;
        for (int i = 0; i < 100; i++) objs.push_back(pool.alloc());
        EXPECT_EQ(Tracked::ctor, 100);
        for (auto* o : objs) pool.free(o);
        EXPECT_EQ(Tracked::dtor, 100);
    }
    // the pool's destructor must not destroy already-freed slots again
    EXPECT_EQ(Tracked::ctor, 100);
    EXPECT_EQ(Tracked::dtor, 100);
}

TEST_F(TestSimpleObjectPool, ConstructorArguments) {
    SimpleObjectPool<Tracked> pool(2, 2);
    auto* t = pool.alloc(std::string(100, 'a'), 42);
    EXPECT_EQ(t->value, 42);
    EXPECT_EQ(t->payload, std::string(100, 'a'));
    pool.free(t);
}

TEST_F(TestSimpleObjectPool, ThrowingConstructorReturnsSlot) {
    SimpleObjectPool<Throwing> pool(1, 1);
    EXPECT_THROW((void)pool.alloc(true), std::runtime_error);
    EXPECT_EQ(pool.get_size_used(), 0u);
    EXPECT_EQ(pool.get_capacity(), 1u);
    auto* ok = pool.alloc(false);
    EXPECT_EQ(pool.get_capacity(), 1u);  // reused the slot, no growth
    pool.free(ok);
}

TEST_F(TestSimpleObjectPool, FreeNullIsNoop) {
    SimpleObjectPool<Order> pool(1, 1);
    pool.free(nullptr);
    EXPECT_EQ(pool.get_size_used(), 0u);
}

TEST_F(TestSimpleObjectPool, LifoReuseAndAddressOrder) {
    SimpleObjectPool<Order> pool(4, 4);
    Order* a = pool.alloc();
    Order* b = pool.alloc();
    EXPECT_EQ(reinterpret_cast<char*>(b) - reinterpret_cast<char*>(a), static_cast<ptrdiff_t>(sizeof(Order)));
    pool.free(a);
    EXPECT_EQ(pool.alloc(), a);  // most recently freed slot is reused first (cache hot)
}

TEST_F(TestSimpleObjectPool, OverAlignedType) {
    struct alignas(128) Wide { char c[200]; };
    SimpleObjectPool<Wide> pool(3, 3);
    for (int i = 0; i < 10; i++) {
        auto* w = pool.alloc();
        EXPECT_EQ(reinterpret_cast<uintptr_t>(w) % 128, 0u);
    }
}

#ifndef NDEBUG
TEST_F(TestSimpleObjectPool, DoubleFreeDetectedInDebug) {
    SimpleObjectPool<Order> pool(2, 2);
    auto* o = pool.alloc();
    pool.free(o);
    EXPECT_DEATH(pool.free(o), "double free");
    Order foreign;
    EXPECT_DEATH(pool.free(&foreign), "not from this pool");
}
#endif

#if defined(__SANITIZE_ADDRESS__)
TEST_F(TestSimpleObjectPool, UseAfterFreeReportedUnderAsan) {
    SimpleObjectPool<Order> pool(2, 2);
    auto* o = pool.alloc();
    pool.free(o);
    EXPECT_DEATH({ volatile double q = o->qty; (void)q; }, "use-after-poison");
}
#endif
