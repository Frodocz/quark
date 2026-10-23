#include "quark/net/BufferPool.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace quark;
using namespace quark::net;

namespace {

MemoryArena::Options pages(int64_t huge) {
    return {.huge_pages = huge ? MemoryArena::HugePages::Always : MemoryArena::HugePages::Never};
}

// acquire + release of one buffer: the free-list fast path
void BM_AcquireRelease(benchmark::State& state) {
    BufferPool pool(256, 4096, pages(state.range(0)));
    for (auto _ : state) {
        IOBuffer b = pool.acquire(14);
        benchmark::DoNotOptimize(b.data());
        pool.release(b);
    }
}
BENCHMARK(BM_AcquireRelease)->ArgName("huge")->Arg(0)->Arg(1);

// Frame building over many held buffers (e.g. per-session receive buffers or queued frames):
// write a 256-byte payload and prepend a header into a randomly chosen buffer.
void BM_RandomFrameWrite(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(1));
    BufferPool pool(count, 4096, pages(state.range(0)));
    std::vector<IOBuffer> bufs(count);
    for (auto& b : bufs) b = pool.acquire(14);
    std::vector<uint32_t> idx(1 << 20);
    std::mt19937 rng(7);
    for (auto& i : idx) i = static_cast<uint32_t>(rng() % count);
    uint8_t payload[256];
    std::memset(payload, 'x', sizeof(payload));
    std::size_t i = 0;
    for (auto _ : state) {
        IOBuffer& b = bufs[idx[i++ & (idx.size() - 1)]];
        b.reset(14);
        std::memcpy(b.append(sizeof(payload)).data(), payload, sizeof(payload));
        auto hdr = b.prepend(4);
        hdr[0] = 0x82;
        benchmark::DoNotOptimize(b.data());
    }
    for (auto& b : bufs) pool.release(b);
    state.SetLabel(pool.page_kind() == MemoryArena::PageKind::Normal ? "4k_pages" : "huge_pages");
}
BENCHMARK(BM_RandomFrameWrite)->ArgNames({"huge", "buffers"})->ArgsProduct({{0, 1}, {256, 8192}});

} // namespace
