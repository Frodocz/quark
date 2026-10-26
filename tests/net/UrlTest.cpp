#include "quark/net/Url.h"
#include "gtest/gtest.h"

#include <string_view>

using namespace quark::net;

class TestUrl : public ::testing::Test {};

TEST_F(TestUrl, ExchangeEndpoints) {
    auto u = parse_url("wss://stream.binance.com:9443/stream?streams=btcusdt@trade/ethusdt@trade");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->scheme, "wss");
    EXPECT_EQ(u->host, "stream.binance.com");
    EXPECT_EQ(u->port, 9443);
    EXPECT_EQ(u->target, "/stream?streams=btcusdt@trade/ethusdt@trade");
    EXPECT_TRUE(u->secure());

    auto r = parse_url("https://fapi.binance.com/fapi/v1/order");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->port, 443);
    EXPECT_EQ(r->target, "/fapi/v1/order");
    EXPECT_TRUE(r->secure());
}

TEST_F(TestUrl, DefaultsAndNormalisation) {
    auto a = parse_url("WS://localhost");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->scheme, "ws");
    EXPECT_EQ(a->port, 80);
    EXPECT_EQ(a->target, "/");
    EXPECT_FALSE(a->secure());

    auto b = parse_url("http://10.0.0.1:8080?x=1#frag");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->host, "10.0.0.1");
    EXPECT_EQ(b->port, 8080);
    EXPECT_EQ(b->target, "/?x=1");

    auto c = parse_url("wss://[::1]:7000/ws");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->host, "::1");
    EXPECT_EQ(c->port, 7000);
    EXPECT_EQ(c->target, "/ws");
}

TEST_F(TestUrl, Rejects) {
    EXPECT_FALSE(parse_url("stream.binance.com:9443"));  // no scheme
    EXPECT_FALSE(parse_url("ftp://host/"));
    EXPECT_FALSE(parse_url("wss:///path"));               // no host
    EXPECT_FALSE(parse_url("wss://host:/x"));             // empty port
    EXPECT_FALSE(parse_url("wss://host:0"));
    EXPECT_FALSE(parse_url("wss://host:65536"));
    EXPECT_FALSE(parse_url("wss://host:12ab"));
    EXPECT_FALSE(parse_url("wss://user:pw@host/"));
    EXPECT_FALSE(parse_url("wss://[::1/"));
    EXPECT_FALSE(parse_url("wss://[::1]x/"));
    EXPECT_FALSE(parse_url("wss://[]/"));
    EXPECT_FALSE(parse_url("wss://host:-1/"));
    EXPECT_FALSE(parse_url("wss://host:00000/"));
    EXPECT_FALSE(parse_url("wss://h:80@evil/"));
    EXPECT_FALSE(parse_url(""));
    EXPECT_FALSE(parse_url("ws:/host"));
    EXPECT_FALSE(parse_url("ws://"));
}

// host and target go verbatim into the request line and Host header
TEST_F(TestUrl, RejectsSpacesAndControlCharacters) {
    EXPECT_FALSE(parse_url("ws://host/a\r\nX-Injected: 1"));
    EXPECT_FALSE(parse_url("ws://ho st/"));
    EXPECT_FALSE(parse_url("ws://host/a b"));
    EXPECT_FALSE(parse_url("ws://host/\x7f"));
    EXPECT_FALSE(parse_url(std::string_view("ws://host/\0x", 13)));
    EXPECT_TRUE(parse_url("ws://host/a%20b?x=1"));  // percent-encoded is fine
}

TEST_F(TestUrl, PortAndFragmentEdges) {
    EXPECT_EQ(parse_url("ws://host:080")->port, 80);
    EXPECT_EQ(parse_url("ws://host:65535")->port, 65535);
    EXPECT_EQ(parse_url("ws://[::1]")->port, 80);
    EXPECT_EQ(parse_url("ws://host#frag")->target, "/");
    EXPECT_EQ(parse_url("ws://host?x#frag")->target, "/?x");
    EXPECT_EQ(parse_url("wss://host/p?email=a@b")->host, "host");  // '@' in the query is not user info
}
