#pragma once

#include "quark/Attributes.h"
#include "quark/net/Endpoint.h"
#include "quark/net/Stream.h"
#include "quark/net/TcpSocket.h"
#include "quark/net/TlsContext.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// OpenSSL types, kept out of this header (see src/net/TlsStream.cpp)
struct ssl_st;
struct bio_st;
struct ssl_session_st;

QUARK_BEGIN_NAMESPACE

namespace net {

/// TLS client over an owned nonblocking TcpSocket, satisfying Stream: WsSession<TlsStream> is
/// wss://, HttpClient<TlsStream> is https://.
///
/// OpenSSL runs on memory BIOs; this class moves ciphertext between them and the socket:
/// - Outgoing ciphertext the socket doesn't take is staged and sent strictly in order before
///   anything newer. write() returns -EAGAIN once kMaxPendingCiphertext bytes are waiting, so
///   a slow or stalled peer surfaces as backpressure instead of unbounded buffering.
/// - read() returns -EAGAIN only when neither OpenSSL nor the socket has anything left, which is
///   what an edge-triggered poller needs.
/// - write() never reads the socket: no extra syscall on the send path.
/// - wants_write() reports staged ciphertext: with an edge-triggered poller keep write interest
///   while it is true and call flush() on the writable edge, or it waits for the next write().
/// - The session of the last connection is kept and resumed on the next connect() (to the same
///   host and context; a failed handshake forgets it).
///
/// Not movable (OpenSSL callbacks point back to it).
class QUARK_API TlsStream {
public:
    static constexpr std::size_t kMaxPendingCiphertext = 64 * 1024;

    TlsStream();
    ~TlsStream();

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // ---------------------------------------------------------------- configuration (cold path)

    void set_context(TlsContext& ctx) noexcept {
        if (ctx_ != &ctx) forget_session();
        ctx_ = &ctx;
    }
    /// SNI and certificate verification target: the server's DNS name (or IP literal). Required
    /// when the context verifies peers.
    void set_hostname(std::string_view host) {
        if (host != host_) forget_session();  // a session is only valid for the host it came from
        host_.assign(host);
    }
    /// The underlying socket, e.g. for set_local_ip() or buffer sizes.
    [[nodiscard]] TcpSocket& tcp() noexcept { return tcp_; }

    // ---------------------------------------------------------------- Stream

    QUARK_ATTRIBUTE_COLD bool connect(const Endpoint& remote);
    QUARK_ATTRIBUTE_COLD StreamPhase poll_open();
    [[nodiscard]] int fd() const noexcept { return tcp_.fd(); }
    [[nodiscard]] bool closed() const noexcept { return phase_ == Phase::Closed || phase_ == Phase::Failed || phase_ == Phase::Idle; }
    QUARK_ATTRIBUTE_HOT sys::io_result read(std::span<uint8_t> dst) noexcept;
    QUARK_ATTRIBUTE_HOT sys::io_result write(std::span<const uint8_t> src, bool more) noexcept;
    /// Staged ciphertext the socket didn't take yet (see the class comment).
    [[nodiscard]] bool wants_write() const noexcept { return out_off_ != out_len_; }
    QUARK_ALWAYS_INLINE sys::io_result flush() noexcept { return wants_write() ? flush_staged() : 0; }
    /// Sends close_notify (best effort) and closes the socket.
    QUARK_ATTRIBUTE_COLD void close() noexcept;

    // ---------------------------------------------------------------- introspection

    /// Encrypted bytes accepted by write() but not yet handed to the socket.
    [[nodiscard]] std::size_t pending_ciphertext() const noexcept;
    /// Whether the last handshake resumed a previous session.
    [[nodiscard]] bool session_resumed() const noexcept { return resumed_; }
    /// Negotiated protocol and cipher, e.g. "TLSv1.3 TLS_AES_128_GCM_SHA256", once open.
    [[nodiscard]] QUARK_ATTRIBUTE_COLD std::string description() const;

private:
    friend struct TlsStreamAccess;
    enum class Phase : uint8_t { Idle, ConnectingTcp, Handshaking, Established, Closed, Failed };

    QUARK_ATTRIBUTE_COLD bool begin_handshake();
    QUARK_ATTRIBUTE_HOT sys::io_result flush_staged() noexcept;
    QUARK_ATTRIBUTE_COLD void forget_session() noexcept;
    QUARK_ATTRIBUTE_HOT bool flush_out() noexcept;  // false on a dead socket
    QUARK_ATTRIBUTE_HOT int pump_in() noexcept;     // bytes moved into rbio, 0 if none, -1 on a dead socket
    QUARK_ATTRIBUTE_COLD void fail() noexcept;
    void release_ssl() noexcept;

    TcpSocket tcp_;
    TlsContext* ctx_{nullptr};
    ssl_st* ssl_{nullptr};
    bio_st* rbio_{nullptr};  // socket -> OpenSSL
    bio_st* wbio_{nullptr};  // OpenSSL -> socket
    ssl_session_st* session_{nullptr};
    std::string host_;
    std::unique_ptr<uint8_t[]> out_;  // staged ciphertext, out_[out_off_, out_len_) not sent yet
    std::size_t out_off_{0};
    std::size_t out_len_{0};
    Phase phase_{Phase::Idle};
    bool eof_{false};
    bool resumed_{false};
};

static_assert(Stream<TlsStream>);

} // namespace net

QUARK_END_NAMESPACE
