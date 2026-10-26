#pragma once

#include "quark/Attributes.h"
#include "quark/net/Endpoint.h"
#include "quark/net/SysApi.h"

#include <concepts>
#include <span>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Connection-setup progress reported by Stream::poll_open().
enum class StreamPhase : uint8_t { Connecting, Open, Failed, Closed };

/// The transport the protocol layers (WsSession, HttpClient) are written against, so the same
/// state machine runs over plain TCP (TcpSocket) or TLS (TlsStream) at zero runtime cost.
/// All calls are nonblocking:
///   - connect():   starts connecting to a resolved endpoint; false on an immediate local error.
///   - poll_open(): advances setup (TCP connect, then e.g. the TLS handshake).
///   - read():      bytes now, 0 on EOF, -EAGAIN when nothing is buffered *anywhere* (callers on
///                  an edge-triggered poller read until -EAGAIN), otherwise -errno on error.
///   - write():     bytes accepted now (may be partial), -EAGAIN when full, other -errno on error.
///   - wants_write(): output the transport buffered itself (e.g. TLS records the socket didn't
///                  take) is waiting: keep write interest and call flush() when writable.
///   - flush():     sends that output: 0 once it is all out, -EAGAIN while some is left, other
///                  -errno on error.
template <typename T>
concept Stream = requires(T s, const T& cs, const Endpoint& ep, std::span<uint8_t> rb, std::span<const uint8_t> wb,
                          bool more) {
    { s.connect(ep) } -> std::convertible_to<bool>;
    { s.poll_open() } -> std::convertible_to<StreamPhase>;
    { s.fd() } -> std::convertible_to<int>;
    { s.read(rb) } -> std::convertible_to<sys::io_result>;
    { s.write(wb, more) } -> std::convertible_to<sys::io_result>;
    { cs.wants_write() } -> std::convertible_to<bool>;
    { s.flush() } -> std::convertible_to<sys::io_result>;
    { s.closed() } -> std::convertible_to<bool>;
    { s.close() };
};

} // namespace net

QUARK_END_NAMESPACE
