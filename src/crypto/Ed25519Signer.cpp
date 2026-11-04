#include "quark/crypto/Ed25519Signer.h"

#include "OpenSslError.h"
#include "TlsBackend.h"
#include "quark/crypto/Base64Encoding.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cassert>
#include <limits>
#include <memory>
#include <utility>

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
    // a passphrase callback that refuses: the library's default one prompts on the terminal (and blocks)
    // for an encrypted key
    pem_password_cb* no_passphrase = [](char*, int, int, void*) -> int { return 0; };
    state->pkey = ::PEM_read_bio_PrivateKey(bio, nullptr, no_passphrase, nullptr);
    ::BIO_free(bio);
    if (state->pkey == nullptr) detail::throw_openssl_error("Ed25519Signer: cannot parse PEM private key");
    if (::EVP_PKEY_id(state->pkey) != EVP_PKEY_ED25519) {
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

std::optional<Ed25519Signer::Signature> Ed25519Signer::try_sign(std::string_view message) noexcept {
    assert(state_ && "Ed25519Signer: moved from");
    Signature sig;
    std::size_t len = sig.size();
    // Ed25519 is one-shot (no digest): re-initialize the context for every message
    if (::EVP_MD_CTX_reset(state_->md_ctx) != 1 ||
        ::EVP_DigestSignInit(state_->md_ctx, nullptr, nullptr, nullptr, state_->pkey) != 1 ||
        ::EVP_DigestSign(state_->md_ctx, sig.data(), &len, reinterpret_cast<const unsigned char*>(message.data()),
                         message.size()) != 1 ||
        len != kSignatureSize) [[unlikely]] {
        return std::nullopt;  // the reason stays in the error queue for sign() to report
    }
    return sig;
}

Ed25519Signer::Signature Ed25519Signer::sign(std::string_view message) {
    if (auto sig = try_sign(message)) [[likely]] {
        return *sig;
    }
    detail::throw_openssl_error("Ed25519Signer: sign failed");
}

std::size_t Ed25519Signer::sign_base64(std::string_view message, std::span<char> out) noexcept {
    if (out.size() < base64_encoded_size(kSignatureSize)) return 0;
    const auto sig = try_sign(message);
    if (!sig) [[unlikely]] {
        ::ERR_clear_error();
        return 0;
    }
    return base64_encode(*sig, out);
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
