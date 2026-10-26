// Subscribes to Binance market data (WebSocket over TLS) and prints a line per stream once a
// second. The same code runs on kernel sockets and, built with QUARK_USE_FSTACK, through the DPDK
// port; only the F-Stack initialization below differs.
//
//     ./quark_example_binance_ws [--count N] [streams]
//     sudo ./quark_example_binance_ws --conf f-stack.conf [--count N] [streams]   (F-Stack build)
//
// streams: Binance stream names joined by '/', default "btcusdt@bookTicker/btcusdt@trade".
// Stops after N messages (--count) or on Ctrl-C. Binance refuses connections from some regions
// (HTTP 451 on the upgrade, printed below).

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

#include "quark/net/BufferPool.h"
#include "quark/net/Endpoint.h"
#include "quark/net/EventLoop.h"
#include "quark/net/TlsContext.h"
#include "quark/net/TlsStream.h"
#include "quark/net/WsSession.h"
#include "quark/utils/JsonParser.h"
#include "quark/utils/System.h"
#if defined(QUARK_USE_FSTACK)
#include "quark/net/FStack.h"
#endif

using namespace quark;
using namespace quark::net;

namespace {

constexpr std::string_view kHost = "stream.binance.com";
constexpr uint16_t kPort = 9443;
#if defined(QUARK_USE_FSTACK)
constexpr const char* kBackend = "F-Stack (DPDK)";
#else
constexpr const char* kBackend = "kernel sockets";
#endif

struct StreamStats {
    uint64_t messages{0};      // since the last report
    uint64_t total{0};
    std::string bid, ask;      // bookTicker
    std::string price, qty;    // trade
};

const char* state_name(WsState s) {
    switch (s) {
        case WsState::Disconnected: return "disconnected";
        case WsState::Connecting: return "connecting";
        case WsState::Upgrading: return "upgrading";
        case WsState::Open: return "open";
        case WsState::Closing: return "closing";
    }
    return "?";
}

// Combined stream message: {"stream":"btcusdt@trade","data":{...}}. Parsed in place: the receive
// buffer has the padding simdjson needs (WsMessage::kPadding), so nothing is copied.
bool handle(JsonParser& parser, const WsMessage& m, std::map<std::string, StreamStats, std::less<>>& streams) {
    auto doc = parser.parse_in_place(reinterpret_cast<const char*>(m.payload.data()), m.payload.size(), m.capacity);
    std::string_view stream;
    simdjson::ondemand::object data;
    if (doc["stream"].get_string().get(stream) != simdjson::SUCCESS) return false;
    if (doc["data"].get_object().get(data) != simdjson::SUCCESS) return false;

    auto it = streams.find(stream);
    if (it == streams.end()) it = streams.emplace(std::string{stream}, StreamStats{}).first;
    StreamStats& st = it->second;
    ++st.messages;
    ++st.total;

    std::string_view a, b;  // assign() reuses the strings' capacity: no allocation after the first
    if (stream.ends_with("@bookTicker")) {
        if (data["b"].get_string().get(a) == simdjson::SUCCESS) st.bid.assign(a);
        if (data["a"].get_string().get(b) == simdjson::SUCCESS) st.ask.assign(b);
    } else if (stream.ends_with("@trade")) {
        if (data["p"].get_string().get(a) == simdjson::SUCCESS) st.price.assign(a);
        if (data["q"].get_string().get(b) == simdjson::SUCCESS) st.qty.assign(b);
    }
    return true;
}

void report(std::map<std::string, StreamStats, std::less<>>& streams) {
    for (auto& [name, st] : streams) {
        if (!st.bid.empty()) {
            std::printf("%-24s %6lu msg/s  bid %s  ask %s\n", name.c_str(), st.messages, st.bid.c_str(), st.ask.c_str());
        } else if (!st.price.empty()) {
            std::printf("%-24s %6lu msg/s  last %s x %s\n", name.c_str(), st.messages, st.price.c_str(), st.qty.c_str());
        } else {
            std::printf("%-24s %6lu msg/s\n", name.c_str(), st.messages);
        }
        st.messages = 0;
    }
    std::fflush(stdout);
}

int usage(const char* prog) {
    std::fprintf(stderr, "usage: %s [--conf f-stack.conf] [--count N] [streams]\n", prog);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string conf;
    uint64_t max_messages = 0;  // 0: until Ctrl-C
    std::string streams_arg = "btcusdt@bookTicker/btcusdt@trade";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--conf" && i + 1 < argc) {
            conf = argv[++i];
        } else if (arg == "--count" && i + 1 < argc) {
            max_messages = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg.starts_with("-")) {
            return usage(argv[0]);
        } else {
            streams_arg = arg;
        }
    }

#if defined(QUARK_USE_FSTACK)
    // F-Stack rejects options it doesn't know: hand it only its own.
    if (conf.empty()) return usage(argv[0]);
    std::string conf_flag = "--conf";
    char* fstack_argv[] = {argv[0], conf_flag.data(), conf.data(), nullptr};
    if (!init_fstack(3, fstack_argv)) return 1;
#else
    if (!conf.empty()) std::fprintf(stderr, "--conf ignored: built without QUARK_USE_FSTACK\n");
#endif
    install_shutdown_handler();

    // DNS goes through the kernel in both builds (under F-Stack: the instance's primary ENI)
    const auto ep = Endpoint::resolve(kHost, kPort);
    if (!ep) {
        std::fprintf(stderr, "cannot resolve %.*s\n", static_cast<int>(kHost.size()), kHost.data());
        return 1;
    }

    EventLoop loop;  // busy-poll
    BufferPool recv_pool(4, 64 * 1024);
    BufferPool send_pool(4, 4 * 1024);
    TlsContext tls;  // system CAs, certificate verification on
    JsonParser parser;
    std::map<std::string, StreamStats, std::less<>> streams;
    uint64_t total = 0;
    uint64_t parse_errors = 0;

    WsSession<TlsStream> ws(loop, recv_pool, send_pool);
    ws.transport().set_context(tls);
    ws.set_host(kHost);
    const std::string target = "/stream?streams=" + streams_arg;
    ws.set_target(target);
    ws.on_state([&](WsState s) {
        std::printf("[%s] %s\n", ep->to_string().c_str(), state_name(s));
        if (s == WsState::Disconnected && ws.last_upgrade_status() > 101) {
            std::printf("upgrade refused: HTTP %d\n", ws.last_upgrade_status());
        }
        std::fflush(stdout);
    });
    ws.on_message([&](const WsMessage& m) {
        ++total;
        if (!handle(parser, m, streams)) ++parse_errors;
    });

    std::printf("subscribing to wss://%.*s:%u%s over %s\n", static_cast<int>(kHost.size()), kHost.data(), kPort,
                target.c_str(), kBackend);
    ws.connect(*ep);

    loop.timers().add_task("report", [&] { report(streams); return true; }, loop.now_ns() + 1'000'000'000,
                           1'000'000'000);

    bool closing = false;
    loop.set_step_hook([&] {
        if (!closing && (shutdown_requested() || (max_messages != 0 && total >= max_messages))) {
            closing = true;
            ws.close();  // Close frame, then disconnect (at most close_timeout_ns)
        }
        if (closing && ws.state() == WsState::Disconnected) loop.stop();
    });
    loop.run();

    for (const auto& [name, st] : streams) std::printf("%-24s %6lu messages\n", name.c_str(), st.total);
    std::printf("%lu messages, %lu unparsed\n", total, parse_errors);
    return total > 0 && parse_errors == 0 ? 0 : 1;
}
