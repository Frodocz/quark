#pragma once

#include "quark/Attributes.h"
#include "quark/QuarkLog.h"
#include "quark/net/Endpoint.h"
#include "quark/net/EventLoop.h"
#include "quark/net/SysApi.h"
#include "quark/net/TcpSocket.h"
#include "quark/utils/InplaceFunction.h"

#include <cerrno>
#include <chrono>
#include <cstring>

QUARK_BEGIN_NAMESPACE

namespace net {

/// Accepts TCP connections on the EventLoop: each one is handed over as a connected,
/// nonblocking TcpSocket (e.g. an admin or test port next to the trading connections).
class TcpListener final : public Pollable {
public:
    using AcceptHandler = InplaceFunction<void(TcpSocket&&, const Endpoint& peer), 64>;

    explicit TcpListener(EventLoop& loop) : loop_{loop} {}
    ~TcpListener() override { close(); }

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    void on_accept(AcceptHandler h) { on_accept_ = std::move(h); }

    /// Listens on `local` (ip 0: every interface; port 0: an ephemeral one, see local()).
    QUARK_NOINLINE QUARK_ATTRIBUTE_COLD bool listen(const Endpoint& local, int backlog = 64) {
        close();
        const int fd = sys::listen_tcp(local.to_sockaddr(), backlog);
        if (fd < 0) {
            QUARK_LOG_ERROR("TcpListener: listen on {} failed: {}", local.to_string(), std::strerror(-fd));
            return false;
        }
        fd_ = fd;
        sockaddr_in sa{};
        sys::local_address(fd_, sa);
        local_ = {sa.sin_addr.s_addr, sa.sin_port};
        if (!loop_.add(*this, false)) {
            close();
            return false;
        }
        return true;
    }

    void close() noexcept {
        if (fd_ < 0) return;
        loop_.remove(*this);
        sys::close(fd_);
        fd_ = -1;
    }

    /// The bound address (the actual port when listening on port 0).
    [[nodiscard]] const Endpoint& local() const noexcept { return local_; }
    [[nodiscard]] bool listening() const noexcept { return fd_ >= 0; }

    [[nodiscard]] int fd() const noexcept override { return fd_; }

    void on_ready(uint32_t /*flags*/) override {
        while (fd_ >= 0) {
            sockaddr_in peer{};
            const int c = sys::accept(fd_, peer);
            if (c < 0) {
                if (c != -EAGAIN && c != -EWOULDBLOCK) {  // e.g. EMFILE: retried every step
                    QUARK_LOG_WARN_LIMIT(std::chrono::seconds(1), "TcpListener: accept failed: {}", std::strerror(-c));
                }
                return;
            }
            loop_.note_work();
            TcpSocket s = TcpSocket::adopt(c);
            if (on_accept_) on_accept_(std::move(s), Endpoint{peer.sin_addr.s_addr, peer.sin_port});
        }
    }

private:
    EventLoop& loop_;
    int fd_{-1};
    Endpoint local_{};
    AcceptHandler on_accept_;
};

} // namespace net

QUARK_END_NAMESPACE
