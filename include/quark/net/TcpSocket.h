#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkLog.h"
#include "quark/net/Endpoint.h"
#include "quark/net/Stream.h"
#include "quark/net/SysApi.h"

#include <cstring>
#include <span>
#include <utility>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Nonblocking TCP connection (TCP_NODELAY), satisfying Stream.
/// No buffering: that lives in the protocol layers.
class TcpSocket {
public:
    enum class State : uint8_t { Idle, Connecting, Connected, Failed, Closed };

    TcpSocket() = default;
    ~TcpSocket() { close(); }

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& o) noexcept
        : fd_{std::exchange(o.fd_, -1)}, state_{std::exchange(o.state_, State::Idle)}, local_ip_be_{o.local_ip_be_} {}
    TcpSocket& operator=(TcpSocket&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = std::exchange(o.fd_, -1);
            state_ = std::exchange(o.state_, State::Idle);
            local_ip_be_ = o.local_ip_be_;
        }
        return *this;
    }

    /// Source address for the next connect() (network byte order), 0 for the kernel's choice.
    /// E.g. to spread connections over several local IPs against per-IP rate limits.
    void set_local_ip(uint32_t ip_be) noexcept { local_ip_be_ = ip_be; }

    // ---------------------------------------------------------------- Stream

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD bool connect(const Endpoint& remote) {
        close();
        if (!remote.valid()) {
            // 0.0.0.0 would reach this host: an unresolved endpoint must not connect anywhere
            QUARK_LOG_ERROR("TcpSocket: connect to an unset address ({})", remote.to_string());
            state_ = State::Failed;
            return false;
        }
        const int fd = sys::tcp_socket();
        if (fd < 0) {
            QUARK_LOG_ERROR("TcpSocket: socket() failed: {}", std::strerror(-fd));
            state_ = State::Failed;
            return false;
        }
        fd_ = fd;
        sys::set_nodelay(fd_, true);
        if (local_ip_be_ != 0) {
            if (const int rc = sys::bind_local(fd_, local_ip_be_); rc != 0) {
                QUARK_LOG_ERROR("TcpSocket: bind to local ip failed: {}", std::strerror(-rc));
                fail();
                return false;
            }
        }
        const int rc = sys::connect(fd_, remote.to_sockaddr());
        if (rc == 0) {
            state_ = State::Connected;
        } else if (rc == -EINPROGRESS) {
            state_ = State::Connecting;
        } else {
            QUARK_LOG_WARN("TcpSocket: connect to {} failed: {}", remote.to_string(), std::strerror(-rc));
            fail();
            return false;
        }
        return true;
    }

    StreamPhase poll_open() noexcept {
        if (state_ == State::Connecting) {
            if (const int err = sys::socket_error(fd_); err != 0) {
                QUARK_LOG_WARN("TcpSocket: connect failed: {}", std::strerror(err));
                fail();
            } else if (sys::peer_connected(fd_)) {
                // SO_ERROR == 0 also while still connecting: getpeername tells them apart
                state_ = State::Connected;
            }
        }
        switch (state_) {
            case State::Connected: return StreamPhase::Open;
            case State::Failed: return StreamPhase::Failed;
            case State::Connecting: return StreamPhase::Connecting;
            default: return StreamPhase::Closed;  // Idle (never connected) or Closed
        }
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool closed() const noexcept { return fd_ < 0; }

    QUARK_ALWAYS_INLINE sys::io_result read(std::span<uint8_t> dst) noexcept { return sys::recv(fd_, dst.data(), dst.size()); }

    QUARK_ALWAYS_INLINE sys::io_result write(std::span<const uint8_t> src, bool more) noexcept {
        return sys::send(fd_, src.data(), src.size(), more);
    }

    /// Nothing is buffered here: every write goes straight to the socket.
    [[nodiscard]] static constexpr bool wants_write() noexcept { return false; }
    static constexpr sys::io_result flush() noexcept { return 0; }

    void close() noexcept {
        if (fd_ >= 0) {
            sys::close(fd_);
            fd_ = -1;
        }
        if (state_ != State::Failed) state_ = State::Closed;
    }

    [[nodiscard]] State state() const noexcept { return state_; }

    void set_quickack(bool on) noexcept { sys::set_quickack(fd_, on); }
    void set_recv_buf(int bytes) noexcept { sys::set_recv_buf(fd_, bytes); }
    void set_send_buf(int bytes) noexcept { sys::set_send_buf(fd_, bytes); }

private:
    int fd_{-1};
    State state_{State::Idle};
    uint32_t local_ip_be_{0};

    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD void fail() noexcept {
        sys::close(fd_);
        fd_ = -1;
        state_ = State::Failed;
    }
};

static_assert(Stream<TcpSocket>);

} // namespace net

QUARK_END_NAMESPACE
