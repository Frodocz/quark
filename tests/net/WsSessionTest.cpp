#include "TestServers.h"
#include "quark/net/BufferPool.h"
#include "quark/net/EventLoop.h"
#include "quark/net/TcpSocket.h"
#include "quark/net/TlsStream.h"
#include "quark/net/WsSession.h"
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace quark;
using namespace quark::net;
using namespace quark::net::test;
using namespace std::chrono_literals;

namespace {

struct Params {
    bool tls;
    bool busy_poll;
};

// One client session wired to a loop and pools, over TLS or plain TCP.
template <class Transport>
struct Client {
    using Session = WsSession<Transport>;
    EventLoop loop;
    BufferPool recv_pool;
    BufferPool send_pool;
    TlsContext tls_ctx{trusting_test_ca()};
    std::unique_ptr<Session> ws;
    std::vector<std::string> messages;
    std::vector<WsOpcode> opcodes;
    int opened{0};
    bool padding_ok{true};
    int64_t last_recv_ns{0};

    Client(bool busy_poll, WsSessionOptions opts, std::size_t recv_buf = 64 * 1024, std::size_t send_buf = 4096)
        : loop{{.busy_poll = busy_poll}},
          recv_pool{8, recv_buf, {.huge_pages = MemoryArena::HugePages::Never}},
          send_pool{64, send_buf, {.huge_pages = MemoryArena::HugePages::Never}} {
        ws = std::make_unique<Session>(loop, recv_pool, send_pool, opts);
        if constexpr (std::is_same_v<Transport, TlsStream>) ws->transport().set_context(tls_ctx);
        ws->set_host("localhost");
        ws->set_target("/ws?streams=btcusdt@trade");
        ws->add_header("X-Api-Key", "test-key");
        ws->on_message([this](const typename Session::Message& m) {
            messages.emplace_back(m.text());
            opcodes.push_back(m.opcode);
            padding_ok = padding_ok && m.capacity >= m.payload.size() + Session::kPadding;
            if (m.recv_ns <= 0 || m.recv_ns < last_recv_ns) padding_ok = false;
            last_recv_ns = m.recv_ns;
        });
        ws->on_state([this](typename Session::State s) { opened += s == Session::State::Open; });
    }
};

WsSessionOptions fast_reconnect() {
    return {.reconnect_initial_ns = 20'000'000, .reconnect_max_ns = 100'000'000};
}

}  // namespace

class TestWsSession : public ::testing::TestWithParam<Params> {
protected:
    template <class Fn>
    void with_transport(Fn&& fn) {
        if (GetParam().tls) {
            fn.template operator()<TlsStream>();
        } else {
            fn.template operator()<TcpSocket>();
        }
    }
    TestServer::Options server_opts() const { return {.tls = GetParam().tls}; }
    bool busy() const { return GetParam().busy_poll; }
};

TEST_P(TestWsSession, EchoTextAndBinary) {
    std::string request;
    TestServer server(
        [&](ServerConn& c, TestServer& s) {
            request = c.accept_websocket();
            while (!s.stopping()) {
                auto f = c.read_frame();
                if (!f || f->opcode == WsOpcode::Close) return;
                c.send_frame(f->opcode, f->payload);
            }
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client(busy(), {});
        client.ws->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.ws->is_open(); }));
        EXPECT_EQ(client.ws->last_upgrade_status(), 101);

        const std::string big(3000, 'z');
        EXPECT_EQ(client.ws->send_text("hello"), WsSession<T>::SendStatus::Ok);
        EXPECT_EQ(client.ws->send_binary({reinterpret_cast<const uint8_t*>(big.data()), big.size()}),
                  WsSession<T>::SendStatus::Ok);
        EXPECT_EQ(client.ws->send_text(""), WsSession<T>::SendStatus::Ok);
        ASSERT_TRUE(run_until(client.loop, [&] { return client.messages.size() == 3; }));
        EXPECT_EQ(client.messages[0], "hello");
        EXPECT_EQ(client.messages[1], big);
        EXPECT_EQ(client.messages[2], "");
        EXPECT_EQ(client.opcodes[1], WsOpcode::Binary);
        EXPECT_TRUE(client.padding_ok) << "capacity/timestamp contract broken";

        // the upgrade request: target, Host with the non-default port, custom header, fresh key
        EXPECT_TRUE(request.starts_with("GET /ws?streams=btcusdt@trade HTTP/1.1\r\n"));
        EXPECT_EQ(ServerConn::header_value(request, "Host"), "localhost:" + std::to_string(server.port()));
        EXPECT_EQ(ServerConn::header_value(request, "X-Api-Key"), "test-key");
        EXPECT_NE(ServerConn::header_value(request, "Sec-WebSocket-Key"), "dGhlIHNhbXBsZSBub25jZQ==");

        std::string too_big(5000, 'x');
        EXPECT_EQ(client.ws->send_text(too_big), WsSession<T>::SendStatus::TooLarge);
    });
}

