#include "TestServers.h"
#include "quark/net/HttpPool.h"
#include "quark/net/TcpSocket.h"
#include "gtest/gtest.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace quark;
using namespace quark::net;
using namespace quark::net::test;
using namespace std::chrono_literals;

namespace {

/// Answers every request on a connection with its name, after `delay`.
TestServer::Handler named(std::string name, std::chrono::milliseconds delay) {
    return [name, delay](ServerConn& c, TestServer& s) {
        while (!s.stopping()) {
            if (c.read_until("\r\n\r\n").empty()) return;
            std::this_thread::sleep_for(delay);
            c.write_all("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(name.size()) + "\r\n\r\n" + name);
        }
    };
}

std::string body_of(const HttpResponseView* r) {
    return r == nullptr ? "<failed>" : std::string(reinterpret_cast<const char*>(r->body.data()), r->body.size());
}

struct Fixture {
    EventLoop loop;
    BufferPool pool{8, 16 * 1024, {.huge_pages = MemoryArena::HugePages::Never}};
};

}  // namespace

TEST(TestHttpPool, ASlowResponseDoesNotHoldUpTheOtherRequests) {
    TestServer slow(named("slow", 300ms)), fast(named("fast", 0ms));
    Fixture f;
    HttpPool<TcpSocket> pool(f.loop, f.pool, {.clients = 2});
    pool.set_host("localhost");
    pool.connect([&] { return std::vector{slow.endpoint(), fast.endpoint()}; });
    ASSERT_TRUE(run_until(f.loop, [&] { return pool.connected_count() == 2; }));

    std::vector<std::string> done;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(pool.request(HttpMethod::Get, "/" + std::to_string(i), {"Accept: */*"}, {},
                                 [&, i](const HttpResponseView* r) { done.push_back(std::to_string(i) + body_of(r)); }));
    }
    run_until(f.loop, [&] { return done.size() == 4; });
    // request 0 went to the slow server; 1..3 were served by the fast one meanwhile
    EXPECT_EQ(done, (std::vector<std::string>{"1fast", "2fast", "3fast", "0slow"}));
    EXPECT_EQ(pool.queued(), 0u);
}

TEST(TestHttpPool, RequestsWaitForAClientToConnect) {
    TestServer server(named("ok", 0ms));
    Fixture f;
    HttpPool<TcpSocket> pool(f.loop, f.pool, {.clients = 1});
    pool.set_host("localhost");
    std::string got;
    pool.connect([&] { return std::vector{server.endpoint()}; });
    ASSERT_TRUE(pool.request(HttpMethod::Get, "/", {}, {}, [&](const HttpResponseView* r) { got = body_of(r); }));
    EXPECT_EQ(pool.queued(), 1u);  // still connecting
    EXPECT_TRUE(run_until(f.loop, [&] { return !got.empty(); }));
    EXPECT_EQ(got, "ok");
}

TEST(TestHttpPool, QueueIsBoundedAndWaitingRequestsTimeOut) {
    Fixture f;
    HttpPoolOptions o{.clients = 1, .max_queued = 2, .queue_timeout_ns = 50'000'000};
    o.client.reconnect_initial_ns = 20'000'000;
    HttpPool<TcpSocket> pool(f.loop, f.pool, o);
    pool.set_host("localhost");
    uint16_t closed_port = 0;
    {
        TestServer gone(named("x", 0ms));
        closed_port = gone.port();
    }
    pool.connect([&] { return std::vector{*Endpoint::from_ip("127.0.0.1", closed_port)}; });
    int failed = 0;
    auto on_done = [&](const HttpResponseView* r) { failed += r == nullptr; };
    EXPECT_TRUE(pool.request(HttpMethod::Get, "/a", {}, {}, on_done));
    EXPECT_TRUE(pool.request(HttpMethod::Get, "/b", {}, {}, on_done));
    EXPECT_FALSE(pool.request(HttpMethod::Get, "/c", {}, {}, on_done));
    EXPECT_TRUE(run_until(f.loop, [&] { return failed == 2; }, 1000ms));
    EXPECT_EQ(pool.queued(), 0u);
}

TEST(TestHttpPool, ADroppedConnectionFailsItsRequest) {
    TestServer server([](ServerConn& c, TestServer&) { c.read_until("\r\n\r\n"); });  // closes without answering
    Fixture f;
    HttpPool<TcpSocket> pool(f.loop, f.pool, {.clients = 1});
    pool.set_host("localhost");
    pool.connect([&] { return std::vector{server.endpoint()}; });
    ASSERT_TRUE(run_until(f.loop, [&] { return pool.connected_count() == 1; }));
    std::string got;
    pool.request(HttpMethod::Post, "/order", {"Content-Type: text/plain"}, "x=1", [&](const HttpResponseView* r) { got = body_of(r); });
    EXPECT_TRUE(run_until(f.loop, [&] { return !got.empty(); }));
    EXPECT_EQ(got, "<failed>");
}
