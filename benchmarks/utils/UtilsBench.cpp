// Building blocks used on every step or message: timers, arena allocation, inline callables,
// clocks, byte order, JSON parsing, the circular buffer.
#include "quark/utils/CircularBuffer.h"
#include "quark/utils/HwEndian.h"
#include "quark/utils/InplaceFunction.h"
#include "quark/utils/JsonParser.h"
#include "quark/utils/MemoryArena.h"
#include "quark/utils/RdtscClock.h"
#include "quark/utils/TimerQueue.h"

#include <benchmark/benchmark.h>

#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace quark;

namespace {

// ------------------------------------------------------------------ TimerQueue

// nothing due: what every EventLoop step pays
void BM_TimerQueuePollIdle(benchmark::State& state) {
    TimerQueue tq;
    tq.add_task("far", [] { return false; }, std::numeric_limits<int64_t>::max() - 1);
    int64_t now = 1'000;
    for (auto _ : state) benchmark::DoNotOptimize(tq.poll(++now));
}
BENCHMARK(BM_TimerQueuePollIdle);

// e.g. an order timeout armed, then cancelled when the order fills; with N other timers pending
void BM_TimerQueueAddCancel(benchmark::State& state) {
    TimerQueue tq;
    tq.reserve(static_cast<uint32_t>(state.range(0)) + 16);
    for (int64_t i = 0; i < state.range(0); ++i) tq.add_task("bg", [] { return false; }, 1'000'000'000 + i);
    int64_t now = 1'000;
    for (auto _ : state) {
        const TimerId id = tq.add_task("order_timeout", [] { return false; }, now + 500'000);
        benchmark::DoNotOptimize(tq.cancel(id));
        ++now;
    }
}
BENCHMARK(BM_TimerQueueAddCancel)->Arg(0)->Arg(64)->Arg(1024);

// a timer added and fired (one-shot)
void BM_TimerQueueAddFire(benchmark::State& state) {
    TimerQueue tq;
    tq.reserve(64);
    int64_t now = 1'000;
    int64_t fired = 0;
    for (auto _ : state) {
        tq.add_task("t", [&fired] { ++fired; return false; }, now);
        benchmark::DoNotOptimize(tq.poll(now));
        now += 10;
    }
    benchmark::DoNotOptimize(fired);
}
BENCHMARK(BM_TimerQueueAddFire);

// ------------------------------------------------------------------ MemoryArena

void BM_MemoryArenaAllocate(benchmark::State& state) {
    constexpr std::size_t kCap = 64 * 1024 * 1024;
    auto arena = std::make_unique<MemoryArena>(kCap, MemoryArena::Options{.huge_pages = MemoryArena::HugePages::Never, .lock = false});
    for (auto _ : state) {
        void* p = arena->allocate(64, 64);
        if (p == nullptr) [[unlikely]] {
            state.PauseTiming();
            arena = std::make_unique<MemoryArena>(kCap, MemoryArena::Options{.huge_pages = MemoryArena::HugePages::Never, .lock = false});
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_MemoryArenaAllocate);

// ------------------------------------------------------------------ callables

void BM_InplaceFunctionCall(benchmark::State& state) {
    int64_t sum = 0;
    std::array<int64_t, 4> ctx{1, 2, 3, 4};
    InplaceFunction<void(), 64> f([&sum, ctx] { sum += ctx[3]; });
    for (auto _ : state) {
        f();
        benchmark::ClobberMemory();
    }
    benchmark::DoNotOptimize(sum);
}
BENCHMARK(BM_InplaceFunctionCall);

void BM_StdFunctionCall(benchmark::State& state) {
    int64_t sum = 0;
    std::array<int64_t, 4> ctx{1, 2, 3, 4};
    std::function<void()> f([&sum, ctx] { sum += ctx[3]; });
    for (auto _ : state) {
        f();
        benchmark::ClobberMemory();
    }
    benchmark::DoNotOptimize(sum);
}
BENCHMARK(BM_StdFunctionCall);

// constructing one: InplaceFunction never allocates, std::function does past its small buffer
void BM_InplaceFunctionConstruct(benchmark::State& state) {
    std::array<int64_t, 5> ctx{1, 2, 3, 4, 5};
    for (auto _ : state) {
        InplaceFunction<void(), 64> f([ctx] {
            int64_t x = ctx[0];
            benchmark::DoNotOptimize(x);
        });
        benchmark::DoNotOptimize(f);
    }
}
BENCHMARK(BM_InplaceFunctionConstruct);

void BM_StdFunctionConstruct(benchmark::State& state) {
    std::array<int64_t, 5> ctx{1, 2, 3, 4, 5};
    for (auto _ : state) {
        std::function<void()> f([ctx] {
            int64_t x = ctx[0];
            benchmark::DoNotOptimize(x);
        });
        benchmark::DoNotOptimize(f);
    }
}
BENCHMARK(BM_StdFunctionConstruct);

// ------------------------------------------------------------------ clocks, byte order

void BM_Rdtsc(benchmark::State& state) {
    for (auto _ : state) benchmark::DoNotOptimize(RdtscClock::rdtsc());
}
BENCHMARK(BM_Rdtsc);

void BM_Tscns(benchmark::State& state) {
    for (auto _ : state) benchmark::DoNotOptimize(RdtscClock::tscns());
}
BENCHMARK(BM_Tscns);

void BM_LoadBe64(benchmark::State& state) {
    std::vector<uint8_t> buf(4096 + 8, 0x5a);
    std::size_t i = 0;
    for (auto _ : state) benchmark::DoNotOptimize(load_be<uint64_t>(buf.data() + (i++ & 4095)));
}
BENCHMARK(BM_LoadBe64);

// ------------------------------------------------------------------ JSON

// a trade message, the common market data case
void BM_JsonParseSmall(benchmark::State& state) {
    const std::string msg = R"({"e":"trade","E":1672515782136,"s":"BTCUSDT","t":12345,"p":"16573.10","q":"0.003",)"
                            R"("b":88,"a":50,"T":1672515782136,"m":true,"M":true})";
    JsonParser parser;
    for (auto _ : state) {
        auto doc = parser.parse_copy(msg);
        double price = 0;
        std::string_view p;
        if (doc["p"].get_string().get(p) == simdjson::SUCCESS) price = static_cast<double>(p.size());
        benchmark::DoNotOptimize(price);
        benchmark::DoNotOptimize(doc["T"].get_int64().value_unsafe());
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * msg.size()));
}
BENCHMARK(BM_JsonParseSmall);

// a 650 KB order book snapshot, every level visited
void BM_JsonParseDepthSnapshot(benchmark::State& state) {
    std::ifstream f(QUARK_BENCH_DATA_DIR "/utils/Binance_LinearSwap_BTCUSDT_depth.json");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string json = ss.str();
    JsonParser parser(json.size() + 64);
    for (auto _ : state) {
        auto doc = parser.parse_copy(json);
        std::size_t levels = 0;
        for (auto level : doc["bids"].get_array()) {
            (void)level;
            ++levels;
        }
        benchmark::DoNotOptimize(levels);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * json.size()));
}
BENCHMARK(BM_JsonParseDepthSnapshot);

// ------------------------------------------------------------------ CircularBuffer

void BM_CircularBufferPushPop(benchmark::State& state) {
    CircularBuffer<int64_t> b(1024);
    for (int i = 0; i < 512; ++i) b.push_back(i);
    int64_t v = 0;
    for (auto _ : state) {
        b.push_back(++v);
        benchmark::DoNotOptimize(b.front());
        b.pop_front();
    }
}
BENCHMARK(BM_CircularBufferPushPop);

}  // namespace
