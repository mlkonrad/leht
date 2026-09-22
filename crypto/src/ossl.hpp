// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal header: OpenSSL ownership and the small conversions every part of
// leht::crypto needs. Nothing outside crypto/src includes it.
#pragma once

#include "leht/crypto/crypto.hpp"

#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <memory>
#include <string>
#include <vector>

namespace leht::crypto::detail {

template <auto Free>
struct Deleter {
    template <typename T>
    void operator()(T* p) const noexcept {
        Free(p);
    }
};
template <typename T, auto Free>
using Ptr = std::unique_ptr<T, Deleter<Free>>;

using X509Ptr = Ptr<X509, X509_free>;
using PkeyPtr = Ptr<EVP_PKEY, EVP_PKEY_free>;
using BioPtr = Ptr<BIO, BIO_free_all>;
using CmsPtr = Ptr<CMS_ContentInfo, CMS_ContentInfo_free>;
using MdCtxPtr = Ptr<EVP_MD_CTX, EVP_MD_CTX_free>;
using StorePtr = Ptr<X509_STORE, X509_STORE_free>;
using StoreCtxPtr = Ptr<X509_STORE_CTX, X509_STORE_CTX_free>;

inline void free_x509_stack(STACK_OF(X509)* s) noexcept { sk_X509_pop_free(s, X509_free); }
using X509StackPtr = Ptr<STACK_OF(X509), free_x509_stack>;

/// OpenSSL's error queue as one line, emptied. Empty when there was nothing.
std::string drain_errors();

/// Throws leht::Error(what + OpenSSL's reasons).
[[noreturn]] void fail(const std::string& what);

X509Ptr up_ref(X509* cert);
Bytes to_der(X509* cert);
X509Ptr from_der(const Bytes& der);
CertInfo info(X509* cert);
std::string hex(const unsigned char* p, std::size_t n);
/// Unix seconds, or 0 when the time cannot be read.
std::int64_t to_unix(const ASN1_TIME* t);
/// "SHA-256" for an EVP_MD.
std::string digest_name(const EVP_MD* md);

/// Builds an X509_STORE from the store's certificates.
StorePtr make_store(const TrustStore& trust);

struct TimestampToken {
    Bytes der;              ///< the TimeStampToken (a CMS SignedData)
    std::int64_t time = 0;  ///< its genTime
};

/// RFC 3161: asks the TSA at `url` to timestamp `data` (SHA-256 imprint, a
/// random nonce, the TSA certificate requested). Checks the reply's status,
/// imprint and nonce, and the token's signature against the certificate it
/// carries. Whether that TSA is trusted is decided at verification, like any
/// other signer. Throws leht::Error on any failure.
TimestampToken request_timestamp(const std::string& url, const unsigned char* data,
                                 std::size_t size, int timeout_seconds);

}  // namespace leht::crypto::detail

namespace leht::crypto {

struct Identity::Impl {
    detail::PkeyPtr key;
    detail::X509Ptr cert;
    std::vector<detail::X509Ptr> extra;  ///< the rest of the chain, as the file had it
};

struct TrustStore::Impl {
    std::vector<detail::X509Ptr> certs;
};

}  // namespace leht::crypto
