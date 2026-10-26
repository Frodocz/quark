// Hash containers for the workloads applications on quark have: maps keyed by integer ids
// (orders, requests), maps keyed by short strings looked up through string_view (symbols,
// channels), steady-state insert/erase churn (an order's lifecycle) and full iteration.
//
// Compared: std::unordered_map, absl::flat_hash_map, ankerl::unordered_dense::map (and the
// matching sets). Sizes: 1k (fits L1/L2) and 1M (beyond the LLC). All tables are reserved up
// front, as a latency-sensitive application would.
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <ankerl/unordered_dense.h>
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Value {  // a small record, e.g. an order's state
    uint64_t a[8];
};

// transparent hashes so string maps can be probed with a string_view without building a std::string
struct StdStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

using StdIdMap = std::unordered_map<uint64_t, Value>;
using AbslIdMap = absl::flat_hash_map<uint64_t, Value>;
using AnkerlIdMap = ankerl::unordered_dense::map<uint64_t, Value>;
using StdIdSet = std::unordered_set<uint64_t>;
using AbslIdSet = absl::flat_hash_set<uint64_t>;
using AnkerlIdSet = ankerl::unordered_dense::set<uint64_t>;

using StdStrMap = std::unordered_map<std::string, Value, StdStringHash, std::equal_to<>>;
using AbslStrMap = absl::flat_hash_map<std::string, Value>;  // heterogeneous lookup built in
struct AnkerlStringHash {
    using is_transparent = void;
    using is_avalanching = void;  // ankerl's string hash (wyhash) needs no extra mixing
    uint64_t operator()(std::string_view s) const noexcept { return ankerl::unordered_dense::hash<std::string_view>{}(s); }
};
using AnkerlStrMap = ankerl::unordered_dense::map<std::string, Value, AnkerlStringHash, std::equal_to<>>;

// ids as an exchange or an id generator hands them out: increasing, not contiguous
std::vector<uint64_t> make_ids(size_t n) {
    std::mt19937_64 rng(42);
    std::vector<uint64_t> ids(n);
    uint64_t id = 1'000'000'000;
    for (auto& x : ids) x = id += 1 + rng() % 16;
    return ids;
}

std::vector<std::string> make_symbols(size_t n) {
    std::mt19937_64 rng(7);
    std::vector<std::string> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        std::string s;
        const size_t len = 6 + rng() % 8;  // "BTCUSDT"-like, within SSO
        for (size_t j = 0; j < len; ++j) s += static_cast<char>('A' + rng() % 26);
        out.push_back(std::move(s) + std::to_string(i));
    }
    return out;
}

template <class T>
std::vector<T> shuffled(std::vector<T> v) {
    std::shuffle(v.begin(), v.end(), std::mt19937_64(1));
    return v;
}

// ------------------------------------------------------------------ integer keys

template <class Map>
void BM_IdFindHit(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto ids = make_ids(n);
    Map m;
    m.reserve(n);
    for (auto id : ids) m[id] = Value{{id}};
    const auto probes = shuffled(ids);
    size_t i = 0;
    for (auto _ : state) {
        auto it = m.find(probes[i]);
        benchmark::DoNotOptimize(it->second.a[0]);
        if (++i == n) i = 0;
    }
}

template <class Map>
void BM_IdFindMiss(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto ids = make_ids(n);
    Map m;
    m.reserve(n);
    for (auto id : ids) m[id] = Value{{id}};
    auto probes = shuffled(ids);
    for (auto& p : probes) p += 1u << 31;  // beyond every stored id
    size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(m.find(probes[i]) == m.end());
        if (++i == n) i = 0;
    }
}

// n live entries; each iteration inserts a new id and erases the oldest (orders opening and closing)
template <class Map>
void BM_IdChurn(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto ids = make_ids(n * 4);
    Map m;
    m.reserve(n * 2);
    for (size_t k = 0; k < n; ++k) m.emplace(ids[k], Value{{ids[k]}});
    size_t oldest = 0, next = n;
    for (auto _ : state) {
        m.emplace(ids[next], Value{{ids[next]}});
        m.erase(ids[oldest]);
        if (++next == ids.size()) next = 0;
        if (++oldest == ids.size()) oldest = 0;
    }
    benchmark::DoNotOptimize(m.size());
}

