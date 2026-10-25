#include "quark/crypto/Ed25519Signer.h"

#include "OpenSslError.h"
#include "quark/crypto/Base64Encoding.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>

#include <limits>
#include <memory>
#include <utility>

#if OPENSSL_VERSION_NUMBER < 0x30000000L
    #error "quark::crypto requires OpenSSL 3.0 or newer"
#endif

QUARK_BEGIN_NAMESPACE

namespace crypto {

struct Ed25519Signer::State {
    EVP_PKEY* pkey{nullptr};
    EVP_MD_CTX* md_ctx{nullptr};

    ~State() {
        ::EVP_MD_CTX_free(md_ctx);
        ::EVP_PKEY_free(pkey);
    }
};

Ed25519Signer::Ed25519Signer(std::unique_ptr<State> state) : state_{std::move(state)} {
    state_->md_ctx = ::EVP_MD_CTX_new();
    if (state_->md_ctx == nullptr) detail::throw_openssl_error("Ed25519Signer: EVP_MD_CTX_new failed");
}

Ed25519Signer Ed25519Signer::from_pem(std::string_view pem) {
    if (pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        QUARK_THROW(QuarkError{"Ed25519Signer: PEM too large"});
    }
    BIO* bio = ::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) detail::throw_openssl_error("Ed25519Signer: BIO_new_mem_buf failed");
    auto state = std::make_unique<State>();
    state->pkey = ::PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    ::BIO_free(bio);
    if (state->pkey == nullptr) detail::throw_openssl_error("Ed25519Signer: cannot parse PEM private key");
    if (::EVP_PKEY_get_id(state->pkey) != EVP_PKEY_ED25519) {
        QUARK_THROW(QuarkError{"Ed25519Signer: PEM key is not an Ed25519 key"});
    }
    return Ed25519Signer{std::move(state)};
}

Ed25519Signer Ed25519Signer::from_private_key(std::span<const uint8_t, kKeySize> key) {
    auto state = std::make_unique<State>();
    state->pkey = ::EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size());
    if (state->pkey == nullptr) detail::throw_openssl_error("Ed25519Signer: invalid private key");
    return Ed25519Signer{std::move(state)};
}

Ed25519Signer::~Ed25519Signer() = default;
Ed25519Signer::Ed25519Signer(Ed25519Signer&&) noexcept = default;
Ed25519Signer& Ed25519Signer::operator=(Ed25519Signer&&) noexcept = default;

Ed25519Signer::Signature Ed25519Signer::sign(std::string_view message) {
    Signature sig;
    std::size_t len = sig.size();
    // Ed25519 is one-shot (no digest): re-initialize the context for every message
    if (::EVP_MD_CTX_reset(state_->md_ctx) != 1 ||
        ::EVP_DigestSignInit(state_->md_ctx, nullptr, nullptr, nullptr, state_->pkey) != 1 ||
        ::EVP_DigestSign(state_->md_ctx, sig.data(), &len, reinterpret_cast<const unsigned char*>(message.data()),
                         message.size()) != 1 ||
        len != kSignatureSize) {
        detail::throw_openssl_error("Ed25519Signer: sign failed");
    }
    return sig;
}

std::size_t Ed25519Signer::sign_base64(std::string_view message, std::span<char> out) {
    if (out.size() < base64_encoded_size(kSignatureSize)) return 0;
    return base64_encode(sign(message), out);
}

Ed25519Signer::PublicKey Ed25519Signer::public_key() const {
    PublicKey pub;
    std::size_t len = pub.size();
    if (::EVP_PKEY_get_raw_public_key(state_->pkey, pub.data(), &len) != 1 || len != kKeySize) {
        detail::throw_openssl_error("Ed25519Signer: cannot read public key");
    }
    return pub;
}

} // namespace crypto

QUARK_END_NAMESPACE
