#pragma once

// Loopback servers for the quark::net integration tests: a TCP server running a handler per
// accepted connection on its own thread, optionally wrapped in TLS (OpenSSL, blocking), plus the
// server side of the WebSocket protocol. Deliberately simple blocking code, independent of the
// client implementation under test.

#include "quark/net/Endpoint.h"
#include "quark/net/EventLoop.h"
#include "quark/net/TlsContext.h"
#include "quark/net/WsFrame.h"
#include "quark/net/WsHandshake.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace quark::net::test {

inline std::string data_path(const char* file) { return std::string(QUARK_TEST_DATA_DIR) + "/net/data/" + file; }

/// Client TLS options trusting (only) the self-signed test certificate.
inline TlsContext::Options trusting_test_ca() {
    TlsContext::Options o;
    o.verify_peer = true;
    o.ca_file = data_path("localhost.crt");
    return o;
}

/// Blocking server side of one connection; reads time out after 5 s.
class ServerConn {
public:
    ServerConn(int fd, SSL* ssl) : fd_{fd}, ssl_{ssl} {}
    ~ServerConn() {
        if (ssl_ != nullptr) {
            ::SSL_shutdown(ssl_);
            ::SSL_free(ssl_);
        }
        ::close(fd_);
    }

    ServerConn(const ServerConn&) = delete;
    ServerConn& operator=(const ServerConn&) = delete;

    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] SSL* ssl() const { return ssl_; }

    /// Bytes read, 0 on EOF, < 0 on error or timeout. Serves buffered leftovers first.
    int read_some(void* buf, std::size_t len) {
        if (!leftover_.empty()) {
            const std::size_t n = std::min(len, leftover_.size());
            std::memcpy(buf, leftover_.data(), n);
            leftover_.erase(0, n);
            return static_cast<int>(n);
        }
        if (ssl_ != nullptr) return ::SSL_read(ssl_, buf, static_cast<int>(len));
        return static_cast<int>(::recv(fd_, buf, len, 0));
    }

    bool read_exact(void* buf, std::size_t len) {
        auto* p = static_cast<uint8_t*>(buf);
        while (len > 0) {
            const int n = read_some(p, len);
            if (n <= 0) return false;
            p += n;
            len -= static_cast<std::size_t>(n);
        }
        return true;
    }

    bool write_all(const void* buf, std::size_t len) {
        const auto* p = static_cast<const uint8_t*>(buf);
        while (len > 0) {
            const int n = ssl_ != nullptr ? ::SSL_write(ssl_, p, static_cast<int>(len))
                                          : static_cast<int>(::send(fd_, p, len, MSG_NOSIGNAL));
            if (n <= 0) return false;
            p += n;
            len -= static_cast<std::size_t>(n);
        }
        return true;
    }
    bool write_all(std::string_view s) { return write_all(s.data(), s.size()); }

    /// Reads up to and including `delim`; bytes after it are kept for later reads. "" on failure.
    std::string read_until(std::string_view delim) {
        std::string got;
        char buf[4096];
        for (;;) {
            if (const auto pos = got.find(delim); pos != std::string::npos) {
                leftover_ = got.substr(pos + delim.size()) + leftover_;
                return got.substr(0, pos + delim.size());
            }
            const int n = read_some(buf, sizeof(buf));
            if (n <= 0) return {};
            got.append(buf, static_cast<std::size_t>(n));
        }
    }

    /// Batch writes into as few segments as possible (TCP_CORK on / off).
    void cork(bool on) {
        const int v = on ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_CORK, &v, sizeof(v));
    }

    // ---------------------------------------------------------------- WebSocket server side

    /// Reads the upgrade request and answers 101 with the correct accept. Returns the request,
    /// "" on failure.
    std::string accept_websocket() {
        std::string req = read_until("\r\n\r\n");
        if (req.empty()) return {};
        const std::string_view key = header_value(req, "Sec-WebSocket-Key");
        char accept[28];
        compute_ws_accept(key, accept);
        const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " +
                                 std::string(accept, 28) + "\r\n\r\n";
        return write_all(resp) ? req : std::string{};
    }

    static std::string_view header_value(std::string_view req, std::string_view name) {
        const std::string needle = "\r\n" + std::string(name) + ": ";
        const auto pos = req.find(needle);
        if (pos == std::string_view::npos) return {};
        const auto start = pos + needle.size();
        return req.substr(start, req.find("\r\n", start) - start);
    }

    /// An unmasked server frame.
    static std::string frame(WsOpcode op, std::string_view payload, bool fin = true) {
        std::string f;
        f += static_cast<char>((fin ? 0x80 : 0) | static_cast<int>(op));
        if (payload.size() < 126) {
            f += static_cast<char>(payload.size());
        } else if (payload.size() <= 0xFFFF) {
            f += static_cast<char>(126);
            f += static_cast<char>(payload.size() >> 8);
            f += static_cast<char>(payload.size() & 0xFF);
        } else {
            f += static_cast<char>(127);
            for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<uint64_t>(payload.size()) >> (8 * i)) & 0xFF);
        }
        f.append(payload);
        return f;
    }

    bool send_frame(WsOpcode op, std::string_view payload, bool fin = true) { return write_all(frame(op, payload, fin)); }

    struct Frame {
        WsOpcode opcode;
        bool fin;
        bool masked;
        std::string payload;
    };

    /// Reads and unmasks one client frame.
    std::optional<Frame> read_frame() {
        uint8_t h[2];
        if (!read_exact(h, 2)) return std::nullopt;
        Frame f{static_cast<WsOpcode>(h[0] & 0x0F), (h[0] & 0x80) != 0, (h[1] & 0x80) != 0, {}};
        uint64_t len = h[1] & 0x7F;
        if (len == 126) {
            uint8_t e[2];
            if (!read_exact(e, 2)) return std::nullopt;
            len = (uint64_t{e[0]} << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (!read_exact(e, 8)) return std::nullopt;
            len = 0;
            for (const uint8_t b : e) len = (len << 8) | b;
        }
        uint8_t key[4] = {};
        if (f.masked && !read_exact(key, 4)) return std::nullopt;
        f.payload.resize(len);
        if (len > 0 && !read_exact(f.payload.data(), len)) return std::nullopt;
        for (std::size_t i = 0; i < len; ++i) f.payload[i] = static_cast<char>(f.payload[i] ^ key[i & 3]);
        return f;
    }

private:
    int fd_;
    SSL* ssl_;
    std::string leftover_;
};

