// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Verifying a detached CMS signature. The DER comes out of a document, so it is
// attacker-controlled: nothing here throws on bad input, and every failure is
// a field of the report. The viewer runs this inside the sandboxed worker.
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/err.h>
#include <openssl/ess.h>
#include <openssl/pkcs7.h>
#include <openssl/ts.h>
#include <openssl/x509v3.h>

#include <array>
#include <functional>
#include <cstring>
#include <ctime>

namespace leht::crypto {

namespace {

using detail::X509Ptr;

/// Largest signature blob looked at. A /Contents hole is rarely over 64 KB;
/// anything far larger is not a signature anyone meant to verify.
constexpr std::size_t kMaxDer = std::size_t{4} << 20;

Trust trust_from(int err) {
    switch (err) {
        case X509_V_OK: return Trust::Trusted;
        case X509_V_ERR_CERT_HAS_EXPIRED: return Trust::Expired;
        case X509_V_ERR_CERT_NOT_YET_VALID: return Trust::NotYetValid;
        default: return Trust::Untrusted;
    }
}

std::vector<CertInfo> describe(STACK_OF(X509)* chain) {
    std::vector<CertInfo> out;
    for (int i = 0; chain != nullptr && i < sk_X509_num(chain); ++i) {
        out.push_back(detail::info(sk_X509_value(chain, i)));
    }
    return out;
}

/// The worst revocation answer about `checks` as a trust verdict: Revoked
/// when a certificate was revoked before the trusted time.
bool revoked(const std::vector<RevocationCheck>& checks, std::string* detail) {
    for (const RevocationCheck& c : checks) {
        if (c.status == RevocationStatus::Revoked) {
            *detail = "the certificate of " +
                      (c.cert.common_name.empty() ? c.cert.subject : c.cert.common_name) +
                      " was revoked (" + c.source + ")";
            return true;
        }
    }
    return false;
}

const EVP_MD* acceptable_digest(const X509_ALGOR* alg) {
    const ASN1_OBJECT* obj = nullptr;
    X509_ALGOR_get0(&obj, nullptr, nullptr, alg);
    switch (OBJ_obj2nid(obj)) {
        case NID_sha1: return EVP_sha1();  // reported as weak by the caller
        case NID_sha224: return EVP_sha224();
        case NID_sha256: return EVP_sha256();
        case NID_sha384: return EVP_sha384();
        case NID_sha512: return EVP_sha512();
        default: return nullptr;
    }
}

bool digest_content(const EVP_MD* md, const ContentReader& content, Bytes* out) {
    detail::MdCtxPtr ctx{EVP_MD_CTX_new()};
    if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) {
        return false;
    }
    std::array<std::uint8_t, 65536> buf{};
    for (;;) {
        const std::size_t n = content(buf.data(), buf.size());
        if (n == 0) {
            break;
        }
        if (EVP_DigestUpdate(ctx.get(), buf.data(), n) != 1) {
            return false;
        }
    }
    out->resize(static_cast<std::size_t>(EVP_MD_get_size(md)));
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), out->data(), &len) != 1) {
        return false;
    }
    out->resize(len);
    return true;
}

/// signing-certificate-v2 must name the signer's certificate by hash, or a
/// substituted certificate with the same key would pass.
bool signing_cert_matches(CMS_SignerInfo* si, X509* signer) {
    const int idx = CMS_signed_get_attr_by_NID(si, NID_id_smime_aa_signingCertificateV2, -1);
    if (idx < 0) {
        return false;
    }
    X509_ATTRIBUTE* attr = CMS_signed_get_attr(si, idx);
    const ASN1_TYPE* type = X509_ATTRIBUTE_get0_type(attr, 0);
    if (type == nullptr || type->type != V_ASN1_SEQUENCE) {
        return false;
    }
    const unsigned char* p = type->value.sequence->data;
    detail::Ptr<ESS_SIGNING_CERT_V2, ESS_SIGNING_CERT_V2_free> v2{d2i_ESS_SIGNING_CERT_V2(
        nullptr, &p, type->value.sequence->length)};
    if (!v2) {
        return false;
    }
    detail::X509StackPtr chain{sk_X509_new_null()};
    if (!chain || sk_X509_push(chain.get(), signer) <= 0) {
        return false;
    }
    X509_up_ref(signer);
    const int ok = OSSL_ESS_check_signing_certs(nullptr, v2.get(), chain.get(), 1);
    return ok > 0;
}

/// Computes the imprint a token must carry, with the token's own digest
/// algorithm; false when that cannot be done.
using ImprintOf = std::function<bool(const EVP_MD* md, Bytes* out)>;

