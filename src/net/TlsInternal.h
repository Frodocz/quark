#pragma once

#include <openssl/ssl.h>

#include <string>

namespace quark::net::detail {

/// SSL ex_data slot holding the owning TlsStream, for OpenSSL callbacks.
int tls_stream_ex_index();

/// SSL_CTX new-session callback: hands a resumable session to its TlsStream (takes ownership).
int on_new_tls_session(SSL* ssl, SSL_SESSION* session);

/// The first queued OpenSSL error as text (and clears the queue).
std::string openssl_error_string();

} // namespace quark::net::detail
