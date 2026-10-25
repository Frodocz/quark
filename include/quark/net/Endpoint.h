#pragma once

#include "quark/Attributes.h"

#include <netinet/in.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

QUARK_BEGIN_NAMESPACE

namespace net {

/// A resolved IPv4 address and port, in network byte order. Trivially copyable: resolve once
/// (cold path) and reuse it across reconnects.
struct QUARK_API Endpoint {
    uint32_t ip_be{0};
    uint16_t port_be{0};

    [[nodiscard]] bool valid() const noexcept { return ip_be != 0; }

    [[nodiscard]] sockaddr_in to_sockaddr() const noexcept {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = port_be;
        sa.sin_addr.s_addr = ip_be;
        return sa;
    }

    /// "a.b.c.d:port"
    [[nodiscard]] std::string to_string() const;

    /// A dotted-quad IPv4 literal and a host-order port, std::nullopt if `ip` isn't one.
    static std::optional<Endpoint> from_ip(std::string_view ip, uint16_t port);

    /// Every IPv4 address `host` (name or literal) resolves to, in resolver order; empty on
    /// failure. BLOCKING (getaddrinfo): startup and reconnect paths only.
    static std::vector<Endpoint> resolve_all(std::string_view host, uint16_t port);

    /// The first address of resolve_all(), std::nullopt on failure. BLOCKING.
    static std::optional<Endpoint> resolve(std::string_view host, uint16_t port);

    /// The IPv4 address of the kernel network interface `name` (e.g. "ens5"), port 0;
    /// std::nullopt if it doesn't exist, is down or has no IPv4 address. Cold path.
    static std::optional<Endpoint> interface_ip(std::string_view name);

    /// The local address the kernel's routing table picks to reach `remote` (no packet is sent),
    /// port 0. Cold path. Both helpers describe kernel interfaces, not an F-Stack/DPDK port.
    static std::optional<Endpoint> source_ip_for(const Endpoint& remote);

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

} // namespace net

QUARK_END_NAMESPACE
