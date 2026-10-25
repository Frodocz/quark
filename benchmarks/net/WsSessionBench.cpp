// Client-side cost of the WebSocket hot paths: frame primitives, and one order-sized send through
// WsSession over loopback (masking, framing, TLS record, send syscall), plain and TLS.
#include "TestServers.h"
#include "quark/net/BufferPool.h"
#include "quark/net/EventLoop.h"
#include "quark/net/TcpSocket.h"
#include "quark/net/TlsStream.h"
#include "quark/net/WsFrame.h"
#include "quark/net/WsSession.h"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

using namespace quark;
using namespace quark::net;
using namespace quark::net::test;

namespace {

void BM_WsMask(benchmark::State& state) {
    std::vector<uint8_t> data(static_cast<std::size_t>(state.range(0)), 0x5A);
    const uint8_t key[4] = {1, 2, 3, 4};
    for (auto _ : state) {
        ws_mask(data.data(), data.size(), key);
        benchmark::DoNotOptimize(data.data());
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_WsMask)->Arg(256)->Arg(4096);

void BM_WsParseHeader(benchmark::State& state) {
    const uint8_t frame[] = {0x81, 126, 0x01, 0x00};
    WsFrameHeader h;
    for (auto _ : state) {
        benchmark::DoNotOptimize(ws_parse_header(frame, h));
        benchmark::DoNotOptimize(h);
    }
}
BENCHMARK(BM_WsParseHeader);

// send_text() of a 256-byte order message, the server draining in the background
template <class Transport>
void BM_WsSendOrder(benchmark::State& state) {
    TestServer server(
        [](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            char buf[65536];
            while (!s.stopping() && c.read_some(buf, sizeof(buf)) > 0) {}
        },
        {.tls = std::is_same_v<Transport, TlsStream>});
    EventLoop loop({.busy_poll = true});
    BufferPool recv_pool(2, 64 * 1024, {.huge_pages = MemoryArena::HugePages::Never});
    BufferPool send_pool(256, 1024, {.huge_pages = MemoryArena::HugePages::Never});
    TlsContext tls(trusting_test_ca());
    WsSession<Transport> ws(loop, recv_pool, send_pool);
    if constexpr (std::is_same_v<Transport, TlsStream>) ws.transport().set_context(tls);
    ws.set_host("localhost");
    ws.connect(server.endpoint());
    if (!run_until(loop, [&] { return ws.is_open(); })) {
        state.SkipWithError("connect failed");
        return;
    }
    const std::string order(256, 'o');
    for (auto _ : state) {
        while (ws.send_text(order) != WsSendStatus::Ok) loop.step();  // drain on backpressure
    }
    ws.close();
}
// Baseline for the above: the bare send() of an equally sized (262-byte) frame on the same
// loopback setup. On loopback the kernel runs the receiver's TCP processing inside send(), so
// this is mostly kernel time; WsSendOrder/tcp minus this is the WsSession overhead.
void BM_RawTcpSend(benchmark::State& state) {
    TestServer server([](ServerConn& c, TestServer& s) {
        char buf[65536];
        while (!s.stopping() && c.read_some(buf, sizeof(buf)) > 0) {}
    });
    TcpSocket sock;
    sock.connect(server.endpoint());
    while (sock.poll_open() == StreamPhase::Connecting) {}
    const std::vector<uint8_t> frame(262, 'o');
    for (auto _ : state) {
        while (sock.write(frame, false) == -EAGAIN) {}
    }
}
BENCHMARK(BM_RawTcpSend);

BENCHMARK(BM_WsSendOrder<TcpSocket>)->Name("BM_WsSendOrder/tcp");
BENCHMARK(BM_WsSendOrder<TlsStream>)->Name("BM_WsSendOrder/tls");

} // namespace
