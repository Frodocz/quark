#include "quark/net/Endpoint.h"

#include "quark/QuarkLog.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

QUARK_BEGIN_NAMESPACE

namespace net {

std::string Endpoint::to_string() const {
    char ip[INET_ADDRSTRLEN] = "?";
    in_addr addr{};
    addr.s_addr = ip_be;
    ::inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(port_be));
}

std::optional<Endpoint> Endpoint::from_ip(std::string_view ip, uint16_t port) {
    const std::string s(ip);  // inet_pton needs a NUL-terminated string
    in_addr addr{};
    if (::inet_pton(AF_INET, s.c_str(), &addr) != 1) return std::nullopt;
    return Endpoint{addr.s_addr, htons(port)};
}

std::vector<Endpoint> Endpoint::resolve_all(std::string_view host, uint16_t port) {
    std::vector<Endpoint> out;
    const std::string name(host);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (const int rc = ::getaddrinfo(name.c_str(), nullptr, &hints, &result); rc != 0) {
        QUARK_LOG_WARN("resolve {} failed: {}", name, ::gai_strerror(rc));
        return out;
    }
    for (const addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const auto* sa = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        const Endpoint ep{sa->sin_addr.s_addr, htons(port)};
        bool seen = false;
        for (const Endpoint& e : out) seen |= e == ep;
        if (!seen) out.push_back(ep);
    }
    ::freeaddrinfo(result);
    return out;
}

std::optional<Endpoint> Endpoint::resolve(std::string_view host, uint16_t port) {
    auto all = resolve_all(host, port);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::optional<Endpoint> Endpoint::interface_ip(std::string_view name) {
    ifaddrs* addrs = nullptr;
    if (::getifaddrs(&addrs) != 0) return std::nullopt;
    std::optional<Endpoint> found;
    for (const ifaddrs* ifa = addrs; ifa != nullptr && !found; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET || !(ifa->ifa_flags & IFF_UP)) continue;
        if (name == ifa->ifa_name) found = Endpoint{reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr.s_addr, 0};
    }
    ::freeifaddrs(addrs);
    return found;
}

std::optional<Endpoint> Endpoint::source_ip_for(const Endpoint& remote) {
    // connect() on a UDP socket only selects a route and a source address
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::nullopt;
    Endpoint target = remote;
    if (target.port_be == 0) target.port_be = htons(9);  // any port: nothing is sent
    sockaddr_in sa = target.to_sockaddr();
    std::optional<Endpoint> local;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0) {
        sockaddr_in out{};
        socklen_t len = sizeof(out);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&out), &len) == 0) local = Endpoint{out.sin_addr.s_addr, 0};
    }
    ::close(fd);
    return local;
}

} // namespace net

QUARK_END_NAMESPACE
