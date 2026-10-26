// Per-message costs of the protocol codecs, without any I/O: WebSocket frame headers and masking,
// HTTP response parsing and request building.
#include "quark/net/HttpCodec.h"
#include "quark/net/WsFrame.h"

#include <benchmark/benchmark.h>

#include <cstring>
#include <string>
#include <vector>

using namespace quark::net;

namespace {

constexpr uint8_t kKey[4] = {0x12, 0x34, 0x56, 0x78};

// a server frame header of each length encoding: 7-bit, 16-bit, 64-bit
void BM_WsParseHeader(benchmark::State& state) {
    const std::size_t len = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> frame(14 + len);
    frame[0] = 0x81;
    std::size_t pos = 1;
    if (len < 126) {
        frame[pos++] = static_cast<uint8_t>(len);
    } else if (len <= 0xFFFF) {
        frame[pos++] = 126;
        frame[pos++] = static_cast<uint8_t>(len >> 8);
        frame[pos++] = static_cast<uint8_t>(len);
    } else {
        frame[pos++] = 127;
        for (int i = 7; i >= 0; --i) frame[pos++] = static_cast<uint8_t>(len >> (8 * i));
    }
    for (auto _ : state) {
        WsFrameHeader h;
        benchmark::DoNotOptimize(ws_parse_header(frame, h));
        benchmark::DoNotOptimize(h);
    }
}
BENCHMARK(BM_WsParseHeader)->Arg(100)->Arg(1000)->Arg(70'000);

void BM_WsEncodeClientHeader(benchmark::State& state) {
    uint8_t out[14];
    std::size_t len = 200;
    for (auto _ : state) {
        benchmark::DoNotOptimize(ws_encode_client_header(out, WsOpcode::Text, len, kKey, true));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_WsEncodeClientHeader);

// the send path: the payload copied into the frame buffer, then masked in place (a fused
// mask-while-copying loop measured no faster than glibc's memcpy plus this pass)
void BM_WsCopyThenMask(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> src(n, 'a'), dst(n);
    for (auto _ : state) {
        std::memcpy(dst.data(), src.data(), n);
        ws_mask(dst.data(), n, kKey);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * n));
}
BENCHMARK(BM_WsCopyThenMask)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

const std::string kJsonBody = R"({"symbol":"BTCUSDT","orderId":28,"clientOrderId":"6gCrw2kRUAF9CvJDGP16IP",)"
                              R"("transactTime":1507725176595,"price":"0.10000000","origQty":"10.00000000",)"
                              R"("executedQty":"10.00000000","status":"FILLED","timeInForce":"GTC","type":"LIMIT","side":"SELL"})";

// a typical REST order response
void BM_HttpParseContentLength(benchmark::State& state) {
    const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json;charset=UTF-8\r\nContent-Length: " +
                             std::to_string(kJsonBody.size()) +
                             "\r\nConnection: keep-alive\r\nDate: Thu, 01 Jan 2026 00:00:00 GMT\r\n"
                             "x-mbx-used-weight: 1\r\nx-mbx-used-weight-1m: 1\r\n\r\n" + kJsonBody;
    std::vector<uint8_t> buf(resp.begin(), resp.end());
    for (auto _ : state) {
        auto r = parse_http_response(buf);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_HttpParseContentLength);

// the same body chunked in three (decoded in place: the buffer is restored every iteration)
void BM_HttpParseChunked(benchmark::State& state) {
    const std::size_t third = kJsonBody.size() / 3;
    auto hex = [](std::size_t v) {
        char b[16];
        return std::string(b, static_cast<std::size_t>(std::snprintf(b, sizeof(b), "%zx", v)));
    };
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (std::size_t off = 0; off < kJsonBody.size(); off += third) {
        const std::string part = kJsonBody.substr(off, third);
        resp += hex(part.size()) + "\r\n" + part + "\r\n";
    }
    resp += "0\r\n\r\n";
    const std::vector<uint8_t> original(resp.begin(), resp.end());
    std::vector<uint8_t> buf = original;
    for (auto _ : state) {
        std::memcpy(buf.data(), original.data(), original.size());
        auto r = parse_http_response(buf);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_HttpParseChunked);

// a signed REST order request
void BM_HttpBuildRequest(benchmark::State& state) {
    char out[1024];
    const std::string_view headers[] = {"X-MBX-APIKEY: vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A",
                                        "Content-Type: application/x-www-form-urlencoded"};
    const std::string body = "symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000"
                             "&timestamp=1499827319559&signature=c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71";
    for (auto _ : state) {
        benchmark::DoNotOptimize(build_http_request(out, HttpMethod::Post, "fapi.binance.com", "/fapi/v1/order", headers,
                                                    {reinterpret_cast<const uint8_t*>(body.data()), body.size()}));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_HttpBuildRequest);

}  // namespace
