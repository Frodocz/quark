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

// NET-08: when the lowest-RTT session can't take a frame (its queue is full: the peer stopped reading),
// send_best() used to return WouldBlock although another session was open. A WouldBlock frame was
// never queued, so the next session in RTT order may send it safely.
TEST(TestWsGroup, SendBestFallsBackWhenTheBestSessionIsBackedUp) {
    std::atomic<int> got_slow{0};
    TestServer::Handler stalls_after_first_pong = [](ServerConn& c, TestServer& s) {
        c.accept_websocket();
        for (bool ponged = false; !s.stopping() && !ponged;) {
            auto f = c.read_frame();
            if (!f) return;
            if (f->opcode == WsOpcode::Ping) {
                c.send_frame(WsOpcode::Pong, f->payload);
                ponged = true;
            }
        }
        while (!s.stopping()) std::this_thread::sleep_for(10ms);  // stops reading: backpressure
    };
    TestServer::Handler pongs_late = [&](ServerConn& c, TestServer& s) {
        c.accept_websocket();
        while (!s.stopping()) {
            auto f = c.read_frame();
            if (!f) return;
            if (f->opcode == WsOpcode::Ping) {
                std::this_thread::sleep_for(30ms);
                c.send_frame(WsOpcode::Pong, f->payload);
            } else if (f->opcode == WsOpcode::Text) {
                ++got_slow;
            }
        }
    };
    TestServer fast(stalls_after_first_pong, {.rcvbuf = 4096}), slow(pongs_late);
    EventLoop loop;
    BufferPool recv{16, 64 * 1024, {.huge_pages = MemoryArena::HugePages::Never}};
    BufferPool send{512, 4096, {.huge_pages = MemoryArena::HugePages::Never}};
    WsGroupOptions opts{.sessions = 2, .dedup = false, .rotate_interval_ns = 0};
    opts.session.ping_interval_ns = 50'000'000;
    opts.session.pong_timeout_ns = 10'000'000'000;  // the stalled session stays open during the test
    WsGroup<TcpSocket> g(loop, recv, send, opts);
    g.set_host("localhost");
    g.connect([&] { return std::vector{fast.endpoint(), slow.endpoint()}; });
    ASSERT_TRUE(run_until(loop, [&] { return g.session(0).last_rtt_ns() > 0 && g.session(1).last_rtt_ns() > 0; }, 3000ms));
    const std::size_t fast_i = g.session(0).last_rtt_ns() < g.session(1).last_rtt_ns() ? 0 : 1;
    ASSERT_EQ(g.address(fast_i), fast.endpoint());
    g.session(fast_i).transport().set_send_buf(4096);

    const std::string order(3000, 'o');
    int would_block = 0;
    for (int i = 0; i < 3000 && got_slow.load() < 20; ++i) {
        would_block += g.send_best(order) == WsSendStatus::WouldBlock;
        loop.step();
    }
    EXPECT_EQ(g.session(fast_i).pending_frames(), WsSession<TcpSocket>::kMaxPendingFrames);  // it did back up
    EXPECT_EQ(would_block, 0);
    EXPECT_GE(got_slow.load(), 20);
}

// NET-05: hashing every copy of every message in full cost ~0.4 cycles/byte before the first copy was
// delivered; a feed can give a cheaper exchange-native key (e.g. stream + update id), 0 meaning "none"
TEST(TestWsGroup, DedupUsesTheGivenKeyAndFallsBackToTheTextWhenItIsZero) {
    const auto t0 = Clock::now() + 200ms;
    // both venues send "<n>:<k>": the same message k, with a per-IP prefix the key ignores
    auto venue = [t0](char prefix) {
        return [t0, prefix](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            for (int k = 0; !s.stopping() && k < 40; ++k) {
                std::this_thread::sleep_until(t0 + k * 5ms);
                const std::string text = k % 2 == 0 ? std::string(1, prefix) + ":" + std::to_string(k)  // keyed
                                                    : "plain" + std::to_string(k);                     // key 0: text
                if (!c.send_frame(WsOpcode::Text, text)) return;
            }
            while (!s.stopping()) std::this_thread::sleep_for(10ms);
        };
    };
    TestServer a(venue('a')), b(venue('b'));
    EventLoop loop;
    Pools p;
    WsGroupOptions opts{.sessions = 2, .rotate_interval_ns = 0};
    int keyed_calls = 0;
    opts.dedup_key = [&keyed_calls](const WsMessage& m) -> uint64_t {
        ++keyed_calls;
        const std::string_view t = m.text();
        const std::size_t colon = t.find(':');
        return colon == std::string_view::npos ? 0 : 1000 + std::stoul(std::string(t.substr(colon + 1)));
    };
    WsGroup<TcpSocket> g(loop, p.recv, p.send, opts);
    g.set_host("localhost");
    std::vector<std::string> got;
    g.on_message([&](const WsMessage& m, std::size_t) { got.emplace_back(m.text()); });
    g.connect([&] { return std::vector{a.endpoint(), b.endpoint()}; });
    run_until(loop, [&] { return got.size() >= 40 && keyed_calls >= 80; }, 3000ms);
    run_for(loop, 50ms);
    EXPECT_EQ(got.size(), 40u);  // each k once: keyed ones by key (texts differ), plain ones by text
    EXPECT_GE(keyed_calls, 80);
}