template <class Map>
void BM_IdIterate(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    Map m;
    m.reserve(n);
    for (auto id : make_ids(n)) m[id] = Value{{id}};
    for (auto _ : state) {
        uint64_t sum = 0;
        for (const auto& [k, v] : m) sum += v.a[0];
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * n));
}

template <class Set>
void BM_IdSetContains(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto ids = make_ids(n);
    Set s;
    s.reserve(n);
    s.insert(ids.begin(), ids.end());
    auto probes = shuffled(ids);
    for (size_t k = 0; k < n; k += 2) probes[k] += 1u << 31;  // half hits, half misses
    size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(s.contains(probes[i]));
        if (++i == n) i = 0;
    }
}

// ------------------------------------------------------------------ string keys

// lookups come from parsed messages: a string_view into the receive buffer
template <class Map>
void BM_StrFindHit(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto syms = make_symbols(n);
    Map m;
    m.reserve(n);
    for (const auto& s : syms) m[s] = Value{{s.size()}};
    const auto probes = shuffled(syms);
    std::vector<std::string_view> views(probes.begin(), probes.end());
    size_t i = 0;
    for (auto _ : state) {
        auto it = m.find(views[i]);
        benchmark::DoNotOptimize(it->second.a[0]);
        if (++i == n) i = 0;
    }
}

template <class Map>
void BM_StrFindMiss(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto syms = make_symbols(n);
    Map m;
    m.reserve(n);
    for (const auto& s : syms) m[s] = Value{{s.size()}};
    auto probes = shuffled(syms);
    for (auto& p : probes) p.back() = 'x';  // digits only end the stored keys
    std::vector<std::string_view> views(probes.begin(), probes.end());
    size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(m.find(views[i]) == m.end());
        if (++i == n) i = 0;
    }
}

constexpr int64_t kSmall = 1 << 10;
constexpr int64_t kLarge = 1 << 20;

#define QUARK_CONTAINER_BENCH(fn, type) BENCHMARK_TEMPLATE(fn, type)->Arg(kSmall)->Arg(kLarge)

QUARK_CONTAINER_BENCH(BM_IdFindHit, StdIdMap);
QUARK_CONTAINER_BENCH(BM_IdFindHit, AbslIdMap);
QUARK_CONTAINER_BENCH(BM_IdFindHit, AnkerlIdMap);
QUARK_CONTAINER_BENCH(BM_IdFindMiss, StdIdMap);
QUARK_CONTAINER_BENCH(BM_IdFindMiss, AbslIdMap);
QUARK_CONTAINER_BENCH(BM_IdFindMiss, AnkerlIdMap);
QUARK_CONTAINER_BENCH(BM_IdChurn, StdIdMap);
QUARK_CONTAINER_BENCH(BM_IdChurn, AbslIdMap);
QUARK_CONTAINER_BENCH(BM_IdChurn, AnkerlIdMap);
QUARK_CONTAINER_BENCH(BM_IdIterate, StdIdMap);
QUARK_CONTAINER_BENCH(BM_IdIterate, AbslIdMap);
QUARK_CONTAINER_BENCH(BM_IdIterate, AnkerlIdMap);
QUARK_CONTAINER_BENCH(BM_IdSetContains, StdIdSet);
QUARK_CONTAINER_BENCH(BM_IdSetContains, AbslIdSet);
QUARK_CONTAINER_BENCH(BM_IdSetContains, AnkerlIdSet);
QUARK_CONTAINER_BENCH(BM_StrFindHit, StdStrMap);
QUARK_CONTAINER_BENCH(BM_StrFindHit, AbslStrMap);
QUARK_CONTAINER_BENCH(BM_StrFindHit, AnkerlStrMap);
QUARK_CONTAINER_BENCH(BM_StrFindMiss, StdStrMap);
QUARK_CONTAINER_BENCH(BM_StrFindMiss, AbslStrMap);
QUARK_CONTAINER_BENCH(BM_StrFindMiss, AnkerlStrMap);

}  // namespace
