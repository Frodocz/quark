#include "quark/net/Url.h"
#include "gtest/gtest.h"

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
}
