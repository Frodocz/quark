#include "TestServers.h"
#include "quark/net/BufferPool.h"
#include "quark/net/EventLoop.h"
#include "quark/net/HttpClient.h"
#include "quark/net/TcpSocket.h"
#include "quark/net/TlsStream.h"
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

template <class Transport>
struct Client {
    EventLoop loop{{.busy_poll = true}};
    BufferPool pool{4, 16 * 1024, {.huge_pages = MemoryArena::HugePages::Never}};
    TlsContext tls_ctx{trusting_test_ca()};
    std::unique_ptr<HttpClient<Transport>> http;
    std::vector<std::pair<int, std::string>> responses;
    int errors{0};

    explicit Client(HttpClientOptions opts = {.reconnect_initial_ns = 20'000'000}) {
        http = std::make_unique<HttpClient<Transport>>(loop, pool, opts);
        if constexpr (std::is_same_v<Transport, TlsStream>) http->transport().set_context(tls_ctx);
        http->set_host("localhost");
        http->on_response([this](const HttpResponseView& r) {
            responses.emplace_back(r.status, std::string(reinterpret_cast<const char*>(r.body.data()), r.body.size()));
        });
        http->on_error([this] { ++errors; });
    }

    HttpSubmit get(std::string_view target) {
        const std::string_view headers[] = {"Accept: application/json"};
        return http->request(HttpMethod::Get, target, headers);
    }
};

// serves `responses` in order on one connection, then holds or closes it
void serve(ServerConn& c, TestServer& s, const std::vector<std::string>& responses, bool hold) {
    for (const auto& resp : responses) {
        if (c.read_until("\r\n\r\n").empty()) return;
        c.write_all(resp);
    }
    if (hold) s.hold();
}

}  // namespace

class TestHttpClient : public ::testing::TestWithParam<bool> {
protected:
    template <class Fn>
    void with_transport(Fn&& fn) {
        if (GetParam()) {
            fn.template operator()<TlsStream>();
        } else {
            fn.template operator()<TcpSocket>();
        }
    }
    TestServer::Options server_opts() const { return {.tls = GetParam()}; }
};

TEST_P(TestHttpClient, KeepAliveReusesTheConnection) {
    TestServer server(
        [](ServerConn& c, TestServer& s) {
            serve(c, s,
                  {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst",
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nsec\r\n3\r\nond\r\n0\r\n\r\n"},
                  true);
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client;
        client.http->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.http->ready(); }));
        ASSERT_EQ(client.get("/api/v3/depth?symbol=BTCUSDT"), HttpSubmit::Ok);
        EXPECT_EQ(client.get("/again"), HttpSubmit::Busy);  // one request in flight
        ASSERT_TRUE(run_until(client.loop, [&] { return client.responses.size() == 1 && client.http->ready(); }));
        ASSERT_EQ(client.get("/second"), HttpSubmit::Ok);
        ASSERT_TRUE(run_until(client.loop, [&] { return client.responses.size() == 2; }));
        EXPECT_EQ(client.responses[0], std::make_pair(200, std::string("first")));
        EXPECT_EQ(client.responses[1], std::make_pair(200, std::string("second")));
        EXPECT_EQ(server.accepted(), 1);
        EXPECT_EQ(client.errors, 0);
    });
}

TEST_P(TestHttpClient, ConnectionCloseResponseReconnects) {
    TestServer server(
        [](ServerConn& c, TestServer& s) {
            if (s.accepted() == 1) {
                serve(c, s, {"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok"}, false);
            } else {
                serve(c, s, {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain"}, true);
            }
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client;
        client.http->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.http->ready(); }));
        ASSERT_EQ(client.get("/a"), HttpSubmit::Ok);
        ASSERT_TRUE(run_until(client.loop, [&] { return client.responses.size() == 1; }));
        ASSERT_TRUE(run_until(client.loop, [&] { return server.accepted() == 2 && client.http->ready(); }));
        ASSERT_EQ(client.get("/b"), HttpSubmit::Ok);
        ASSERT_TRUE(run_until(client.loop, [&] { return client.responses.size() == 2; }));
        EXPECT_EQ(client.responses[1].second, "again");
    });
}

// The server closes an idle keep-alive connection (common behind load balancers): the client
// notices and reopens it before the next request, instead of failing that request.
TEST_P(TestHttpClient, IdleCloseIsDetectedBeforeTheNextRequest) {
    TestServer server(
        [](ServerConn& c, TestServer& s) {
            if (s.accepted() == 1) {
                serve(c, s, {"HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n1"}, false);
                std::this_thread::sleep_for(50ms);  // then drop it while idle
            } else {
                serve(c, s, {"HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n2"}, true);
            }
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client;
        client.http->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.http->ready(); }));
        ASSERT_EQ(client.get("/1"), HttpSubmit::Ok);
        ASSERT_TRUE(run_until(client.loop, [&] { return client.responses.size() == 1; }));
        ASSERT_TRUE(run_until(client.loop, [&] { return server.accepted() == 2 && client.http->ready(); }));
        ASSERT_EQ(client.get("/2"), HttpSubmit::Ok);
        ASSERT_TRUE(run_until(client.loop, [&] { return client.responses.size() == 2; }));
        EXPECT_EQ(client.responses[1].second, "2");
        EXPECT_EQ(client.errors, 0);
    });
}

TEST_P(TestHttpClient, RequestTimeoutReportsAnError) {
    TestServer server(
        [](ServerConn& c, TestServer& s) {
            c.read_until("\r\n\r\n");  // never answers
            s.hold();
        },
        server_opts());
    with_transport([&]<class T>() {
        Client<T> client({.request_timeout_ns = 100'000'000, .reconnect_initial_ns = 20'000'000});
        client.http->connect(server.endpoint());
        ASSERT_TRUE(run_until(client.loop, [&] { return client.http->ready(); }));
        ASSERT_EQ(client.get("/slow"), HttpSubmit::Ok);
        ASSERT_TRUE(run_until(client.loop, [&] { return client.errors == 1; }, 2000ms));
        EXPECT_TRUE(client.responses.empty());
    });
}

INSTANTIATE_TEST_SUITE_P(Transports, TestHttpClient, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Tls" : "Tcp"; });
