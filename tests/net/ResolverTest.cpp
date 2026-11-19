#include "quark/net/Resolver.h"
#include "gtest/gtest.h"

using namespace quark::net;

TEST(TestResolver, WatchResolvesAndRotates) {
    Resolver dns;
    const auto id = dns.watch("localhost", 443);
    const auto addrs = dns.endpoints(id);
    ASSERT_FALSE(addrs.empty());
    EXPECT_EQ(addrs.front().to_string(), "127.0.0.1:443");
    for (std::size_t i = 0; i < 2 * addrs.size(); ++i) EXPECT_EQ(dns.next(id), addrs[i % addrs.size()]);
    EXPECT_EQ(dns.rotating(id)(), addrs[0]);
}

TEST(TestResolver, UnknownHostHasNoAddress) {
    Resolver dns;
    const auto id = dns.watch("no-such-host.invalid", 443);
    EXPECT_TRUE(dns.endpoints(id).empty());
    EXPECT_FALSE(dns.next(id).valid());
}

TEST(TestResolver, RefreshKeepsRunningAndStopsCleanly) {
    Resolver dns(1'000'000);  // 1 ms refresh
    const auto id = dns.watch("localhost", 80);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(dns.endpoints(id).empty());
}
