// Fixed-point conversions, tick rounding and formatting: what a price or quantity costs per order.
#include "quark/math/ScaleInt.h"

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

using namespace quark::math;

namespace {

std::vector<double> prices() {
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> d(20'000.0, 120'000.0);
    std::vector<double> v(4096);
    for (auto& x : v) x = d(rng);
    return v;
}

std::vector<DoubleE8> fixed_prices() {
    std::vector<DoubleE8> v;
    for (double p : prices()) v.emplace_back(p);
    return v;
}

void BM_ScaleIntFromDouble(benchmark::State& state) {
    const auto in = prices();
    std::size_t i = 0;
    for (auto _ : state) benchmark::DoNotOptimize(DoubleE8(in[i++ & 4095]));
}
BENCHMARK(BM_ScaleIntFromDouble);

void BM_ScaleIntAsDouble(benchmark::State& state) {
    const auto in = fixed_prices();
    std::size_t i = 0;
    for (auto _ : state) benchmark::DoNotOptimize(in[i++ & 4095].as_double());
}
BENCHMARK(BM_ScaleIntAsDouble);

// tick rounding, from the fastest form to the slowest (see ScaleInt.h)
void BM_FloorToTickCompileTime(benchmark::State& state) {
    const auto in = fixed_prices();
    std::size_t i = 0;
    for (auto _ : state) {
        DoubleE8 p = in[i++ & 4095];
        benchmark::DoNotOptimize(p.floor_to_tick<5'000'000>());  // 0.05
    }
}
BENCHMARK(BM_FloorToTickCompileTime);

void BM_FloorToTickSizePow10(benchmark::State& state) {
    const auto in = fixed_prices();
    TickSize<8> tick(0.01);
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(tick);  // a runtime tick (per instrument), not a constant
        DoubleE8 p = in[i++ & 4095];
        benchmark::DoNotOptimize(p.floor_to_tick(tick));
    }
}
BENCHMARK(BM_FloorToTickSizePow10);

void BM_FloorToTickSizeOther(benchmark::State& state) {
    const auto in = fixed_prices();
    TickSize<8> tick(0.05);  // not a power of ten: one idiv
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(tick);
        DoubleE8 p = in[i++ & 4095];
        benchmark::DoNotOptimize(p.floor_to_tick(tick));
    }
}
BENCHMARK(BM_FloorToTickSizeOther);

void BM_FloorToTickDouble(benchmark::State& state) {
    const auto in = fixed_prices();
    double tick = 0.05;
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(tick);
        DoubleE8 p = in[i++ & 4095];
        benchmark::DoNotOptimize(p.floor_to_tick(tick));
    }
}
BENCHMARK(BM_FloorToTickDouble);

// formatting a price into an order message
void BM_ScaleIntToChars(benchmark::State& state) {
    const auto in = fixed_prices();
    char out[64];
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(in[i++ & 4095].to_chars(out));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ScaleIntToChars);

void BM_DoubleToString(benchmark::State& state) {
    const auto in = prices();
    std::size_t i = 0;
    for (auto _ : state) benchmark::DoNotOptimize(double_to_string(in[i++ & 4095]));
}
BENCHMARK(BM_DoubleToString);

void BM_RoundToTickFreeFunction(benchmark::State& state) {
    const auto in = prices();
    std::size_t i = 0;
    double tick = 0.05;
    for (auto _ : state) {
        benchmark::DoNotOptimize(tick);
        benchmark::DoNotOptimize(round_to_tick(in[i++ & 4095], tick));
    }
}
BENCHMARK(BM_RoundToTickFreeFunction);

}  // namespace
