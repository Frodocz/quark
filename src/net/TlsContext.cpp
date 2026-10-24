#include "quark/net/TlsContext.h"

#include "TlsInternal.h"
#include "quark/QuarkError.h"
#include "quark/QuarkLog.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

QUARK_BEGIN_NAMESPACE

namespace net {

namespace detail {

std::string openssl_error_string() {
    char buf[256] = "unknown OpenSSL error";
    if (const unsigned long e = ::ERR_get_error(); e != 0) ::ERR_error_string_n(e, buf, sizeof(buf));
    ::ERR_clear_error();
    return buf;
}

} // namespace detail

namespace {

[[noreturn]] void throw_ssl(const char* what) {
    QUARK_THROW(QuarkError{std::string("TlsContext: ") + what + ": " + detail::openssl_error_string()});
#if defined(QUARK_NO_EXCEPTIONS)
    std::abort();  // unreachable: QUARK_THROW aborts
#endif
}

} // namespace

TlsContext::TlsContext(const Options& options)
    : verify_peer_{options.verify_peer}, session_resumption_{options.session_resumption} {
    ctx_ = ::SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr) throw_ssl("SSL_CTX_new failed");

    // TLS 1.2 floor; prefer AES-128-GCM for TLS 1.3 (AES-NI, 10 rounds vs 14 for AES-256),
    // which OpenSSL otherwise ranks after AES-256-GCM
    if (::SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION) != 1) throw_ssl("set_min_proto_version failed");
    if (::SSL_CTX_set_ciphersuites(ctx_, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256") != 1) {
        throw_ssl("set_ciphersuites failed");
    }

    if (!options.ca_file.empty()) {
        if (::SSL_CTX_load_verify_locations(ctx_, options.ca_file.c_str(), nullptr) != 1) {
            throw_ssl("load_verify_locations failed");
        }
    } else if (::SSL_CTX_set_default_verify_paths(ctx_) != 1) {
        throw_ssl("set_default_verify_paths failed");
    }
    ::SSL_CTX_set_verify(ctx_, options.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

    if (!options.alpn.empty()) {
        std::string wire;  // length-prefixed protocol names (RFC 7301)
        for (const auto& proto : options.alpn) {
            if (proto.empty() || proto.size() > 255) {
                QUARK_THROW(QuarkError{"TlsContext: ALPN protocol names must be 1..255 bytes"});
            }
            wire += static_cast<char>(proto.size());
            wire += proto;
        }
        if (::SSL_CTX_set_alpn_protos(ctx_, reinterpret_cast<const unsigned char*>(wire.data()),
                                      static_cast<unsigned>(wire.size())) != 0) {
            throw_ssl("set_alpn_protos failed");
        }
    }

    if (options.session_resumption) {
        // sessions are kept by each TlsStream for its own reconnects, not in a shared cache
        ::SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        ::SSL_CTX_sess_set_new_cb(ctx_, &detail::on_new_tls_session);
    } else {
        ::SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_OFF);
        ::SSL_CTX_set_options(ctx_, SSL_OP_NO_TICKET);
    }

    // SSL_write() may return after one record of a large buffer, like a partial send()
    ::SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    QUARK_LOG_INFO("TlsContext ready (verify_peer={}, session_resumption={})", verify_peer_, session_resumption_);
}

TlsContext::~TlsContext() { ::SSL_CTX_free(ctx_); }

} // namespace net

QUARK_END_NAMESPACE
