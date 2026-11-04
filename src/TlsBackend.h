#pragma once

// The TLS/crypto library behind quark::crypto and quark::net, chosen by QUARK_TLS_BACKEND:
// AWS-LC (default) or OpenSSL >= 3.0. Both expose the OpenSSL API under <openssl/...>; this
// header checks that the headers on the include path belong to the library being linked.

#include <openssl/crypto.h>  // AWS-LC: includes base.h, which defines OPENSSL_IS_AWSLC
#include <openssl/opensslv.h>

#if defined(QUARK_TLS_AWSLC)
    #if !defined(OPENSSL_IS_AWSLC)
        #error "QUARK_TLS_BACKEND=AWS-LC, but the openssl/ headers found are not AWS-LC's (system OpenSSL first on the include path?)"
    #endif
#else
    #if defined(OPENSSL_IS_AWSLC) || defined(OPENSSL_IS_BORINGSSL)
        #error "QUARK_TLS_BACKEND=OpenSSL, but the openssl/ headers found are AWS-LC's or BoringSSL's"
    #endif
    #if OPENSSL_VERSION_NUMBER < 0x30000000L
        #error "quark requires OpenSSL 3.0 or newer (or AWS-LC)"
    #endif
#endif
