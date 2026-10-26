#include "TestServers.h"
#include "quark/net/TcpSocket.h"
#include "gtest/gtest.h"

#include <chrono>
#include <string>

using namespace quark::net;
using namespace quark::net::test;
using namespace std::chrono_literals;

namespace {
StreamPhase open_within(TcpSocket& s, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    StreamPhase ph = s.poll_open();
    while (ph == StreamPhase::Connecting && std::chrono::steady_clock::now() < deadline) ph = s.poll_open();
    return ph;
}
}  // namespace

class TestTcpSocket : public ::testing::Test {};

TEST_F(TestTcpSocket, ConnectWriteRead) {
    TestServer server([](ServerConn& c, TestServer&) {
        char buf[64];
        const int n = c.read_some(buf, sizeof(buf));
        if (n > 0) c.write_all(buf, static_cast<std::size_t>(n));
    });
    TcpSocket s;
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    const std::string msg = "ping";
    ASSERT_EQ(s.write({reinterpret_cast<const uint8_t*>(msg.data()), msg.size()}, false), 4);
    uint8_t buf[16];
    sys::io_result n = -EAGAIN;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (n == -EAGAIN && std::chrono::steady_clock::now() < deadline) n = s.read(buf);
    ASSERT_EQ(n, 4);
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(buf), 4), "ping");
    s.close();
    EXPECT_TRUE(s.closed());
}

TEST_F(TestTcpSocket, BindsTheChosenLocalIp) {
    TestServer server([](ServerConn&, TestServer&) {});
    TcpSocket s;
    s.set_local_ip(Endpoint::from_ip("127.0.0.2", 0)->ip_be);  // any 127/8 address is local
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (server.accepted() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(server.last_peer_ip(), "127.0.0.2");
}

TEST_F(TestTcpSocket, NeverConnectedIsNotConnecting) {
    TcpSocket s;
    EXPECT_EQ(s.poll_open(), StreamPhase::Closed);
    EXPECT_EQ(s.fd(), -1);
}

TEST_F(TestTcpSocket, UnsetEndpointIsRefused) {
    // 0.0.0.0 would reach a local service: an unresolved endpoint must not connect
    TestServer server([](ServerConn&, TestServer& s) { s.hold(); });
    TcpSocket s;
    EXPECT_FALSE(s.connect(Endpoint{0, server.endpoint().port_be}));
    EXPECT_EQ(s.poll_open(), StreamPhase::Failed);
    EXPECT_EQ(s.fd(), -1);
}

TEST_F(TestTcpSocket, PeerCloseReadsEofAndWriteAfterCloseFails) {
    TestServer server([](ServerConn&, TestServer&) {});  // accepts, then closes
    TcpSocket s;
    ASSERT_TRUE(s.connect(server.endpoint()));
    ASSERT_EQ(open_within(s), StreamPhase::Open);
    uint8_t buf[16];
    sys::io_result n = -EAGAIN;
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (n == -EAGAIN && std::chrono::steady_clock::now() < deadline) n = s.read(buf);
    EXPECT_EQ(n, 0);
    // writing into the closed connection: an error, never SIGPIPE
    const uint8_t data[64]{};
    sys::io_result w = 0;
    for (int i = 0; i < 100 && w >= 0; ++i) w = s.write(data, false);
    EXPECT_LT(w, 0);
    EXPECT_NE(w, -EAGAIN);
    s.close();
    EXPECT_LT(s.read(buf), 0);  // -EBADF on a closed socket
}

TEST_F(TestTcpSocket, ConnectionRefusedFails) {
    uint16_t closed_port;
    {
        TestServer probe([](ServerConn&, TestServer&) {});
        closed_port = probe.port();
    }  // listener gone: nothing on that port
    TcpSocket s;
    if (!s.connect(*Endpoint::from_ip("127.0.0.1", closed_port))) SUCCEED();  // may fail immediately
    else EXPECT_EQ(open_within(s), StreamPhase::Failed);
}
