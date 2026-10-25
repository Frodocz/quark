#include "quark/utils/CommandQueue.h"
#include "quark/utils/LatencyHistogram.h"

#include <benchmark/benchmark.h>

#include <atomic>
#include <random>
#include <thread>
#include <vector>

using namespace quark;

namespace {

// post + drain on one thread: the queue's own cost
void BM_CommandQueuePostDrain(benchmark::State& state) {
    CommandQueue<1024> q;
    int64_t sum = 0;
    for (auto _ : state) {
        q.post([&sum] { ++sum; });
        q.drain();
    }
    benchmark::DoNotOptimize(sum);
}
BENCHMARK(BM_CommandQueuePostDrain);

// a producer thread posting, this thread draining (as an event loop's step hook would)
void BM_CommandQueueCrossThread(benchmark::State& state) {
    CommandQueue<4096> q;
    std::atomic<bool> stop{false};
    int64_t ran = 0;
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) q.post([&ran] { ++ran; });
    });
    for (auto _ : state) {
        while (q.drain(1) == 0) {
        }
    }
    stop = true;
    producer.join();
    q.drain();
    benchmark::DoNotOptimize(ran);
}
BENCHMARK(BM_CommandQueueCrossThread)->UseRealTime();

void BM_LatencyHistogramRecord(benchmark::State& state) {
    LatencyHistogram h;
    std::vector<int64_t> samples(4096);
    std::mt19937_64 rng(1);
    for (auto& s : samples) s = static_cast<int64_t>(rng() % 1'000'000);
    std::size_t i = 0;
    benchmark::DoNotOptimize(h);  // h escapes, so the clobber below keeps every store
    for (auto _ : state) {
        h.record(samples[i++ & 4095]);
        benchmark::ClobberMemory();
    }
    benchmark::DoNotOptimize(h.count());
}
BENCHMARK(BM_LatencyHistogramRecord);

} // namespace
