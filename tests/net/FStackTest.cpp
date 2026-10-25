// quark::net over F-Stack (built with QUARK_USE_FSTACK). Needs root and an F-Stack config:
//
//     sudo ./quark_test_fstack --conf /path/to/f-stack.conf
//
// F-Stack only processes packets inside its own loop (ff_run), on the thread that initialized it,
// and can run that loop once per process. So the main thread runs it (EventLoop::run) and steps
// the event loops the tests register, while the tests run on a second thread and post their
// socket work to the loop thread (on_loop / wait_until below).
//
// - Public smoke tests reach the internet through the DPDK port (HTTP, HTTPS with certificate
//   verification, a public WebSocket echo service); they skip when DNS or the peer is unavailable.
// - Local tests run when QUARK_FSTACK_PEER_IP names an address of this host that the DPDK port can
//   reach (e.g. the kernel interface, if the security group admits VPC-internal TCP): test
//   servers listen there on kernel sockets.

#include "TestServers.h"
#include "quark/net/BufferPool.h"
#include "quark/net/EventLoop.h"
#include "quark/net/FStack.h"
#include "quark/net/HttpClient.h"
#include "quark/net/TcpSocket.h"
#include "quark/net/TlsStream.h"
#include "quark/net/WsSession.h"
#include "gtest/gtest.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace quark;
using namespace quark::net;
using namespace quark::net::test;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------- loop thread plumbing

/// Owns the F-Stack loop (main thread) and the work the test thread posts to it.
class LoopThread {
public:
    LoopThread() {
        driver_.set_step_hook([this] {
            run_posted();
            for (EventLoop* l : loops_) l->step();
        });
    }

    /// Runs on the main thread until stop(): F-Stack's loop.
    void run() { driver_.run(); }
    void stop() { post([this] { driver_.stop(); }); }

    /// Runs `fn` on the loop thread and waits for it.
    template <class Fn>
    auto call(Fn fn) -> decltype(fn()) {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(fn));
        std::future<R> result = task->get_future();
        post([task] { (*task)(); });
        return result.get();
    }

    /// Steps `loop` on every F-Stack iteration (call on the loop thread).
    void attach(EventLoop& loop) { loops_.push_back(&loop); }
    void detach(EventLoop& loop) { std::erase(loops_, &loop); }

private:
    EventLoop driver_{{.busy_poll = true}};
    std::vector<EventLoop*> loops_;
    std::mutex mu_;
    std::deque<std::function<void()>> posted_;

    void post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mu_);
        posted_.push_back(std::move(fn));
    }

    void run_posted() {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lock(mu_);
            work.swap(posted_);
        }
        for (auto& fn : work) fn();
    }
};

LoopThread* g_loop_thread = nullptr;

template <class Fn>
auto on_loop(Fn fn) {
    return g_loop_thread->call(std::move(fn));
}

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!on_loop(pred)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// Owns an object that must be created and destroyed on the loop thread, even when an ASSERT
/// leaves the test early.
template <class T>
struct OnLoop {
    std::unique_ptr<T> p;
    ~OnLoop() { reset(); }
    void reset() {
        if (p) on_loop([this] { p.reset(); });
    }
    T* operator->() const { return p.get(); }
    T& operator*() const { return *p; }
};

/// An event loop, pools and TLS context living on the loop thread for one test.
struct Env {
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<BufferPool> recv_pool;
    std::unique_ptr<BufferPool> send_pool;
    std::unique_ptr<TlsContext> tls;

    explicit Env(bool busy_poll, TlsContext::Options tls_options = {}) {
        on_loop([&] {
            loop = std::make_unique<EventLoop>(EventLoopConfig{.busy_poll = busy_poll});
            recv_pool = std::make_unique<BufferPool>(8, 64 * 1024, MemoryArena::Options{.huge_pages = MemoryArena::HugePages::Never});
            send_pool = std::make_unique<BufferPool>(64, 4096, MemoryArena::Options{.huge_pages = MemoryArena::HugePages::Never});
            tls = std::make_unique<TlsContext>(tls_options);
            g_loop_thread->attach(*loop);
        });
    }
    ~Env() {
        on_loop([&] {
            g_loop_thread->detach(*loop);
            tls.reset();
            send_pool.reset();
            recv_pool.reset();
            loop.reset();
        });
    }
};

const char* peer_ip() { return std::getenv("QUARK_FSTACK_PEER_IP"); }

// ---------------------------------------------------------------- HTTP / HTTPS over the internet

template <class Transport>
void http_get(Env& env, std::string_view host, const Endpoint& ep, int& status) {
    OnLoop<HttpClient<Transport>> http;
    on_loop([&] {
        http.p = std::make_unique<HttpClient<Transport>>(*env.loop, *env.recv_pool);
        if constexpr (std::is_same_v<Transport, TlsStream>) http->transport().set_context(*env.tls);
        http->set_host(host);
        http->on_response([&](const HttpResponseView& r) { status = r.status; });
        http->connect(ep);
    });
    ASSERT_TRUE(wait_until([&] { return http->ready(); })) << "connect to " << ep.to_string() << " failed";
    const std::string_view headers[] = {"User-Agent: quark-fstack-test", "Connection: keep-alive"};
    ASSERT_EQ(on_loop([&] { return http->request(HttpMethod::Get, "/", headers); }), HttpSubmit::Ok);
    EXPECT_TRUE(wait_until([&] { return status != 0; })) << "no response";
}

}  // namespace

