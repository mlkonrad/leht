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

/// Builds a chain for `leaf` against `trust` at `when`, with `untrusted` as the
/// intermediates the signature carried.
Trust evaluate(X509* leaf, STACK_OF(X509)* untrusted, const TrustStore& trust, std::int64_t when,
               std::vector<CertInfo>* chain, std::string* detail) {
    detail::StorePtr store = detail::make_store(trust);
    detail::StoreCtxPtr ctx{X509_STORE_CTX_new()};
    if (!ctx || X509_STORE_CTX_init(ctx.get(), store.get(), leaf, untrusted) != 1) {
        *detail = "cannot evaluate the certificate chain";
        ERR_clear_error();
        return Trust::Unknown;
    }
    X509_STORE_CTX_set_time(ctx.get(), 0, static_cast<time_t>(when));
    const int ok = X509_verify_cert(ctx.get());
    const int err = ok == 1 ? X509_V_OK : X509_STORE_CTX_get_error(ctx.get());
    if (chain != nullptr) {
        chain->clear();
        STACK_OF(X509)* built = X509_STORE_CTX_get0_chain(ctx.get());
        for (int i = 0; built != nullptr && i < sk_X509_num(built); ++i) {
            chain->push_back(detail::info(sk_X509_value(built, i)));
        }
        if (chain->empty()) {
            chain->push_back(detail::info(leaf));
        }
    }
    if (err != X509_V_OK) {
        *detail = X509_verify_cert_error_string(err);
    }
    ERR_clear_error();
    return trust_from(err);
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

TimestampReport check_timestamp(CMS_SignerInfo* si, const TrustStore& trust) {
    TimestampReport ts;
    const int idx = CMS_unsigned_get_attr_by_NID(si, NID_id_smime_aa_timeStampToken, -1);
    X509_ATTRIBUTE* attr = CMS_unsigned_get_attr(si, idx);
    const ASN1_TYPE* type = X509_ATTRIBUTE_get0_type(attr, 0);
    if (type == nullptr || type->type != V_ASN1_SEQUENCE) {
        ts.problem = "the timestamp attribute is malformed";
        return ts;
    }
    const unsigned char* p = type->value.sequence->data;
    detail::Ptr<PKCS7, PKCS7_free> token{
        d2i_PKCS7(nullptr, &p, type->value.sequence->length)};
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

    // The imprint must be the hash of this signature's value.
    TS_MSG_IMPRINT* imprint = TS_TST_INFO_get_msg_imprint(tst.get());
    const EVP_MD* md = acceptable_digest(TS_MSG_IMPRINT_get_algo(imprint));
    const ASN1_OCTET_STRING* value = CMS_SignerInfo_get0_signature(si);
    const ASN1_OCTET_STRING* got = TS_MSG_IMPRINT_get_msg(imprint);
    std::array<unsigned char, EVP_MAX_MD_SIZE> want{};
    unsigned int want_len = 0;
    if (md == nullptr ||
        EVP_Digest(ASN1_STRING_get0_data(value), static_cast<std::size_t>(ASN1_STRING_length(value)),
                   want.data(), &want_len, md, nullptr) != 1 ||
        got == nullptr || static_cast<unsigned>(ASN1_STRING_length(got)) != want_len ||
        std::memcmp(ASN1_STRING_get0_data(got), want.data(), want_len) != 0) {
        ts.problem = "the timestamp does not cover this signature";
        ERR_clear_error();
        return ts;
    }

    // The token's signature, against its own certificate.
    detail::StorePtr empty{X509_STORE_new()};
    STACK_OF(X509)* signers = nullptr;
    if (PKCS7_verify(token.get(), nullptr, empty.get(), nullptr, nullptr, PKCS7_NOVERIFY) != 1) {
        ts.problem = "the timestamp token's signature does not verify";
        ERR_clear_error();
        return ts;
    }
    signers = PKCS7_get0_signers(token.get(), nullptr, 0);
    // get0: the stack is ours, the certificates in it are the token's.
    struct StackFree {
        void operator()(STACK_OF(X509)* s) const noexcept { sk_X509_free(s); }
    };
    const std::unique_ptr<STACK_OF(X509), StackFree> owned_signers{signers};
    if (signers == nullptr || sk_X509_num(signers) != 1) {
        ts.problem = "the timestamp token has no single signer";
        ERR_clear_error();
        return ts;
    }
    ts.valid = true;
    X509* tsa = sk_X509_value(signers, 0);
    ts.authority = detail::info(tsa);
    std::string why;
    ts.trust = evaluate(tsa, token->d.sign->cert, trust, ts.time, nullptr, &why);
    if (ts.trust != Trust::Trusted) {
        ts.problem = "timestamp authority: " + why;
    }
    ERR_clear_error();
    return ts;
}

}  // namespace

CmsReport verify_cms(const Bytes& der, const ContentReader& content, const TrustStore& trust,
                     std::int64_t now) {
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
        r.timestamp = check_timestamp(si, trust);
    }
    std::int64_t when = now != 0 ? now : static_cast<std::int64_t>(std::time(nullptr));
    if (r.timestamp && r.timestamp->valid) {
        when = r.timestamp->time;
    }
    r.trust = evaluate(signer, certs.get(), trust, when, &r.chain, &r.trust_detail);
    if (r.trust == Trust::Trusted && !r.signer.can_sign) {
        r.trust = Trust::Untrusted;
        r.trust_detail = "the certificate's key usage does not allow signing";
    }
    ERR_clear_error();
    return r;
}

}  // namespace leht::crypto
