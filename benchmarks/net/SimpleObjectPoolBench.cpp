#include "quark/utils/MemoryArena.h"
#include "quark/utils/SimpleObjectPool.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace quark;

namespace {

// a typical order record: 216 bytes, value-initialized on alloc
struct Order {
    uint32_t account_id, sid;
    int8_t side, status, offset, tif;
    char reserve[4];
    double price, qty, exec_price, exec_qty, remain_qty, fee, cum_qty, avg_price;
    char order_id[64];
    char trade_id[64];
    int64_t exec_ts_ns;
};
static_assert(sizeof(Order) == 216);

enum Backing : int64_t { kHeap, kArena4K, kArenaHuge };

const char* backing_name(int64_t b) {
    return b == kHeap ? "heap" : b == kArena4K ? "arena_4k" : "arena_huge";
}

// Pool plus (optionally) the arena it draws from, sized for `live` objects in one chunk
struct Fixture {
    std::unique_ptr<MemoryArena> arena;
    std::unique_ptr<SimpleObjectPool<Order>> pool;

    Fixture(int64_t backing, std::size_t live) {
        if (backing != kHeap) {
            const auto huge = backing == kArenaHuge ? MemoryArena::HugePages::Always : MemoryArena::HugePages::Never;
            arena = std::make_unique<MemoryArena>(live * sizeof(Order) + 4096, MemoryArena::Options{.huge_pages = huge});
        }
        pool = std::make_unique<SimpleObjectPool<Order>>(live, 128, arena.get());
    }
};

std::vector<uint32_t> random_indices(std::size_t live) {
    std::vector<uint32_t> idx(1 << 20);
    std::mt19937 rng(42);
    for (auto& i : idx) i = static_cast<uint32_t>(rng() % live);
    return idx;
}

// Tight alloc/free of one object: the free-list fast path, independent of the backing
void BM_AllocFreePair(benchmark::State& state) {
    Fixture f(state.range(0), 4096);
    for (auto _ : state) {
        Order* o = f.pool->alloc();
        o->qty = 1;
        benchmark::DoNotOptimize(o);
        f.pool->free(o);
    }
    state.SetLabel(backing_name(state.range(0)));
}
BENCHMARK(BM_AllocFreePair)->ArgName("backing")->Arg(kHeap)->Arg(kArena4K)->Arg(kArenaHuge);

// An OMS-like working set: `live` open orders, each step cancels a random one and places a new
// one (free + alloc + write). The random slot access is what huge pages speed up.
void BM_RandomChurn(benchmark::State& state) {
    const auto live = static_cast<std::size_t>(state.range(1));
    Fixture f(state.range(0), live);
    std::vector<Order*> orders(live);
    for (auto& o : orders) o = f.pool->alloc();
    const auto idx = random_indices(live);
    std::size_t i = 0;
    for (auto _ : state) {
        const uint32_t k = idx[i++ & (idx.size() - 1)];
        f.pool->free(orders[k]);
        Order* o = f.pool->alloc();
        o->qty = static_cast<double>(i);
        orders[k] = o;
    }
    for (auto* o : orders) f.pool->free(o);
    state.SetLabel(backing_name(state.range(0)));
}
BENCHMARK(BM_RandomChurn)
    ->ArgNames({"backing", "live"})
    ->ArgsProduct({{kHeap, kArena4K, kArenaHuge}, {50'000, 500'000}});

} // namespace
