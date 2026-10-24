#include "quark/net/WsHandshake.h"
#include "gtest/gtest.h"

#include <string>

using namespace quark::net;

namespace {
std::span<const uint8_t> bytes(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }
const std::string kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";  // RFC 6455 section 1.3 example
}  // namespace

class TestWsHandshake : public ::testing::Test {};

TEST_F(TestWsHandshake, AcceptOfTheRfcExampleKey) {
    char out[28];
    compute_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", out);
    EXPECT_EQ(std::string_view(out, 28), kAccept);
}

TEST_F(TestWsHandshake, FreshKeysAreRandomAndConsistent) {
    const WsHandshakeKey a = make_ws_handshake_key();
    const WsHandshakeKey b = make_ws_handshake_key();
    EXPECT_NE(a.key_view(), b.key_view());
    char expected[28];
    compute_ws_accept(a.key_view(), expected);
    EXPECT_EQ(a.accept_view(), std::string_view(expected, 28));
    EXPECT_EQ(a.key_view().back(), '=');  // base64 of 16 bytes
}

TEST_F(TestWsHandshake, UpgradeRequest) {
    std::string req;
    build_ws_upgrade_request(req, "stream.binance.com:9443", "/stream?streams=btcusdt@trade", "KEY",
                             "X-MBX-APIKEY: abc\r\n");
    EXPECT_TRUE(req.starts_with("GET /stream?streams=btcusdt@trade HTTP/1.1\r\nHost: stream.binance.com:9443\r\n"));
    EXPECT_NE(req.find("\r\nSec-WebSocket-Key: KEY\r\n"), std::string::npos);
    EXPECT_NE(req.find("\r\nSec-WebSocket-Version: 13\r\n"), std::string::npos);
    EXPECT_NE(req.find("\r\nX-MBX-APIKEY: abc\r\n"), std::string::npos);
    EXPECT_TRUE(req.ends_with("\r\n\r\n"));
}

TEST_F(TestWsHandshake, AcceptsOnlyAMatching101) {
    const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + kAccept + "\r\n\r\n";
    auto r = parse_ws_upgrade_response(bytes(ok + "\x81\x02hi"), kAccept);  // a frame may follow
    EXPECT_TRUE(r.complete && r.accepted);
    EXPECT_EQ(r.status, 101);
    EXPECT_EQ(r.header_len, ok.size());

    // header names and the Upgrade value are case-insensitive
    const std::string lower = "HTTP/1.1 101 OK\r\nupgrade: WebSocket\r\nsec-websocket-accept:  " + kAccept + " \r\n\r\n";
    EXPECT_TRUE(parse_ws_upgrade_response(bytes(lower), kAccept).accepted);

    EXPECT_FALSE(parse_ws_upgrade_response(bytes(ok.substr(0, ok.size() - 2)), kAccept).complete);
    EXPECT_FALSE(parse_ws_upgrade_response(bytes(ok), "AAAAAAAAAAAAAAAAAAAAAAAAAAA=").accepted);  // wrong accept

    const std::string no_upgrade = "HTTP/1.1 101 OK\r\nSec-WebSocket-Accept: " + kAccept + "\r\n\r\n";
    EXPECT_FALSE(parse_ws_upgrade_response(bytes(no_upgrade), kAccept).accepted);

    // rate limited: a complete, rejected response that reports the status
    const auto limited = parse_ws_upgrade_response(bytes("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 5\r\n\r\n"), kAccept);
    EXPECT_TRUE(limited.complete);
    EXPECT_FALSE(limited.accepted);
    EXPECT_EQ(limited.status, 429);

    EXPECT_FALSE(parse_ws_upgrade_response(bytes("garbage\r\n\r\n"), kAccept).accepted);
}
