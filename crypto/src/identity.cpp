// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/err.h>
#include <openssl/pkcs12.h>

namespace leht::crypto {

Identity::Identity(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Identity::Identity(Identity&&) noexcept = default;
Identity& Identity::operator=(Identity&&) noexcept = default;
Identity::~Identity() = default;

Identity Identity::from_pkcs12(const Bytes& p12, const Secret& password) {
    init();
    const unsigned char* p = p12.data();
    detail::Ptr<PKCS12, PKCS12_free> bundle{
        d2i_PKCS12(nullptr, &p, static_cast<long>(p12.size()))};
    if (!bundle) {
        detail::fail("not a PKCS#12 file");
    }
    if (PKCS12_verify_mac(bundle.get(), password.str().c_str(),
                          static_cast<int>(password.str().size())) != 1) {
        ERR_clear_error();
        throw Error(0, "wrong password for the PKCS#12 file");
    }

    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* ca = nullptr;
    if (PKCS12_parse(bundle.get(), password.str().c_str(), &key, &cert, &ca) != 1) {
        const unsigned long e = ERR_peek_last_error();
        if (ERR_GET_REASON(e) == ERR_R_UNSUPPORTED) {
            ERR_clear_error();
            throw Error(0, "the PKCS#12 file uses encryption only OpenSSL's legacy provider "
                           "reads (an old RC2/3DES export); re-export it with AES");
        }
        detail::fail("cannot read the PKCS#12 file");
    }
    auto impl = std::make_unique<Impl>();
    impl->key.reset(key);
    impl->cert.reset(cert);
    const detail::X509StackPtr others{ca};
    if (!impl->key) {
        throw Error(0, "the PKCS#12 file holds no private key");
    }
    if (!impl->cert) {
        throw Error(0, "the PKCS#12 file holds no certificate for its key");
    }
    if (X509_check_private_key(impl->cert.get(), impl->key.get()) != 1) {
        ERR_clear_error();
        throw Error(0, "the PKCS#12 file's certificate does not match its key");
    }
    for (int i = 0; others && i < sk_X509_num(others.get()); ++i) {
        impl->extra.push_back(detail::up_ref(sk_X509_value(others.get(), i)));
    }
    return Identity{std::move(impl)};
}

CertInfo Identity::certificate() const { return detail::info(impl_->cert.get()); }

std::vector<Bytes> Identity::chain_der() const {
    std::vector<Bytes> out{detail::to_der(impl_->cert.get())};
    for (const detail::X509Ptr& c : impl_->extra) {
        out.push_back(detail::to_der(c.get()));
    }
    return out;
}

KeyType Identity::key_type() const {
    switch (EVP_PKEY_get_base_id(impl_->key.get())) {
        case EVP_PKEY_RSA:
        case EVP_PKEY_RSA_PSS:
            return KeyType::Rsa;
        case EVP_PKEY_EC:
            return KeyType::Ec;
        default:
            return KeyType::Other;
    }
}

int Identity::key_bits() const { return EVP_PKEY_get_bits(impl_->key.get()); }

}  // namespace leht::crypto