TEST_P(TestWsSession, FragmentsReassembledAndServerPingAnswered) {
    std::atomic<bool> pong_ok{false};
    TestServer server(
        [&](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            c.send_frame(WsOpcode::Text, "hel", false);
            c.send_frame(WsOpcode::Ping, "p1");  // control frames may interleave fragments
            c.send_frame(WsOpcode::Continuation, "lo", true);
            auto f = c.read_frame();
            pong_ok = f && f->opcode == WsOpcode::Pong && f->payload == "p1" && f->masked;
            s.hold();
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client(busy(), {});
        client.ws->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.messages.size() == 1 && pong_ok; }));
        EXPECT_EQ(client.messages[0], "hello");
        EXPECT_TRUE(client.padding_ok);
    });
}

// Regression (fix D): a 101 and the first frame in the same segment. Over TLS in epoll mode the
// frame used to stay in OpenSSL's buffers: the socket was already drained, so no edge came.
TEST_P(TestWsSession, UpgradeResponseAndFirstFrameInOneSegment) {
    TestServer server(
        [&](ServerConn& c, TestServer& s) {
            const std::string req = c.read_until("\r\n\r\n");
            char accept[28];
            compute_ws_accept(ServerConn::header_value(req, "Sec-WebSocket-Key"), accept);
            c.cork(true);
            c.write_all("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " + std::string(accept, 28) + "\r\n\r\n");
            c.send_frame(WsOpcode::Text, R"({"e":"trade","p":"100.5"})");
            c.send_frame(WsOpcode::Text, R"({"e":"trade","p":"100.6"})");
            c.cork(false);
            s.hold();
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client(busy(), {});
        client.ws->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.messages.size() == 2; }, 2000ms))
            << "delivered " << client.messages.size() << " of 2";
    });
}

// Regression (fix C): a rejected upgrade (e.g. 429 rate limit) used to count as open, and the
// response body was delivered as a WebSocket message.
TEST_P(TestWsSession, RejectedUpgradeNeverOpens) {
    TestServer server(
        [&](ServerConn& c, TestServer&) {
            c.read_until("\r\n\r\n");
            c.write_all("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 4\r\n\r\n\x81\x02hi");
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client(busy(), fast_reconnect());
        client.ws->connect(server.endpoint());
        run_for(client.loop, 300ms);
        EXPECT_EQ(client.opened, 0);
        EXPECT_TRUE(client.messages.empty());
        EXPECT_EQ(client.ws->last_upgrade_status(), 429);
        EXPECT_GE(server.accepted(), 2);  // it does keep retrying, with backoff
    });
}

// Regression (fix B): a server that accepts and drops at once used to be reconnected to in a
// tight loop (198 connections in 4 s), since the backoff was reset as soon as the session opened.
TEST_P(TestWsSession, BackoffSurvivesConnectionsThatDropRightAway) {
    TestServer server([&](ServerConn& c, TestServer&) { c.accept_websocket(); }, server_opts());
    with_transport([&]<class T>() {
        WsSessionOptions opts{.reconnect_initial_ns = 50'000'000, .reconnect_max_ns = 400'000'000,
                                            .stable_after_ns = 10'000'000'000};
        Client<T> client(busy(), opts);
        client.ws->connect(server.endpoint());
        run_for(client.loop, 1500ms);
        // ~50, 100, 200, 400, 400 ms apart (+-20%): about 6 connections, never dozens
        EXPECT_GE(server.accepted(), 3);
        EXPECT_LE(server.accepted(), 8);
        EXPECT_GE(client.opened, 3);
    });
}

// Regression (fix E): in epoll mode a closed session never got another event, so it never
// reconnected. Also: the server's Close is echoed before reconnecting.
TEST_P(TestWsSession, ServerCloseIsEchoedThenReconnects) {
    std::atomic<bool> echoed{false};
    TestServer server(
        [&](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            if (s.accepted() == 1) {
                c.send_frame(WsOpcode::Close, std::string("\x03\xE9", 2));  // 1001 going away
                auto f = c.read_frame();
                echoed = f && f->opcode == WsOpcode::Close && f->payload == std::string("\x03\xE9", 2);
                return;
            }
            s.hold();
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client(busy(), fast_reconnect());
        client.ws->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.opened == 2 && client.ws->is_open(); }));
        EXPECT_TRUE(echoed);
        EXPECT_EQ(server.accepted(), 2);
    });
}