/// Accepts connections on 127.0.0.1 (ephemeral port) and runs `handler` for each, one at a time,
/// on a background thread.
class TestServer {
public:
    using Handler = std::function<void(ServerConn&, TestServer&)>;

    struct Options {
        bool tls{false};
        int rcvbuf{0};  ///< SO_RCVBUF of accepted sockets (0: default), to provoke backpressure
    };

    explicit TestServer(Handler handler) : TestServer(std::move(handler), Options{}) {}

    TestServer(Handler handler, const Options& options) : handler_{std::move(handler)}, opts_{options} {
        // OpenSSL's blocking writes (and SSL_shutdown) to a client that already closed would
        // raise SIGPIPE; quark's own sends use MSG_NOSIGNAL
        std::signal(SIGPIPE, SIG_IGN);
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (opts_.rcvbuf > 0) ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVBUF, &opts_.rcvbuf, sizeof(opts_.rcvbuf));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(listen_fd_, 16);
        socklen_t len = sizeof(a);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        if (opts_.tls) {
            ctx_ = ::SSL_CTX_new(TLS_server_method());
            ::SSL_CTX_use_certificate_file(ctx_, data_path("localhost.crt").c_str(), SSL_FILETYPE_PEM);
            ::SSL_CTX_use_PrivateKey_file(ctx_, data_path("localhost.key").c_str(), SSL_FILETYPE_PEM);
        }
        thread_ = std::thread([this] { run(); });
    }

    ~TestServer() {
        stop();
        if (ctx_ != nullptr) ::SSL_CTX_free(ctx_);
    }

    void stop() {
        stopping_ = true;
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    [[nodiscard]] Endpoint endpoint() const { return *Endpoint::from_ip("127.0.0.1", port_); }
    [[nodiscard]] uint16_t port() const { return port_; }
    [[nodiscard]] int accepted() const { return accepted_.load(); }
    [[nodiscard]] bool stopping() const { return stopping_.load(); }

    /// Keeps a handler's connection open until the server stops.
    void hold() const {
        while (!stopping_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    /// The peer address of the last accepted connection.
    [[nodiscard]] std::string last_peer_ip() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_peer_ip_;
    }

private:
    Handler handler_;
    Options opts_;
    int listen_fd_{-1};
    uint16_t port_{0};
    SSL_CTX* ctx_{nullptr};
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepted_{0};
    mutable std::mutex mu_;
    std::string last_peer_ip_;

    void run() {
        while (!stopping_) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (fd < 0) continue;
            {
                char ip[INET_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                std::lock_guard<std::mutex> lock(mu_);
                last_peer_ip_ = ip;
            }
            ++accepted_;
            timeval tv{5, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            SSL* ssl = nullptr;
            if (ctx_ != nullptr) {
                ssl = ::SSL_new(ctx_);
                ::SSL_set_fd(ssl, fd);
                if (::SSL_accept(ssl) != 1) {
                    ::ERR_clear_error();
                    ::SSL_free(ssl);
                    ::close(fd);
                    continue;
                }
            }
            ServerConn conn(fd, ssl);
            handler_(conn, *this);
        }
    }
};

/// Steps `loop` until `done()` or the timeout. Returns done().
template <class Pred>
bool run_until(EventLoop& loop, Pred done, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        loop.step();
    }
    return true;
}

/// Steps `loop` for `duration`.
inline void run_for(EventLoop& loop, std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) loop.step();
}

} // namespace quark::net::test