/// An RFC 3161 token: its imprint against `expected`, its signature against
/// its own certificate, and that authority's trust (and revocation) at the
/// token's own time.
TimestampReport check_token(const unsigned char* der, long len, const ImprintOf& expected,
                            const TrustStore& trust, const RevocationData& embedded,
                            const RevocationData& online) {
    TimestampReport ts;
    detail::Ptr<PKCS7, PKCS7_free> token{d2i_PKCS7(nullptr, &der, len)};
    if (!token || PKCS7_type_is_signed(token.get()) == 0) {
        ts.problem = "the timestamp token is not a signed CMS";
        ERR_clear_error();
        return ts;
    }
    detail::Ptr<TS_TST_INFO, TS_TST_INFO_free> tst{PKCS7_to_TS_TST_INFO(token.get())};
    if (!tst) {
        ts.problem = "the timestamp token holds no TSTInfo";
        ERR_clear_error();
        return ts;
    }
    ts.time = detail::to_unix(TS_TST_INFO_get_time(tst.get()));

    TS_MSG_IMPRINT* imprint = TS_TST_INFO_get_msg_imprint(tst.get());
    const EVP_MD* md = acceptable_digest(TS_MSG_IMPRINT_get_algo(imprint));
    const ASN1_OCTET_STRING* got = TS_MSG_IMPRINT_get_msg(imprint);
    Bytes want;
    if (md == nullptr || !expected(md, &want) || got == nullptr ||
        static_cast<std::size_t>(ASN1_STRING_length(got)) != want.size() ||
        std::memcmp(ASN1_STRING_get0_data(got), want.data(), want.size()) != 0) {
        ts.problem = "the timestamp does not cover what it claims to";
        ERR_clear_error();
        return ts;
    }

    // The token's signature, against its own certificate.
    detail::StorePtr empty{X509_STORE_new()};
    if (PKCS7_verify(token.get(), nullptr, empty.get(), nullptr, nullptr, PKCS7_NOVERIFY) != 1) {
        ts.problem = "the timestamp token's signature does not verify";
        ERR_clear_error();
        return ts;
    }
    // get0: the stack is ours, the certificates in it are the token's.
    struct StackFree {
        void operator()(STACK_OF(X509)* s) const noexcept { sk_X509_free(s); }
    };
    const std::unique_ptr<STACK_OF(X509), StackFree> signers{
        PKCS7_get0_signers(token.get(), nullptr, 0)};
    if (!signers || sk_X509_num(signers.get()) != 1) {
        ts.problem = "the timestamp token has no single signer";
        ERR_clear_error();
        return ts;
    }
    ts.valid = true;
    X509* tsa = sk_X509_value(signers.get(), 0);
    ts.authority = detail::info(tsa);

    detail::X509StackPtr untrusted{sk_X509_new_null()};
    STACK_OF(X509)* carried = token->d.sign->cert;
    for (int i = 0; untrusted && carried != nullptr && i < sk_X509_num(carried); ++i) {
        X509* c = sk_X509_value(carried, i);
        X509_up_ref(c);
        sk_X509_push(untrusted.get(), c);
    }
    detail::add_certs(embedded, untrusted.get());
    detail::add_certs(online, untrusted.get());
    detail::X509StackPtr chain;
    std::string why;
    ts.trust = detail::evaluate_chain(tsa, untrusted.get(), trust, ts.time, &chain, &why,
                                      detail::Purpose::Timestamp);
    ts.qualified = detail::qualify_timestamp(tsa, trust, ts.time);
    ts.revocation = detail::check_revocation(chain.get(), ts.time, embedded, online);
    if (ts.trust == Trust::Trusted && revoked(ts.revocation, &why)) {
        ts.trust = Trust::Revoked;
    }
    if (ts.trust != Trust::Trusted) {
        ts.problem = "timestamp authority: " + why;
    }
    ERR_clear_error();
    return ts;
}

