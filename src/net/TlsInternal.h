#pragma once

#include "TlsBackend.h"
#include "quark/Attributes.h"

#include <openssl/ssl.h>

#include <string>

QUARK_BEGIN_NAMESPACE

namespace net::detail {

/// SSL ex_data slot holding the owning TlsStream, for OpenSSL callbacks.
int tls_stream_ex_index();

/// SSL_CTX new-session callback: hands a resumable session to its TlsStream (takes ownership).
int on_new_tls_session(SSL* ssl, SSL_SESSION* session);

/// The first queued OpenSSL error as text (and clears the queue).
std::string openssl_error_string();

} // namespace net::detail

QUARK_END_NAMESPACE
