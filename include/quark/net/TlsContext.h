#pragma once

#include "quark/Attributes.h"

#include <string>
#include <vector>

// OpenSSL types, kept out of this header (see src/net/TlsContext.cpp)
struct ssl_ctx_st;

QUARK_BEGIN_NAMESPACE

namespace net {

/// Client TLS configuration shared by the TlsStreams of one event loop: CA trust, peer
/// verification, protocol versions, ALPN and session resumption. Cold path; throws QuarkError
/// on misconfiguration.
class TlsContext {
public:
    struct Options {
        /// Verify the server certificate chain and its hostname (TlsStream refuses to connect
        /// without a hostname then). Only turn off for local tests against self-signed servers.
        bool verify_peer{true};
        std::string ca_file;             ///< PEM bundle, empty for the system's default CAs
        std::vector<std::string> alpn;   ///< e.g. {"http/1.1"}, empty for none
        /// Resume the previous session on reconnect: skips certificate verification and the
        /// full key exchange (about half the client-side handshake CPU).
        bool session_resumption{true};
    };

    TlsContext() : TlsContext(Options{}) {}
    explicit TlsContext(const Options& options);
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    [[nodiscard]] ssl_ctx_st* raw() const noexcept { return ctx_; }
    [[nodiscard]] bool verify_peer() const noexcept { return verify_peer_; }
    [[nodiscard]] bool session_resumption() const noexcept { return session_resumption_; }

private:
    ssl_ctx_st* ctx_{nullptr};
    bool verify_peer_;
    bool session_resumption_;
};

} // namespace net

QUARK_END_NAMESPACE
