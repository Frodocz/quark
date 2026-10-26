#pragma once

#include "quark/Attributes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

QUARK_BEGIN_NAMESPACE

namespace net {

/// A parsed ws://, wss://, http:// or https:// URL. Cold path (configuration, reconnects).
struct Url {
    std::string scheme;  ///< lowercase: "ws", "wss", "http" or "https"
    std::string host;    ///< without brackets for IPv6 literals
    uint16_t port{0};    ///< explicit port, or the scheme's default
    std::string target;  ///< path and query, "/" when absent; the fragment is dropped

    [[nodiscard]] bool secure() const noexcept { return scheme == "wss" || scheme == "https"; }
};

namespace detail {

inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != b[i]) return false;
    }
    return true;
}

} // namespace detail

/// Parses `text`, returning std::nullopt for an unsupported scheme, a missing host, a bad port,
/// user info ("user@host", never needed for exchange endpoints), or a space or control character
/// after the scheme (percent-encode them).
inline std::optional<Url> parse_url(std::string_view text) {
    Url url;
    const auto sep = text.find("://");
    if (sep == std::string_view::npos) return std::nullopt;
    const std::string_view scheme = text.substr(0, sep);
    constexpr std::pair<std::string_view, uint16_t> kSchemes[] = {
        {"ws", 80}, {"wss", 443}, {"http", 80}, {"https", 443}};
    for (const auto& [name, default_port] : kSchemes) {
        if (detail::iequals(scheme, name)) {
            url.scheme = name;
            url.port = default_port;
        }
    }
    if (url.scheme.empty()) return std::nullopt;

    std::string_view rest = text.substr(sep + 3);
    // the host and target go verbatim into the request line and Host header: no spaces or
    // control characters (CRLF would inject headers)
    for (const char c : rest) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return std::nullopt;
    }
    if (const auto frag = rest.find('#'); frag != std::string_view::npos) rest = rest.substr(0, frag);
    const auto target_pos = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, target_pos);
    if (target_pos != std::string_view::npos) {
        const std::string_view target = rest.substr(target_pos);
        url.target = target.front() == '?' ? "/" + std::string(target) : std::string(target);
    } else {
        url.target = "/";
    }
    if (authority.find('@') != std::string_view::npos) return std::nullopt;

    std::string_view port_str;
    if (!authority.empty() && authority.front() == '[') {  // [IPv6]:port
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        url.host = authority.substr(1, close - 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return std::nullopt;
            port_str = after.substr(1);
            if (port_str.empty()) return std::nullopt;
        }
    } else {
        const auto colon = authority.find(':');
        url.host = authority.substr(0, colon);
        if (colon != std::string_view::npos) {
            port_str = authority.substr(colon + 1);
            if (port_str.empty()) return std::nullopt;
        }
    }
    if (url.host.empty()) return std::nullopt;

    if (!port_str.empty()) {
        if (port_str.size() > 5) return std::nullopt;
        uint32_t port = 0;
        for (const char c : port_str) {
            if (c < '0' || c > '9') return std::nullopt;
            port = port * 10 + static_cast<uint32_t>(c - '0');
        }
        if (port == 0 || port > 65535) return std::nullopt;
        url.port = static_cast<uint16_t>(port);
    }
    return url;
}

} // namespace net

QUARK_END_NAMESPACE
