// Request signing and encoding costs (per signed order / logon).
#include "quark/crypto/Base64Encoding.h"
#include "quark/crypto/Ed25519Signer.h"
#include "quark/crypto/HmacSha256.h"

#include <benchmark/benchmark.h>

#include <array>
#include <string>

using namespace quark;
using namespace quark::crypto;

namespace {

void BM_HmacSha256Sign(benchmark::State& state) {
    HmacSha256 h("NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j");
    const std::string msg(static_cast<std::size_t>(state.range(0)), 'q');
    for (auto _ : state) benchmark::DoNotOptimize(h.sign(msg));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * msg.size()));
}
BENCHMARK(BM_HmacSha256Sign)->Arg(64)->Arg(256)->Arg(1024);

// the signature as hex, as REST query strings carry it
void BM_HmacSha256SignHex(benchmark::State& state) {
    HmacSha256 h("NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j");
    const std::string msg(256, 'q');
    char out[64];
    for (auto _ : state) {
        benchmark::DoNotOptimize(h.sign_hex(msg, out));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_HmacSha256SignHex);

void BM_Ed25519Sign(benchmark::State& state) {
    std::array<uint8_t, Ed25519Signer::kKeySize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i * 7 + 1);
    auto signer = Ed25519Signer::from_private_key(key);
    const std::string msg(static_cast<std::size_t>(state.range(0)), 'q');
    for (auto _ : state) benchmark::DoNotOptimize(signer.sign(msg));
}
BENCHMARK(BM_Ed25519Sign)->Arg(128)->Arg(512);

void BM_Base64Encode(benchmark::State& state) {
    const std::vector<uint8_t> in(static_cast<std::size_t>(state.range(0)), 0xA5);
    char out[256];
    for (auto _ : state) {
        benchmark::DoNotOptimize(base64_encode(in, out));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_Base64Encode)->Arg(32)->Arg(64);  // an HMAC digest, an Ed25519 signature

void BM_HexEncode(benchmark::State& state) {
    const std::vector<uint8_t> in(32, 0xA5);
    char out[64];
    for (auto _ : state) {
        benchmark::DoNotOptimize(hex_encode(in, out));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_HexEncode);

}  // namespace