TEST_P(TestWsSession, UserCloseSendsCloseAndStaysClosed) {
    std::atomic<bool> got_close{false};
    TestServer server(
        [&](ServerConn& c, TestServer&) {
            c.accept_websocket();
            auto f = c.read_frame();
            got_close = f && f->opcode == WsOpcode::Close;
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client(busy(), fast_reconnect());
        client.ws->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.ws->is_open(); }));
        client.ws->close();
        run_for(client.loop, 300ms);
        EXPECT_TRUE(got_close);
        EXPECT_EQ(client.ws->state(), WsSession<T>::State::Disconnected);
        EXPECT_EQ(server.accepted(), 1);
    });
}

TEST_P(TestWsSession, MessageLargerThanTheBufferFailsCleanly) {
    TestServer server(
        [&](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            if (s.accepted() == 1) {
                c.send_frame(WsOpcode::Text, std::string(10000, 'x'));
                while (c.read_frame()) {}  // until the client drops the connection
                return;
            }
            c.send_frame(WsOpcode::Text, "small");
            s.hold();
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client(busy(), fast_reconnect(), /*recv_buf=*/4096);
        client.ws->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.messages.size() == 1; }));
        EXPECT_EQ(client.messages[0], "small");  // the oversized one was refused, not truncated
        EXPECT_EQ(server.accepted(), 2);
    });
}

TEST_P(TestWsSession, SendBackpressureWhenThePeerStopsReading) {
    TestServer server(
        [&](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            s.hold();  // never reads
        },
        {.tls = GetParam().tls, .rcvbuf = 4096});
    with_transport([&]<class T>() {
        Client<T> client(busy(), {});
        client.ws->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.ws->is_open(); }));
        if constexpr (std::is_same_v<T, TlsStream>) {
            client.ws->transport().tcp().set_send_buf(4096);
        } else {
            client.ws->transport().set_send_buf(4096);
        }
        const std::string payload(2000, 'o');
        int sent = 0;
        auto status = WsSession<T>::SendStatus::Ok;
        while (sent < 100'000 && (status = client.ws->send_text(payload)) == WsSession<T>::SendStatus::Ok) ++sent;
        EXPECT_EQ(status, WsSession<T>::SendStatus::WouldBlock) << "sent " << sent << " frames without pushback";
        EXPECT_GT(client.ws->pending_frames(), 0u);
        EXPECT_TRUE(client.ws->is_open());  // backpressure, not a failure
    });
}

TEST_P(TestWsSession, PongTimeoutReconnects) {
    std::atomic<int> pings{0};
    TestServer server(
        [&](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            while (!s.stopping()) {  // reads pings, never answers
                auto f = c.read_frame();
                if (!f) return;
                pings += f->opcode == WsOpcode::Ping;
            }
        },
        server_opts());
    with_transport([&]<class T>() {
        WsSessionOptions opts{.reconnect_initial_ns = 20'000'000,
                                            .ping_interval_ns = 30'000'000,
                                            .pong_timeout_ns = 60'000'000};
        Client<T> client(busy(), opts);
        client.ws->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return server.accepted() >= 2; }, 2000ms));
        EXPECT_GE(pings.load(), 1);
    });
}

INSTANTIATE_TEST_SUITE_P(Transports, TestWsSession,
                         ::testing::Values(Params{false, true}, Params{false, false}, Params{true, true},
                                           Params{true, false}),
                         [](const auto& info) {
                             return std::string(info.param.tls ? "Tls" : "Tcp") + (info.param.busy_poll ? "BusyPoll" : "Epoll");
                         });
