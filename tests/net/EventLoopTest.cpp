#include "quark/net/Endpoint.h"
#include "quark/net/EventLoop.h"
#include "gtest/gtest.h"

#include <unistd.h>

#include <algorithm>
#include <functional>

using namespace quark;
using namespace quark::net;

namespace {

// readable end of a pipe with a byte waiting
struct PipePollable : Pollable {
    int fds[2]{-1, -1};
    int calls{0};
    std::function<void()> on_call;

    PipePollable() {
        EXPECT_EQ(::pipe(fds), 0);
        EXPECT_EQ(::write(fds[1], "x", 1), 1);
    }
    ~PipePollable() override {
        ::close(fds[0]);
        ::close(fds[1]);
    }
    [[nodiscard]] int fd() const noexcept override { return fds[0]; }
    void on_ready(uint32_t) override {
        ++calls;
        if (on_call) on_call();
    }
};

}  // namespace

class TestEventLoop : public ::testing::TestWithParam<bool> {};

TEST_P(TestEventLoop, StepHookAndTimers) {
    EventLoop loop({.busy_poll = GetParam()});
    int hooks = 0;
    loop.set_step_hook([&hooks] { ++hooks; });
    int fired = 0;
    loop.timers().add_task("t", [&fired] { ++fired; return false; }, loop.now_ns() + 1'000'000);
    const int64_t start = loop.now_ns();
    while (fired == 0 && loop.now_ns() - start < 1'000'000'000) loop.step();
    EXPECT_EQ(fired, 1);
    EXPECT_GT(hooks, 0);
    EXPECT_GE(loop.now_ns() - start, 1'000'000);
}

TEST_P(TestEventLoop, RemoveFromInsideADispatchedCallback) {
    EventLoop loop({.busy_poll = GetParam()});
    PipePollable a, b;
    ASSERT_TRUE(loop.add(a, false));
    ASSERT_TRUE(loop.add(b, false));
    // whichever runs first removes the other: the other must not be called in the same step
    a.on_call = [&] { loop.remove(b); };
    b.on_call = [&] { loop.remove(a); };
    loop.step();
    EXPECT_EQ(a.calls + b.calls, 1);
    EXPECT_EQ(loop.size(), 1u);
    loop.step();
    EXPECT_EQ(loop.size(), 1u);
}

TEST_P(TestEventLoop, AddWhileStepping) {
    EventLoop loop({.busy_poll = GetParam()});
    PipePollable a, b;
    a.on_call = [&] {
        if (b.calls == 0 && loop.size() == 1) loop.add(b, false);
    };
    ASSERT_TRUE(loop.add(a, false));
    for (int i = 0; i < 3; ++i) loop.step();
    EXPECT_EQ(loop.size(), 2u);
    EXPECT_GE(b.calls, 1);
}

INSTANTIATE_TEST_SUITE_P(Modes, TestEventLoop, ::testing::Values(true, false),
                         [](const auto& info) { return info.param ? "BusyPoll" : "Epoll"; });

class TestEndpoint : public ::testing::Test {};

TEST_F(TestEndpoint, FromIpAndToString) {
    auto ep = Endpoint::from_ip("10.1.2.3", 9443);
    ASSERT_TRUE(ep);
    EXPECT_EQ(ep->to_string(), "10.1.2.3:9443");
    EXPECT_FALSE(Endpoint::from_ip("10.1.2", 1));
    EXPECT_FALSE(Endpoint::from_ip("localhost", 1));
}

TEST_F(TestEndpoint, Resolve) {
    const auto all = Endpoint::resolve_all("localhost", 80);
    ASSERT_FALSE(all.empty());
    EXPECT_TRUE(std::any_of(all.begin(), all.end(), [](const Endpoint& e) { return e.to_string() == "127.0.0.1:80"; }));
    EXPECT_EQ(Endpoint::resolve("127.0.0.1", 443)->to_string(), "127.0.0.1:443");
    EXPECT_FALSE(Endpoint::resolve("no-such-host.invalid", 443));
}
