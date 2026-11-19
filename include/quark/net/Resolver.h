#pragma once

#include "quark/Attributes.h"
#include "quark/net/Endpoint.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Keeps the IPv4 addresses of a few hosts fresh on its own thread, so getaddrinfo (blocking)
/// never runs on an event loop thread. A failed lookup keeps the last good addresses.
///
///   Resolver dns;
///   const auto id = dns.watch("fstream.binance.com", 443);   // startup: resolves once, blocking
///   ws.connect(dns.next(id));
///   ws.set_endpoint_provider(dns.rotating(id));              // reconnects follow DNS changes
///
/// next() takes a mutex: meant for (re)connect paths, not per message.
class QUARK_API Resolver {
public:
    explicit Resolver(int64_t refresh_ns = 60'000'000'000);
    ~Resolver();

    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;

    /// Resolves `host` now (blocking) and keeps refreshing it. Returns its id.
    std::size_t watch(std::string_view host, uint16_t port);

    /// The latest addresses of a watched host; empty if it never resolved.
    [[nodiscard]] std::vector<Endpoint> endpoints(std::size_t id) const;

    /// The next of the latest addresses, round robin; invalid if there are none.
    [[nodiscard]] Endpoint next(std::size_t id);

    /// next(id) as an EndpointProvider. The Resolver must outlive its users.
    [[nodiscard]] EndpointProvider rotating(std::size_t id) {
        return [this, id] { return next(id); };
    }

private:
    struct Host {
        std::string name;
        uint16_t port;
        std::vector<Endpoint> addrs;
        std::size_t cursor{0};
    };

    const int64_t refresh_ns_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Host> hosts_;
    bool stop_{false};
    std::thread thread_;

    void run();
};

} // namespace net

QUARK_END_NAMESPACE
