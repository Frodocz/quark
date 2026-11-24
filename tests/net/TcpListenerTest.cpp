#include "TestServers.h"
#include "quark/net/TcpListener.h"
#include "gtest/gtest.h"

#include <string>
#include <vector>

using namespace quark;
using namespace quark::net;
using namespace quark::net::test;
using namespace std::chrono_literals;

TEST(TestTcpListener, AcceptsConnectionsAsTcpSockets) {
    EventLoop loop;
    TcpListener listener(loop);
    std::vector<TcpSocket> accepted;
    std::vector<Endpoint> peers;
    listener.on_accept([&](TcpSocket&& s, const Endpoint& peer) {
        accepted.push_back(std::move(s));
        peers.push_back(peer);
    });
    ASSERT_TRUE(listener.listen(*Endpoint::from_ip("127.0.0.1", 0)));
    ASSERT_NE(listener.local().port_be, 0);

    TcpSocket a, b;
    ASSERT_TRUE(a.connect(listener.local()));
    ASSERT_TRUE(b.connect(listener.local()));
    ASSERT_TRUE(run_until(loop, [&] { return accepted.size() == 2; }));
    EXPECT_EQ(peers[0].ip_be, listener.local().ip_be);
    EXPECT_EQ(accepted[0].poll_open(), StreamPhase::Open);

    ASSERT_TRUE(run_until(loop, [&] { return a.poll_open() == StreamPhase::Open; }));
    const std::string hello = "hello";
    ASSERT_EQ(a.write({reinterpret_cast<const uint8_t*>(hello.data()), hello.size()}, false), 5);
    uint8_t buf[16];
    sys::io_result n = -EAGAIN;
    ASSERT_TRUE(run_until(loop, [&] { return (n = accepted[0].read(buf)) != -EAGAIN; }));
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), static_cast<std::size_t>(n)), "hello");

    listener.close();
    EXPECT_FALSE(listener.listening());
    EXPECT_EQ(loop.size(), 0u);
}

TEST(TestTcpListener, ListenFailsOnAPortInUse) {
    EventLoop loop;
    TcpListener first(loop), second(loop);
    ASSERT_TRUE(first.listen(*Endpoint::from_ip("127.0.0.1", 0)));
    EXPECT_FALSE(second.listen(first.local()));
    EXPECT_EQ(loop.size(), 1u);
}
