#pragma once

#include "quark/Attributes.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(QUARK_USE_FSTACK)
    #include "quark/net/FStack.h"

    #include <sys/ioctl.h>

    #include <ff_api.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>

QUARK_BEGIN_NAMESPACE

/// The single seam between quark::net and the network stack: every socket call of the
/// transports goes through these thin wrappers. Kernel sockets by default; F-Stack (DPDK
/// userspace TCP/IP) when built with QUARK_USE_FSTACK, chosen at compile time.
namespace net::sys {

/// Result of a nonblocking I/O call: >= 0 bytes transferred (0 == EOF on recv),
/// < 0 negated errno (-EAGAIN: would block, retry on the next readiness).
using io_result = int64_t;

#if defined(QUARK_USE_FSTACK)

// F-Stack: the ff_* twins of the calls below. The FreeBSD stack has no MSG_NOSIGNAL/MSG_MORE
// (and raises no signals), no TCP_QUICKACK, and takes linux_sockaddr.

QUARK_ALWAYS_INLINE int set_nonblock(int fd) noexcept {
    int on = 1;
    return ::ff_ioctl(fd, FIONBIO, &on) == 0 ? 0 : -errno;
}

QUARK_ALWAYS_INLINE int tcp_socket() noexcept {
    const int fd = ::ff_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    if (const int rc = set_nonblock(fd); rc != 0) {
        ::ff_close(fd);
        return rc;
    }
    return fd;
}

/// After EventLoop::run() returns F-Stack is torn down: closing is then a no-op.
QUARK_ALWAYS_INLINE void close(int fd) noexcept {
    if (fd >= 0 && fstack_active()) ::ff_close(fd);
}

QUARK_ALWAYS_INLINE int set_int_option(int fd, int level, int name, int value) noexcept {
    return ::ff_setsockopt(fd, level, name, &value, sizeof(value)) == 0 ? 0 : -errno;
}

QUARK_ALWAYS_INLINE int set_nodelay(int fd, bool on) noexcept {
    return set_int_option(fd, IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0);
}

QUARK_ALWAYS_INLINE int set_quickack(int /*fd*/, bool /*on*/) noexcept { return 0; }

QUARK_ALWAYS_INLINE int set_recv_buf(int fd, int bytes) noexcept {
    return set_int_option(fd, SOL_SOCKET, SO_RCVBUF, bytes);
}

QUARK_ALWAYS_INLINE int set_send_buf(int fd, int bytes) noexcept {
    return set_int_option(fd, SOL_SOCKET, SO_SNDBUF, bytes);
}

QUARK_ALWAYS_INLINE int bind_local(int fd, uint32_t ip_be) noexcept {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip_be;
    return ::ff_bind(fd, reinterpret_cast<const linux_sockaddr*>(&sa), sizeof(sa)) == 0 ? 0 : -errno;
}

QUARK_ALWAYS_INLINE int connect(int fd, const sockaddr_in& sa) noexcept {
    return ::ff_connect(fd, reinterpret_cast<const linux_sockaddr*>(&sa), sizeof(sa)) == 0 ? 0 : -errno;
}

QUARK_ALWAYS_INLINE int socket_error(int fd) noexcept {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::ff_getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return errno;
    return err;
}

/// ENOTCONN until the handshake completes: writing earlier fails on FreeBSD (Linux just EAGAINs).
QUARK_ALWAYS_INLINE bool peer_connected(int fd) noexcept {
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    return ::ff_getpeername(fd, reinterpret_cast<linux_sockaddr*>(&sa), &len) == 0;
}

QUARK_ALWAYS_INLINE io_result recv(int fd, void* buf, std::size_t len) noexcept {
    const ssize_t n = ::ff_recv(fd, buf, len, 0);
    return n >= 0 ? static_cast<io_result>(n) : -errno;
}

QUARK_ALWAYS_INLINE io_result send(int fd, const void* buf, std::size_t len, bool /*more*/) noexcept {
    const ssize_t n = ::ff_send(fd, buf, len, 0);
    return n >= 0 ? static_cast<io_result>(n) : -errno;
}

#else

/// Nonblocking, close-on-exec IPv4 TCP socket. Returns the fd or -errno.
QUARK_ALWAYS_INLINE int tcp_socket() noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return fd >= 0 ? fd : -errno;
}

QUARK_ALWAYS_INLINE void close(int fd) noexcept {
    if (fd >= 0) ::close(fd);
}

QUARK_ALWAYS_INLINE int set_nonblock(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -errno;
    if (flags & O_NONBLOCK) return 0;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -errno;
}

QUARK_ALWAYS_INLINE int set_int_option(int fd, int level, int name, int value) noexcept {
    return ::setsockopt(fd, level, name, &value, sizeof(value)) == 0 ? 0 : -errno;
}

QUARK_ALWAYS_INLINE int set_nodelay(int fd, bool on) noexcept {
    return set_int_option(fd, IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0);
}

/// One-shot on Linux: the kernel clears it again after the next ACK decision.
QUARK_ALWAYS_INLINE int set_quickack(int fd, bool on) noexcept {
    return set_int_option(fd, IPPROTO_TCP, TCP_QUICKACK, on ? 1 : 0);
}

QUARK_ALWAYS_INLINE int set_recv_buf(int fd, int bytes) noexcept {
    return set_int_option(fd, SOL_SOCKET, SO_RCVBUF, bytes);
}

QUARK_ALWAYS_INLINE int set_send_buf(int fd, int bytes) noexcept {
    return set_int_option(fd, SOL_SOCKET, SO_SNDBUF, bytes);
}

/// Binds to a local IPv4 address (network byte order) and an ephemeral port, to pick the
/// source address of an outgoing connection. Returns 0 or -errno.
QUARK_ALWAYS_INLINE int bind_local(int fd, uint32_t ip_be) noexcept {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip_be;
    sa.sin_port = 0;
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0 ? 0 : -errno;
}

/// Starts a nonblocking connect. Returns 0 (connected), -EINPROGRESS, or another -errno.
QUARK_ALWAYS_INLINE int connect(int fd, const sockaddr_in& sa) noexcept {
    return ::connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0 ? 0 : -errno;
}

/// Pending error of a connecting socket (SO_ERROR), 0 if none.
QUARK_ALWAYS_INLINE int socket_error(int fd) noexcept {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return errno;
    return err;
}

/// Whether a connecting socket has completed the handshake.
QUARK_ALWAYS_INLINE bool peer_connected(int fd) noexcept {
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    return ::getpeername(fd, reinterpret_cast<sockaddr*>(&sa), &len) == 0;
}

QUARK_ALWAYS_INLINE io_result recv(int fd, void* buf, std::size_t len) noexcept {
    const ssize_t n = ::recv(fd, buf, len, MSG_DONTWAIT);
    return n >= 0 ? static_cast<io_result>(n) : -errno;
}

/// `more`: more data follows immediately (MSG_MORE), lets the kernel coalesce segments.
QUARK_ALWAYS_INLINE io_result send(int fd, const void* buf, std::size_t len, bool more) noexcept {
    const ssize_t n = ::send(fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL | (more ? MSG_MORE : 0));
    return n >= 0 ? static_cast<io_result>(n) : -errno;
}

#endif

} // namespace net::sys

QUARK_END_NAMESPACE
