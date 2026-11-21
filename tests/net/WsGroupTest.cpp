#include "TestServers.h"
#include "quark/net/TcpSocket.h"
#include "quark/net/WsGroup.h"
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>

using namespace quark;
using namespace quark::net;
using namespace quark::net::test;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

/// A "venue IP": sends m0, m1, ... at t0 + k * 10 ms (+ its delay) until the test ends.
TestServer::Handler streamer(Clock::time_point t0, std::chrono::milliseconds delay) {
    return [t0, delay](ServerConn& c, TestServer& s) {
        c.accept_websocket();
        for (int k = 0; !s.stopping(); ++k) {
            std::this_thread::sleep_until(t0 + k * 10ms + delay);
            if (!c.send_frame(WsOpcode::Text, "m" + std::to_string(k))) return;
        }
    };
}

struct Pools {
    BufferPool recv{16, 64 * 1024, {.huge_pages = MemoryArena::HugePages::Never}};
    BufferPool send{64, 4096, {.huge_pages = MemoryArena::HugePages::Never}};
};

}  // namespace

TEST(TestWsGroup, DeliversEachMessageOnceAndMeasuresTheSlowSession) {
    const auto t0 = Clock::now() + 200ms;
    TestServer fast(streamer(t0, 0ms)), slow(streamer(t0, 30ms));
    EventLoop loop;
    Pools p;
    WsGroup<TcpSocket> g(loop, p.recv, p.send, {.sessions = 2, .rotate_interval_ns = 0, .delay_ema_alpha = 0.2});
    g.set_host("localhost");
    std::vector<std::string> got;
    std::set<std::size_t> from;
    g.on_message([&](const WsMessage& m, std::size_t s) {
        got.emplace_back(m.text());
        from.insert(s);
    });
    g.connect([&] { return std::vector{fast.endpoint(), slow.endpoint()}; });
    run_until(loop, [&] { return got.size() >= 30; }, 3000ms);
    ASSERT_GE(got.size(), 30u);
    for (std::size_t i = 0; i < got.size(); ++i) EXPECT_EQ(got[i], "m" + std::to_string(i));  // once each, in order
    const std::size_t fast_i = g.address(0) == fast.endpoint() ? 0 : 1;
    EXPECT_GT(g.delay_ns(1 - fast_i), 15e6);  // ~30 ms behind
    EXPECT_LT(g.delay_ns(fast_i), 5e6);
    EXPECT_EQ(from, std::set<std::size_t>{fast_i});  // the slow copies never won
}

TEST(TestWsGroup, SlowestSessionMovesToASpareAddress) {
    const auto t0 = Clock::now() + 200ms;
    TestServer fast(streamer(t0, 0ms)), slow(streamer(t0, 30ms)), spare(streamer(t0, 0ms));
    EventLoop loop;
    Pools p;
    WsGroup<TcpSocket> g(loop, p.recv, p.send,
                         {.sessions = 2, .rotate_interval_ns = 400'000'000, .warmup_ns = 300'000'000, .delay_ema_alpha = 0.2});
    g.set_host("localhost");
    int delivered = 0;
    g.on_message([&](const WsMessage&, std::size_t) { ++delivered; });
    g.connect([&] { return std::vector{fast.endpoint(), slow.endpoint(), spare.endpoint()}; });
    run_until(loop, [&] { return spare.accepted() >= 1; }, 3000ms);
    run_for(loop, 100ms);
    const std::set<std::string> now{g.address(0).to_string(), g.address(1).to_string()};
    EXPECT_EQ(now, (std::set<std::string>{fast.endpoint().to_string(), spare.endpoint().to_string()}));
    EXPECT_GT(delivered, 20);
}

TEST(TestWsGroup, SendBestUsesTheLowestRttSession) {
    std::atomic<int> got_fast{0}, got_slow{0};
    auto pong_after = [](std::chrono::milliseconds delay, std::atomic<int>& texts) {
        return [delay, &texts](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            while (!s.stopping()) {
                auto f = c.read_frame();
                if (!f) return;
                if (f->opcode == WsOpcode::Ping) {
                    std::this_thread::sleep_for(delay);
                    c.send_frame(WsOpcode::Pong, f->payload);
                } else if (f->opcode == WsOpcode::Text) {
                    ++texts;
                }
            }
        };
    };
    TestServer fast(pong_after(0ms, got_fast)), slow(pong_after(40ms, got_slow));
    EventLoop loop;
    Pools p;
    WsGroupOptions o{.sessions = 2, .dedup = false};
    o.session.ping_interval_ns = 50'000'000;
    WsGroup<TcpSocket> g(loop, p.recv, p.send, o);
    g.set_host("localhost");
    EXPECT_EQ(g.send_best("x"), WsSendStatus::NotOpen);
    g.connect([&] { return std::vector{fast.endpoint(), slow.endpoint()}; });
    ASSERT_TRUE(run_until(loop, [&] { return g.session(0).last_rtt_ns() > 0 && g.session(1).last_rtt_ns() > 0; }, 3000ms));
    for (int i = 0; i < 5; ++i) EXPECT_EQ(g.send_best("order"), WsSendStatus::Ok);
    run_until(loop, [&] { return got_fast + got_slow >= 5; }, 2000ms);
    EXPECT_EQ(got_fast.load(), 5);
    EXPECT_EQ(got_slow.load(), 0);
}