class TestFStack : public ::testing::TestWithParam<bool> {};

TEST_P(TestFStack, PublicHttp) {
    Env env(GetParam());
    int status = 0;
    http_get<TcpSocket>(env, "1.1.1.1", *Endpoint::from_ip("1.1.1.1", 80), status);
    EXPECT_GE(status, 200);
}

TEST_P(TestFStack, PublicHttpsWithCertificateVerification) {
    Env env(GetParam());  // system CAs, peer verification on
    int status = 0;
    http_get<TlsStream>(env, "one.one.one.one", *Endpoint::from_ip("1.1.1.1", 443), status);
    EXPECT_GE(status, 200);
}

TEST_P(TestFStack, PublicWebSocketEcho) {
    const auto ep = Endpoint::resolve("ws.postman-echo.com", 443);  // DNS through the kernel
    if (!ep) GTEST_SKIP() << "cannot resolve ws.postman-echo.com";
    Env env(GetParam());
    std::vector<std::string> got;
    OnLoop<WsSession<TlsStream>> ws;
    on_loop([&] {
        ws.p = std::make_unique<WsSession<TlsStream>>(*env.loop, *env.recv_pool, *env.send_pool);
        ws->transport().set_context(*env.tls);
        ws->set_host("ws.postman-echo.com");
        ws->set_target("/raw");
        ws->on_message([&](const WsMessage& m) { got.emplace_back(m.text()); });
        ws->connect(*ep);
    });
    if (!wait_until([&] { return ws->is_open(); }, 10s)) GTEST_SKIP() << "echo service unreachable";
    EXPECT_EQ(on_loop([&] { return ws->send_text("hello from f-stack"); }), WsSendStatus::Ok);
    EXPECT_TRUE(wait_until([&] { return !got.empty(); }));
    if (!got.empty()) {
        EXPECT_EQ(got[0], "hello from f-stack");
    }
}

// ---------------------------------------------------------------- local peer (kernel test servers)

TEST_P(TestFStack, LocalWebSocketEchoOverTls) {
    if (peer_ip() == nullptr) GTEST_SKIP() << "set QUARK_FSTACK_PEER_IP to run against local test servers";
    TestServer server(
        [](ServerConn& c, TestServer& s) {
            c.accept_websocket();
            while (!s.stopping()) {
                auto f = c.read_frame();
                if (!f || f->opcode == WsOpcode::Close) return;
                c.send_frame(f->opcode, f->payload);
            }
        },
        {.tls = true, .bind_ip = peer_ip()});
    TlsContext::Options tls = trusting_test_ca();
    tls.verify_peer = false;  // the test certificate names localhost / 127.0.0.1, not the peer IP
    Env env(GetParam(), tls);
    std::vector<std::string> got;
    OnLoop<WsSession<TlsStream>> ws;
    on_loop([&] {
        ws.p = std::make_unique<WsSession<TlsStream>>(*env.loop, *env.recv_pool, *env.send_pool);
        ws->transport().set_context(*env.tls);
        ws->set_host("localhost");
        ws->on_message([&](const WsMessage& m) { got.emplace_back(m.text()); });
        ws->connect(server.endpoint());
    });
    ASSERT_TRUE(wait_until([&] { return ws->is_open(); })) << "cannot reach " << server.endpoint().to_string();
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(on_loop([&] { return ws->send_text("msg " + std::to_string(i)); }), WsSendStatus::Ok);
    }
    EXPECT_TRUE(wait_until([&] { return got.size() == 100; }));
    if (!got.empty()) {
        EXPECT_EQ(got.back(), "msg 99");
    }
}

TEST_P(TestFStack, LocalHttpKeepAlive) {
    if (peer_ip() == nullptr) GTEST_SKIP() << "set QUARK_FSTACK_PEER_IP to run against local test servers";
    TestServer server(
        [](ServerConn& c, TestServer& s) {
            for (int i = 0; i < 2; ++i) {
                if (c.read_until("\r\n\r\n").empty()) return;
                c.write_all("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
            }
            s.hold();
        },
        {.bind_ip = peer_ip()});
    Env env(GetParam());
    for (int round = 0; round < 2; ++round) {
        int status = 0;
        http_get<TcpSocket>(env, "localhost", server.endpoint(), status);
        EXPECT_EQ(status, 200);
    }
}

INSTANTIATE_TEST_SUITE_P(Modes, TestFStack, ::testing::Values(true, false),
                         [](const auto& info) { return info.param ? "BusyPoll" : "Epoll"; });

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);  // leaves only the F-Stack arguments
    if (!init_fstack(argc, argv)) {
        std::fprintf(stderr, "usage: sudo %s --conf <f-stack.conf> [gtest options]\n", argv[0]);
        return 2;
    }
    LoopThread loop_thread;
    g_loop_thread = &loop_thread;
    int rc = 0;
    std::thread tests([&] {
        rc = RUN_ALL_TESTS();
        loop_thread.stop();
    });
    loop_thread.run();
    tests.join();
    return rc;
}
