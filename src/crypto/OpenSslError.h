#pragma once

#include "quark/QuarkError.h"

#include <openssl/err.h>

#include <string>

namespace quark::crypto::detail {

/// Throws QuarkError carrying `what` and the first queued OpenSSL error (then clears the queue).
[[noreturn]] inline void throw_openssl_error(const char* what) {
    char reason[256] = "unknown OpenSSL error";
    if (const unsigned long e = ::ERR_get_error(); e != 0) ::ERR_error_string_n(e, reason, sizeof(reason));
    ::ERR_clear_error();
    QUARK_THROW(QuarkError{std::string(what) + ": " + reason});
#if defined(QUARK_NO_EXCEPTIONS)
    std::abort();  // unreachable: QUARK_THROW aborts
#endif
}

} // namespace quark::crypto::detail