TimestampReport check_timestamp(CMS_SignerInfo* si, const TrustStore& trust,
                                const RevocationData& embedded, const RevocationData& online) {
    const int idx = CMS_unsigned_get_attr_by_NID(si, NID_id_smime_aa_timeStampToken, -1);
    X509_ATTRIBUTE* attr = CMS_unsigned_get_attr(si, idx);
    const ASN1_TYPE* type = X509_ATTRIBUTE_get0_type(attr, 0);
    if (type == nullptr || type->type != V_ASN1_SEQUENCE) {
        TimestampReport ts;
        ts.problem = "the timestamp attribute is malformed";
        return ts;
    }
    // B-T: the imprint is the hash of this signature's value.
    const ASN1_OCTET_STRING* value = CMS_SignerInfo_get0_signature(si);
    const ImprintOf of_value = [value](const EVP_MD* md, Bytes* out) {
        out->resize(EVP_MAX_MD_SIZE);
        unsigned int n = 0;
        if (EVP_Digest(ASN1_STRING_get0_data(value),
                       static_cast<std::size_t>(ASN1_STRING_length(value)), out->data(), &n, md,
                       nullptr) != 1) {
            return false;
        }
        out->resize(n);
        return true;
    };
    return check_token(type->value.sequence->data, type->value.sequence->length, of_value, trust,
                       embedded, online);
}

}  // namespace

CmsReport verify_cms(const Bytes& der, const ContentReader& content, const TrustStore& trust,
                     std::int64_t now, const RevocationData& embedded,
                     const RevocationData& online) {
    init(false);
    CmsReport r;
    const auto give_up = [&](const char* why) {
        r.problem = why;
        ERR_clear_error();
        return r;
    };
    if (der.empty() || der.size() > kMaxDer) {
        return give_up("the signature is empty or implausibly large");
    }
    const unsigned char* p = der.data();
    detail::CmsPtr cms{d2i_CMS_ContentInfo(nullptr, &p, static_cast<long>(der.size()))};
    if (!cms) {
        return give_up("the signature is not a CMS structure");
    }
    if (OBJ_obj2nid(CMS_get0_type(cms.get())) != NID_pkcs7_signed) {
        return give_up("the signature is not CMS SignedData");
    }
    STACK_OF(CMS_SignerInfo)* infos = CMS_get0_SignerInfos(cms.get());
    if (infos == nullptr || sk_CMS_SignerInfo_num(infos) != 1) {
        return give_up("the signature does not have exactly one signer");
    }
    CMS_SignerInfo* si = sk_CMS_SignerInfo_value(infos, 0);
    if (CMS_signed_get_attr_count(si) <= 0) {
        return give_up("signatures without signed attributes are not supported");
    }

    // Match the signer to a certificate in the blob.
    detail::X509StackPtr certs{CMS_get1_certs(cms.get())};
    if (!certs || CMS_set1_signers_certs(cms.get(), nullptr, 0) < 1) {
        return give_up("the signature does not include the signer's certificate");
    }
    EVP_PKEY* pk = nullptr;
    X509* signer = nullptr;
    X509_ALGOR* dig = nullptr;
    X509_ALGOR* sig = nullptr;
    CMS_SignerInfo_get0_algs(si, &pk, &signer, &dig, &sig);
    if (signer == nullptr) {
        return give_up("the signature does not include the signer's certificate");
    }
    r.parsed = true;
    r.signer = detail::info(signer);

    const EVP_MD* md = acceptable_digest(dig);
    if (md == nullptr) {
        r.problem = "the signature uses an unsupported digest algorithm";
        ERR_clear_error();
        return r;
    }
    r.digest = detail::digest_name(md);

    // 1. The content: our digest of the signed ranges against the one signed.
    Bytes digest;
    if (!digest_content(md, content, &digest)) {
        r.problem = "cannot digest the signed bytes";
        ERR_clear_error();
        return r;
    }
    const auto* md_attr = static_cast<const ASN1_OCTET_STRING*>(CMS_signed_get0_data_by_OBJ(
        si, OBJ_nid2obj(NID_pkcs9_messageDigest), -3, V_ASN1_OCTET_STRING));
    r.digest_matches = md_attr != nullptr &&
                       static_cast<std::size_t>(ASN1_STRING_length(md_attr)) == digest.size() &&
                       std::memcmp(ASN1_STRING_get0_data(md_attr), digest.data(), digest.size()) == 0;

    // 2. The signature over the signed attributes, by the signer's key.
    r.signature_valid = CMS_SignerInfo_verify(si) == 1;
    ERR_clear_error();

    r.has_signing_time_attribute = CMS_signed_get_attr_by_NID(si, NID_pkcs9_signingTime, -1) >= 0;
    r.has_signing_certificate_v2 = signing_cert_matches(si, signer);
    ERR_clear_error();

    if (!r.intact()) {
        r.problem = !r.digest_matches ? "the document bytes do not match the signature"
                                      : "the signature value does not verify";
        return r;
    }
    if (md == EVP_sha1()) {
        r.problem = "the signature uses SHA-1, which no longer protects against forgery";
    }

    // 3. Trust, at the timestamp's time if a valid one says when the signature
    //    existed; otherwise now. /M is only the signer's claim.
    if (CMS_unsigned_get_attr_by_NID(si, NID_id_smime_aa_timeStampToken, -1) >= 0) {
        r.timestamp = check_timestamp(si, trust, embedded, online);
    }
    std::int64_t when = now != 0 ? now : static_cast<std::int64_t>(std::time(nullptr));
    if (r.timestamp && r.timestamp->valid) {
        when = r.timestamp->time;
    }
    // The /DSS certificates help: an intermediate the signature left out.
    detail::add_certs(embedded, certs.get());
    detail::add_certs(online, certs.get());
    detail::X509StackPtr chain;
    r.trust = detail::evaluate_chain(signer, certs.get(), trust, when, &chain, &r.trust_detail);
    r.chain = describe(chain.get());
    if (r.chain.empty()) {
        r.chain.push_back(r.signer);
    }
    if (r.trust == Trust::Trusted && !r.signer.can_sign) {
        r.trust = Trust::Untrusted;
        r.trust_detail = "the certificate's key usage does not allow signing";
    }
    // 4. Revocation, at that same time: revoked afterwards does not matter.
    r.revocation = detail::check_revocation(chain.get(), when, embedded, online);
    if (r.trust == Trust::Trusted && revoked(r.revocation, &r.trust_detail)) {
        r.trust = Trust::Revoked;
    }
    // 5. Qualified, by the trusted lists -- for a signature that is trusted
    //    and not revoked; anything less cannot be a qualified one.
    if (trust.trusted_list() != nullptr) {
        if (r.trust == Trust::Trusted) {
            r.qualified = detail::qualify_signer(signer, chain.get(), trust, when);
        } else {
            r.qualified.level = QualifiedReport::Level::NotQualified;
            r.qualified.detail = "the signature is not trusted";
        }
    }
    ERR_clear_error();
    return r;
}

