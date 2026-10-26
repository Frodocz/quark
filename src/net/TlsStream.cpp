#include "quark/net/TlsStream.h"

#include "TlsInternal.h"
#include "quark/QuarkLog.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>

#include <climits>

QUARK_BEGIN_NAMESPACE

namespace net {

namespace {
constexpr std::size_t kOutStaging = 16 * 1024 + 512;  // one full TLS record plus overhead
constexpr std::size_t kMaxPendingIn = 256 * 1024;     // stop reading the socket past this
}  // namespace

// gives the OpenSSL callback access to the private session slot
struct TlsStreamAccess {
    static void set_session(TlsStream& s, SSL_SESSION* session) {
        if (s.session_ != nullptr) ::SSL_SESSION_free(s.session_);
        s.session_ = session;
    }
};

namespace detail {

int tls_stream_ex_index() {
    static const int index = ::SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int on_new_tls_session(SSL* ssl, SSL_SESSION* session) {
    auto* stream = static_cast<TlsStream*>(::SSL_get_ex_data(ssl, tls_stream_ex_index()));
    if (stream == nullptr) return 0;  // not ours: OpenSSL frees it
    TlsStreamAccess::set_session(*stream, session);
    return 1;  // we keep the reference
}

} // namespace detail

TlsStream::TlsStream() : out_{std::make_unique<uint8_t[]>(kOutStaging)} {}

TlsStream::~TlsStream() {
    close();
    if (session_ != nullptr) ::SSL_SESSION_free(session_);
}

bool TlsStream::connect(const Endpoint& remote) {
    close();
    if (ctx_ == nullptr) {
        QUARK_LOG_ERROR("TlsStream: connect without a TlsContext");
        phase_ = Phase::Failed;
        return false;
    }
    if (ctx_->verify_peer() && host_.empty()) {
        // a verified chain without a hostname check accepts any valid certificate
        QUARK_LOG_ERROR("TlsStream: peer verification needs a hostname, call set_hostname()");
        phase_ = Phase::Failed;
        return false;
    }
    if (!tcp_.connect(remote)) {
        phase_ = Phase::Failed;
        return false;
    }
    phase_ = Phase::ConnectingTcp;
    return true;
}

bool TlsStream::begin_handshake() {
    ssl_ = ::SSL_new(ctx_->raw());
    rbio_ = ::BIO_new(BIO_s_mem());
    wbio_ = ::BIO_new(BIO_s_mem());
    if (ssl_ == nullptr || rbio_ == nullptr || wbio_ == nullptr) {
        // nothing is attached yet: free each one on its own
        ::SSL_free(ssl_);
        ::BIO_free(rbio_);
        ::BIO_free(wbio_);
        ssl_ = nullptr;
        rbio_ = wbio_ = nullptr;
        QUARK_LOG_ERROR("TlsStream: SSL/BIO allocation failed: {}", detail::openssl_error_string());
        return false;
    }
    ::BIO_set_mem_eof_return(rbio_, -1);  // empty read BIO means "retry", not EOF
    ::SSL_set_bio(ssl_, rbio_, wbio_);    // SSL owns both BIOs from here
    ::SSL_set_connect_state(ssl_);
    ::SSL_set_ex_data(ssl_, detail::tls_stream_ex_index(), this);

    if (!host_.empty()) {
        in_addr ip{};
        const bool is_ip = ::inet_pton(AF_INET, host_.c_str(), &ip) == 1;
        if (!is_ip) ::SSL_set_tlsext_host_name(ssl_, host_.c_str());  // no SNI for IP literals (RFC 6066)
        if (ctx_->verify_peer()) {
            if (is_ip) {
                ::X509_VERIFY_PARAM_set1_ip_asc(::SSL_get0_param(ssl_), host_.c_str());
            } else {
                ::SSL_set_hostflags(ssl_, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                ::SSL_set1_host(ssl_, host_.c_str());
            }
        }
    }
    if (ctx_->session_resumption() && session_ != nullptr) ::SSL_set_session(ssl_, session_);
    phase_ = Phase::Handshaking;
    return true;
}

StreamPhase TlsStream::poll_open() {
    if (phase_ == Phase::ConnectingTcp) {
        const StreamPhase tp = tcp_.poll_open();
        if (tp == StreamPhase::Connecting) return StreamPhase::Connecting;
        if (tp != StreamPhase::Open || !begin_handshake()) {
            fail();
            return StreamPhase::Failed;
        }
    }
    if (phase_ == Phase::Handshaking) {
        for (;;) {
            const int r = ::SSL_do_handshake(ssl_);
            if (r == 1) {
                if (!flush_out()) {
                    fail();
                    return StreamPhase::Failed;
                }
                phase_ = Phase::Established;
                resumed_ = ::SSL_session_reused(ssl_) == 1;
                QUARK_LOG_INFO("TlsStream: handshake with {} done: {}{}", host_, description(),
                               resumed_ ? " (resumed)" : "");
                return StreamPhase::Open;
            }
            const int err = ::SSL_get_error(ssl_, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                if (!flush_out()) {
                    fail();
                    return StreamPhase::Failed;
                }
                const int got = pump_in();
                if (got < 0 || eof_) {
                    QUARK_LOG_WARN("TlsStream: {} closed the connection during the handshake", host_);
                    forget_session();  // don't offer it again: it may be what the server rejects
                    fail();
                    return StreamPhase::Failed;
                }
                if (got > 0) continue;  // new handshake bytes: run OpenSSL again right away
                return StreamPhase::Connecting;
            }
            const long verify = ::SSL_get_verify_result(ssl_);
            if (verify != X509_V_OK) {
                QUARK_LOG_ERROR("TlsStream: certificate verification of {} failed: {}", host_,
                                ::X509_verify_cert_error_string(verify));
            } else {
                QUARK_LOG_ERROR("TlsStream: handshake with {} failed: {}", host_, detail::openssl_error_string());
            }
            forget_session();
            fail();
            return StreamPhase::Failed;
        }
    }
    switch (phase_) {
        case Phase::Established: return StreamPhase::Open;
        case Phase::Closed: return StreamPhase::Closed;
        default: return StreamPhase::Failed;
    }
}

bool TlsStream::flush_out() noexcept {
    for (;;) {
        if (out_off_ == out_len_) {
            // staging drained: refill it from OpenSSL, oldest ciphertext first
            out_off_ = out_len_ = 0;
            const int n = ::BIO_read(wbio_, out_.get(), static_cast<int>(kOutStaging));
            if (n <= 0) return true;  // nothing pending
            out_len_ = static_cast<std::size_t>(n);
        }
        const sys::io_result w = tcp_.write({out_.get() + out_off_, out_len_ - out_off_}, false);
        if (w == -EAGAIN) return true;  // stays staged: sent before anything newer
        if (w < 0) return false;
        out_off_ += static_cast<std::size_t>(w);
    }
}

// A short read means the socket had no more at that moment: stop there instead of paying one
// more recv() for the -EAGAIN. read() only reports -EAGAIN after a recv() did, so edge-triggered
// callers still drain the socket.
int TlsStream::pump_in() noexcept {
    alignas(64) thread_local uint8_t buf[16 * 1024];
    int moved = 0;
    while (::BIO_ctrl_pending(rbio_) < kMaxPendingIn) {
        const sys::io_result r = tcp_.read(buf);
        if (r > 0) {
            ::BIO_write(rbio_, buf, static_cast<int>(r));
            moved += static_cast<int>(r);
            if (static_cast<std::size_t>(r) < sizeof(buf)) break;
        } else if (r == -EAGAIN) {
            break;
        } else if (r == 0) {
            eof_ = true;
            break;
        } else {
            return -1;
        }
    }
    return moved;
}

sys::io_result TlsStream::read(std::span<uint8_t> dst) noexcept {
    if (phase_ != Phase::Established) [[unlikely]] {
        if (phase_ == Phase::Closed) return 0;
        return phase_ == Phase::Failed ? -EIO : -EAGAIN;
    }
    const int want = dst.size() > INT_MAX ? INT_MAX : static_cast<int>(dst.size());
    for (;;) {
        const int n = ::SSL_read(ssl_, dst.data(), want);
        if (n > 0) return n;
        const int err = ::SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // OpenSSL has nothing decrypted: send what it may want to (e.g. a key update reply),
            // then pull ciphertext from the socket; only report -EAGAIN once that is empty too
            if (!flush_out()) {
                fail();
                return -EIO;
            }
            const int got = pump_in();
            if (got < 0) {
                fail();
                return -EIO;
            }
            if (got > 0) continue;
            if (eof_) return 0;  // peer closed the TCP connection without close_notify
            return -EAGAIN;
        }
        if (err == SSL_ERROR_ZERO_RETURN) return 0;  // close_notify
        QUARK_LOG_WARN("TlsStream: read from {} failed: {}", host_, detail::openssl_error_string());
        fail();
        return -EIO;
    }
}

sys::io_result TlsStream::write(std::span<const uint8_t> src, bool /*more*/) noexcept {
    if (phase_ != Phase::Established) [[unlikely]] {
        if (phase_ == Phase::Closed) return -EPIPE;
        return phase_ == Phase::Failed ? -EIO : -EAGAIN;
    }
    if (src.empty()) return 0;
    if (pending_ciphertext() >= kMaxPendingCiphertext) {
        if (!flush_out()) {
            fail();
            return -EIO;
        }
        if (pending_ciphertext() >= kMaxPendingCiphertext) return -EAGAIN;  // peer isn't reading
    }
    const int len = src.size() > INT_MAX ? INT_MAX : static_cast<int>(src.size());
    const int n = ::SSL_write(ssl_, src.data(), len);
    if (n <= 0) {
        const int err = ::SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return -EAGAIN;
        QUARK_LOG_WARN("TlsStream: write to {} failed: {}", host_, detail::openssl_error_string());
        fail();
        return -EIO;
    }
    if (!flush_out()) {
        fail();
        return -EIO;
    }
    return n;
}

sys::io_result TlsStream::flush_staged() noexcept {
    if (phase_ != Phase::Established && phase_ != Phase::Handshaking) return 0;  // nothing to send any more
    if (!flush_out()) {
        fail();
        return -EIO;
    }
    return wants_write() ? -EAGAIN : 0;
}

void TlsStream::forget_session() noexcept {
    if (session_ != nullptr) {
        ::SSL_SESSION_free(session_);
        session_ = nullptr;
    }
}

std::size_t TlsStream::pending_ciphertext() const noexcept {
    return (out_len_ - out_off_) + (wbio_ != nullptr ? ::BIO_ctrl_pending(wbio_) : 0);
}

std::string TlsStream::description() const {
    if (ssl_ == nullptr) return {};
    return std::string(::SSL_get_version(ssl_)) + " " + ::SSL_get_cipher_name(ssl_);
}

void TlsStream::close() noexcept {
    if (phase_ == Phase::Established) {
        ::SSL_shutdown(ssl_);  // queue close_notify
        (void)flush_out();
    }
    release_ssl();
    tcp_.close();
    if (phase_ != Phase::Idle) phase_ = Phase::Closed;
}

void TlsStream::fail() noexcept {
    ::ERR_clear_error();
    release_ssl();
    tcp_.close();
    phase_ = Phase::Failed;
}

void TlsStream::release_ssl() noexcept {
    if (ssl_ != nullptr) {
        ::SSL_free(ssl_);  // frees both BIOs
    }
    ssl_ = nullptr;
    rbio_ = wbio_ = nullptr;
    out_off_ = out_len_ = 0;
    eof_ = false;
}

} // namespace net

QUARK_END_NAMESPACE
