#pragma once

#include "quark/Attributes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

QUARK_BEGIN_NAMESPACE

namespace net {

/// A Sec-WebSocket-Key (base64 of 16 random bytes) and the Sec-WebSocket-Accept the server
/// must answer with (RFC 6455 section 4).
struct WsHandshakeKey {
    char key[24];
    char accept[28];

    [[nodiscard]] std::string_view key_view() const noexcept { return {key, sizeof(key)}; }
    [[nodiscard]] std::string_view accept_view() const noexcept { return {accept, sizeof(accept)}; }
};

/// A fresh key from the OS random source, with its expected accept value.
QUARK_API WsHandshakeKey make_ws_handshake_key();

/// base64(SHA-1(key + RFC 6455 GUID)) of a base64 `key`, into `out`.
QUARK_API void compute_ws_accept(std::string_view key, std::span<char, 28> out);

/// 64 random bits from the OS (getrandom), e.g. to seed per-session generators.
QUARK_API uint64_t os_random_u64();

/// Appends the HTTP/1.1 upgrade request to `out`. `host` is the Host header value (with
/// ":port" when not the scheme's default), `extra_headers` complete "Name: value\r\n" lines.
QUARK_API void build_ws_upgrade_request(std::string& out, std::string_view host, std::string_view target,
                              std::string_view key, std::string_view extra_headers);

struct WsUpgradeResponse {
    bool complete{false};        ///< the whole header block (up to \r\n\r\n) is there
    bool accepted{false};        ///< 101 with Upgrade: websocket and the expected accept value
    int status{0};               ///< HTTP status code, e.g. 101, 429 or 418 (rate limited / banned)
    std::size_t header_len{0};   ///< bytes of the header block; frames may follow in the buffer
};

/// Validates a possibly partial upgrade response against `expected_accept`.
[[nodiscard]] QUARK_API WsUpgradeResponse parse_ws_upgrade_response(std::span<const uint8_t> resp,
                                                          std::string_view expected_accept) noexcept;

} // namespace net

QUARK_END_NAMESPACE
