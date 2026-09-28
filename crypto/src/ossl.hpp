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

/// What a chain is being built for: a trusted list's qualified CAs anchor
/// signers, its qualified TSAs anchor timestamps, and neither the other.
enum class Purpose { Signature, Timestamp };

/// Builds an X509_STORE from the store's certificates, plus the trusted
/// list's services of the kind `purpose` needs that were granted at `when`.
StorePtr make_store(const TrustStore& trust, Purpose purpose = Purpose::Signature,
                    std::int64_t when = 0);

struct TimestampToken {
    Bytes der;              ///< the TimeStampToken (a CMS SignedData)
    std::int64_t time = 0;  ///< its genTime
};

/// SHA-256 of `n` bytes.
Bytes sha256(const unsigned char* p, std::size_t n);

/// RFC 3161: asks the TSA at `url` to timestamp `imprint`, a SHA-256 digest
/// (with a random nonce, the TSA certificate requested). Checks the reply's
/// status, imprint and nonce, and the token's signature against the
/// certificate it carries. Whether that TSA is trusted is decided at
/// verification, like any other signer. Throws leht::Error on any failure.
TimestampToken request_timestamp(const std::string& url, const Bytes& imprint,
                                 int timeout_seconds);

struct HttpRequest {
    std::string url;
    std::string what;                  ///< "the timestamp authority", for messages
    const Bytes* post = nullptr;       ///< the body to POST; null means GET
    const char* content_type = nullptr;
    const char* expected_type = nullptr;  ///< null: any
    bool expect_asn1 = false;
    std::size_t max_size = std::size_t{1} << 20;
    int timeout_seconds = 20;
    /// Redirects to follow (http or https only). Zero for anything whose
    /// address a document supplied: OCSP, CRLs.
    int max_redirects = 0;
};

/// One HTTP(S) exchange (http.cpp). Throws leht::Error when the URL is not
/// http(s), the server does not answer, or the reply is empty or too large.
Bytes http_transfer(const HttpRequest& request);

struct HttpReply {
    int status = 0;  ///< 200, 404, ...
    Bytes body;      ///< may be empty
};

/// One HTTP/1.1 exchange, for services that refuse HTTP/1.0 (which is all
/// OpenSSL's client speaks) and whose error statuses mean something: SK's
/// Smart-ID and Mobile-ID. Connection: close, no redirects; Content-Length or
/// chunked replies. `expected_type`, `expect_asn1` and `max_redirects` are
/// not used. Throws leht::Error when there is no reply at all.
HttpReply http11(const HttpRequest& request);

/// The host part of a URL; empty when it does not parse.
std::string url_host(const std::string& url);

/// Builds a chain for `leaf` against `trust` at `when`, with `untrusted` as
/// extra intermediates (verify.cpp). `chain` gets it signer first -- partial
/// when no chain to a trust anchor could be built -- and `detail` OpenSSL's
/// reason when the result is not Trusted.
Trust evaluate_chain(X509* leaf, STACK_OF(X509)* untrusted, const TrustStore& trust,
                     std::int64_t when, X509StackPtr* chain, std::string* detail,
                     Purpose purpose = Purpose::Signature);

/// The trusted lists' verdict on `signer` (whose chain is `chain`, signer
/// first) at `when` (qualified.cpp).
QualifiedReport qualify_signer(X509* signer, STACK_OF(X509)* chain, const TrustStore& trust,
                               std::int64_t when);
/// The same for a timestamp authority's certificate at the token's time.
QualifiedReport qualify_timestamp(X509* tsa, const TrustStore& trust, std::int64_t when);

/// Each certificate of `chain` (signer first) but a self-signed last one,
/// checked for revocation at `when` against `embedded` and `online`
/// (revocation.cpp). Empty when both are empty.
std::vector<RevocationCheck> check_revocation(STACK_OF(X509)* chain, std::int64_t when,
                                              const RevocationData& embedded,
                                              const RevocationData& online);

/// The certificates of `data` parsed, those that do not parse skipped, pushed
/// onto `to` (which owns them).
void add_certs(const RevocationData& data, STACK_OF(X509)* to);

/// A logged-in session on a PKCS#11 token, holding one private key. Defined
/// in pkcs11.cpp; the rest of leht::crypto only asks it to sign.
struct Token;

/// Signs `input` on the token: the hash itself for an EC key (CKM_ECDSA),
/// a DER DigestInfo for an RSA key (CKM_RSA_PKCS). Returns what the card
/// returns -- raw r||s for ECDSA. Logs in again first when the key demands it
/// per signature. Throws leht::Error.
Bytes token_sign(Token& token, const Bytes& input);

/// A key that signs somewhere else and is reached over the network -- a
/// phone, through SK's Smart-ID or Mobile-ID. Defined in sk.cpp.
struct Remote {
    Remote() = default;
    Remote(const Remote&) = delete;
    Remote& operator=(const Remote&) = delete;
    virtual ~Remote() = default;
    /// The digest to sign with: for the document, the signed attributes and
    /// the signature algorithm alike.
    [[nodiscard]] virtual const EVP_MD* digest() const = 0;
    /// RSASSA-PSS (MGF1 with the same digest, the salt as long as the digest)
    /// rather than PKCS#1 v1.5, for an RSA key.
    [[nodiscard]] virtual bool pss() const { return false; }
    /// Signs `hash`, the digest of the signed attributes. Returns the value:
    /// for ECDSA either DER or r||s. Throws leht::Error or Cancelled.
    virtual Bytes sign(const Bytes& hash) = 0;
};

/// Base64, standard alphabet with padding; and back, throwing leht::Error on
/// anything else.
std::string base64(const Bytes& data);
Bytes unbase64(const std::string& text);

}  // namespace leht::crypto::detail

namespace leht::crypto {

struct Identity::Impl {
    /// The private key for PKCS#12. For a token key, only the certificate's
    /// public key: the private one never leaves the card, and `token` signs.
    detail::PkeyPtr key;
    detail::X509Ptr cert;
    std::vector<detail::X509Ptr> extra;  ///< the rest of the chain, as the file had it
    std::shared_ptr<detail::Token> token;  ///< set when the key is on a PKCS#11 token
    std::shared_ptr<detail::Remote> remote;  ///< set when a phone signs
};

struct TrustStore::Impl {
    std::vector<detail::X509Ptr> certs;
    /// The trusted list, and each of its services' certificates parsed once.
    std::shared_ptr<const trustlist::TrustedList> list;
    struct ListCert {
        const trustlist::Service* service;
        detail::X509Ptr cert;
    };
    std::vector<ListCert> list_certs;
};

}  // namespace leht::crypto