TimestampReport verify_document_timestamp(const Bytes& der, const ContentReader& content,
                                          const TrustStore& trust, const RevocationData& embedded,
                                          const RevocationData& online) {
    init(false);
    if (der.empty() || der.size() > kMaxDer) {
        TimestampReport ts;
        ts.problem = "the timestamp is empty or implausibly large";
        return ts;
    }
    // The imprint is the digest of the byte ranges, with the token's own
    // algorithm: read once, whatever it is.
    bool read = false;
    const ImprintOf of_ranges = [&](const EVP_MD* md, Bytes* out) {
        if (read) {
            return false;
        }
        read = true;
        return digest_content(md, content, out);
    };
    return check_token(der.data(), static_cast<long>(der.size()), of_ranges, trust, embedded,
                       online);
}

namespace detail {

Trust evaluate_chain(X509* leaf, STACK_OF(X509)* untrusted, const TrustStore& trust,
                     std::int64_t when, X509StackPtr* chain, std::string* detail,
                     Purpose purpose) {
    StorePtr store = make_store(trust, purpose, when);
    StoreCtxPtr ctx{X509_STORE_CTX_new()};
    if (!ctx || X509_STORE_CTX_init(ctx.get(), store.get(), leaf, untrusted) != 1) {
        *detail = "cannot evaluate the certificate chain";
        ERR_clear_error();
        return Trust::Unknown;
    }
    X509_STORE_CTX_set_time(ctx.get(), 0, static_cast<time_t>(when));
    const int ok = X509_verify_cert(ctx.get());
    const int err = ok == 1 ? X509_V_OK : X509_STORE_CTX_get_error(ctx.get());
    if (chain != nullptr) {
        // get1: ours to free. Partial when no chain to an anchor was found.
        chain->reset(X509_STORE_CTX_get1_chain(ctx.get()));
        if (!*chain) {
            chain->reset(sk_X509_new_null());
            if (*chain) {
                X509_up_ref(leaf);
                sk_X509_push(chain->get(), leaf);
            }
        }
    }
    if (err != X509_V_OK) {
        *detail = X509_verify_cert_error_string(err);
    }
    ERR_clear_error();
    return trust_from(err);
}

void add_certs(const RevocationData& data, STACK_OF(X509)* to) {
    if (to == nullptr) {
        return;
    }
    for (const Bytes& der : data.certs) {
        const unsigned char* p = der.data();
        X509* c = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
        if (c != nullptr && sk_X509_push(to, c) <= 0) {
            X509_free(c);
        }
    }
    ERR_clear_error();
}

}  // namespace detail

}  // namespace leht::crypto
