// The per-message dedup step of WsGroup::deliver(): the key, then TimeScopedDedup::first(). Runs before
// the first copy of every market-data message is delivered. Default: std::hash of the whole text;
// with WsGroupOptions::dedup_key: an exchange-native key (here: the update id at a known offset).
#include "quark/net/WsSession.h"
#include "quark/utils/InplaceFunction.h"
#include "quark/utils/TimeScopedDedup.h"

#include <benchmark/benchmark.h>

#include <charconv>
#include <functional>
#include <string>
#include <string_view>

using namespace quark;
using namespace quark::net;

namespace {

// a depth-like message of about `size` bytes with a unique "u" (update id) early in it
std::string message(std::size_t size, uint64_t u) {
    std::string s = R"({"stream":"btcusdt@depth","data":{"e":"depthUpdate","u":)" + std::to_string(u) + R"(,"b":[)";
    while (s.size() + 40 < size) s += R"(["65000.10","0.123"],)";
    s += R"(["1","1"]]}})";
    return s;
}

uint64_t update_id(const WsMessage& m) {
    const std::string_view t = m.text();
    const std::size_t at = t.find("\"u\":");
    if (at == std::string_view::npos || at > 64) return 0;
    uint64_t u = 0;
    std::from_chars(t.data() + at + 4, t.data() + t.size(), u);
    return u;
}

void run(benchmark::State& state, bool with_key) {
    const std::size_t size = static_cast<std::size_t>(state.range(0));
    std::vector<std::string> msgs;
    for (uint64_t u = 0; u < 1024; ++u) msgs.push_back(message(size, 1'000'000 + u));
    TimeScopedDedup dedup{10'000'000'000, 1 << 16};
    InplaceFunction<uint64_t(const WsMessage&), 32> key_fn;
    if (with_key) key_fn = [](const WsMessage& m) { return update_id(m); };
    int64_t now = 1;
    std::size_t i = 0;
    for (auto _ : state) {
        const std::string& s = msgs[i++ & 1023];
        const WsMessage m{WsOpcode::Text, {reinterpret_cast<const uint8_t*>(s.data()), s.size()}, s.size(), now};
        // as in WsGroup::deliver()
        uint64_t key = key_fn ? key_fn(m) : 0;
        if (key == 0) key = std::hash<std::string_view>{}(m.text());
        benchmark::DoNotOptimize(dedup.first(key, now));
        now += 1000;
    }
}

void BM_DedupFullTextHash(benchmark::State& state) { run(state, false); }
void BM_DedupExchangeKey(benchmark::State& state) { run(state, true); }

}  // namespace

BENCHMARK(BM_DedupFullTextHash)->Arg(200)->Arg(1000)->Arg(4000)->Arg(16000);
BENCHMARK(BM_DedupExchangeKey)->Arg(200)->Arg(1000)->Arg(4000)->Arg(16000);
