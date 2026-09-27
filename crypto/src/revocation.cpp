// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Revocation: what to ask (OCSP requests, CRL locations), fetching it, and
// judging the answers. The asking and judging parse certificates and responses
// that come out of a document, so the viewer runs them in the sandboxed
// worker; fetching needs the network, so it runs in the trusted process and
// only moves bytes.
//
// The time that matters is the trusted time: the signature's timestamp when it
// has a valid one. A certificate revoked after it does not undo the signature
// -- that is the whole point of long-term validation.
//
// Freshness: an OCSP response or a CRL speaks for the trusted time when it
// was issued at or after it, or when the trusted time lies within its
// thisUpdate..nextUpdate window. The second case matters in practice: many
// responders pre-compute their answers, so one fetched right after signing is
// often a little older than the signature.
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <ctime>
#include <optional>
#include <set>

namespace leht::crypto {

namespace {

using OcspRespPtr = detail::Ptr<OCSP_RESPONSE, OCSP_RESPONSE_free>;
using OcspBasicPtr = detail::Ptr<OCSP_BASICRESP, OCSP_BASICRESP_free>;
using OcspCertIdPtr = detail::Ptr<OCSP_CERTID, OCSP_CERTID_free>;
using OcspReqPtr = detail::Ptr<OCSP_REQUEST, OCSP_REQUEST_free>;
using CrlPtr = detail::Ptr<X509_CRL, X509_CRL_free>;
using detail::X509Ptr;
using detail::X509StackPtr;

/// Largest single OCSP response or CRL looked at. A CRL can be large; one
/// over this is not worth parsing inside a verification.
constexpr std::size_t kMaxOcsp = std::size_t{1} << 20;
constexpr std::size_t kMaxCrl = std::size_t{16} << 20;
/// Most chain certificates a document's signatures are checked for.
constexpr std::size_t kMaxQueries = 64;

std::string display_name(X509* cert) {
    const CertInfo i = detail::info(cert);
    return i.common_name.empty() ? i.subject : i.common_name;
}

bool self_signed(X509* cert) {
    return X509_check_issued(cert, cert) == X509_V_OK;
}

/// Whether data issued at `this_update`, valid until `next_update` (0: no
/// next update), speaks for `when`. See the freshness note above.
bool fresh(std::int64_t this_update, std::int64_t next_update, std::int64_t when) {
    if (this_update >= when) {
        return true;
    }
    return next_update != 0 && when <= next_update;
}

/// The OCSP response's signer, when it may speak for `issuer`'s certificates:
/// the issuer itself, or a certificate the issuer signed with the
/// id-kp-OCSPSigning purpose (a delegated responder), valid when the response
/// was produced. Null otherwise.
X509* responder(OCSP_BASICRESP* bs, X509* issuer, STACK_OF(X509)* pool, std::int64_t produced,
                std::string* why) {
    X509* signer = nullptr;
    if (OCSP_resp_get0_signer(bs, &signer, pool) != 1 || signer == nullptr) {
        *why = "the OCSP response's signer is not included";
        return nullptr;
    }
    // Only the signature itself here; who signed is judged below, at the
    // right time -- OpenSSL would check the responder's chain against now.
    detail::StorePtr empty{X509_STORE_new()};
    STACK_OF(X509)* just_signer = sk_X509_new_null();
    sk_X509_push(just_signer, signer);
    const int ok = OCSP_basic_verify(bs, just_signer, empty.get(), OCSP_NOVERIFY | OCSP_NOCHAIN);
    sk_X509_free(just_signer);
    if (ok != 1) {
        *why = "the OCSP response's signature does not verify";
        return nullptr;
    }
    if (X509_cmp(signer, issuer) == 0) {
        return signer;
    }
    EVP_PKEY* issuer_key = X509_get0_pubkey(issuer);
    const bool delegated = X509_check_issued(issuer, signer) == X509_V_OK && issuer_key != nullptr &&
                           X509_verify(signer, issuer_key) == 1 &&
                           (X509_get_extension_flags(signer) & EXFLAG_XKUSAGE) != 0 &&
                           (X509_get_extended_key_usage(signer) & XKU_OCSP_SIGN) != 0;
    if (!delegated) {
        *why = "the OCSP response is signed by someone the certificate's issuer did not "
               "authorise";
        return nullptr;
    }
    const CertInfo i = detail::info(signer);
    if (produced < i.not_before || produced > i.not_after) {
        *why = "the OCSP responder's certificate was not valid when it answered";
        return nullptr;
    }
    return signer;
}

struct Answer {
    RevocationStatus status = RevocationStatus::Unknown;
    std::int64_t data_time = 0;
    std::int64_t revoked_at = 0;
};

/// What one OCSP response says about `cert` at `when`; nullopt when it says
/// nothing about it. `why` explains a response that was about it but unusable.
std::optional<Answer> from_ocsp(const Bytes& der, X509* cert, X509* issuer, STACK_OF(X509)* pool,
                                std::int64_t when, std::string* why) {
    if (der.empty() || der.size() > kMaxOcsp) {
        return std::nullopt;
    }
    const unsigned char* p = der.data();
    OcspRespPtr resp{d2i_OCSP_RESPONSE(nullptr, &p, static_cast<long>(der.size()))};
    if (!resp || OCSP_response_status(resp.get()) != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
        return std::nullopt;
    }
    OcspBasicPtr bs{OCSP_response_get1_basic(resp.get())};
    if (!bs) {
        return std::nullopt;
    }
    // A CertID is hashed with some algorithm; responders use SHA-1 or SHA-256.
    int index = -1;
    for (const EVP_MD* md : {EVP_sha1(), EVP_sha256()}) {
        OcspCertIdPtr id{OCSP_cert_to_id(md, cert, issuer)};
        if (id && (index = OCSP_resp_find(bs.get(), id.get(), -1)) >= 0) {
            break;
        }
    }
    if (index < 0) {
        return std::nullopt;
    }
    const std::int64_t produced = detail::to_unix(OCSP_resp_get0_produced_at(bs.get()));
    if (responder(bs.get(), issuer, pool, produced, why) == nullptr) {
        return Answer{};
    }
    OCSP_SINGLERESP* single = OCSP_resp_get0(bs.get(), index);
    int reason = 0;
    ASN1_GENERALIZEDTIME* revtime = nullptr;
    ASN1_GENERALIZEDTIME* this_upd = nullptr;
    ASN1_GENERALIZEDTIME* next_upd = nullptr;
    const int status = OCSP_single_get0_status(single, &reason, &revtime, &this_upd, &next_upd);
    Answer a;
    a.data_time = detail::to_unix(this_upd);
    const std::int64_t next = detail::to_unix(next_upd);
    if (status == V_OCSP_CERTSTATUS_REVOKED) {
        a.revoked_at = detail::to_unix(revtime);
        // Revoked before the time that matters: final, however old the news.
        if (a.revoked_at <= when) {
            a.status = RevocationStatus::Revoked;
            return a;
        }
        // Revoked later: good at `when` if the response is recent enough to
        // say so, which one issued after the revocation is.
        a.status = RevocationStatus::Good;
        return a;
    }
    if (status != V_OCSP_CERTSTATUS_GOOD) {
        *why = "the OCSP responder does not know the certificate";
        return Answer{};
    }
    if (!fresh(a.data_time, next, when)) {
        *why = "the OCSP response is older than the signature";
        return Answer{};
    }
    a.status = RevocationStatus::Good;
    return a;
}

/// What one CRL says about `cert` at `when`; nullopt when it is not its issuer's.
std::optional<Answer> from_crl(const Bytes& der, X509* cert, X509* issuer, std::int64_t when,
                               std::string* why) {
    if (der.empty() || der.size() > kMaxCrl) {
        return std::nullopt;
    }
    const unsigned char* p = der.data();
    CrlPtr crl{d2i_X509_CRL(nullptr, &p, static_cast<long>(der.size()))};
    if (!crl || X509_NAME_cmp(X509_CRL_get_issuer(crl.get()), X509_get_subject_name(issuer)) != 0) {
        return std::nullopt;
    }
    EVP_PKEY* key = X509_get0_pubkey(issuer);
    if (key == nullptr || X509_CRL_verify(crl.get(), key) != 1) {
        *why = "a CRL naming the certificate's issuer is not signed by it";
        return Answer{};
    }
    Answer a;
    a.data_time = detail::to_unix(X509_CRL_get0_lastUpdate(crl.get()));
    const std::int64_t next = detail::to_unix(X509_CRL_get0_nextUpdate(crl.get()));
    X509_REVOKED* entry = nullptr;
    if (X509_CRL_get0_by_cert(crl.get(), &entry, cert) == 1 && entry != nullptr) {
        a.revoked_at = detail::to_unix(X509_REVOKED_get0_revocationDate(entry));
        if (a.revoked_at <= when) {
            a.status = RevocationStatus::Revoked;
            return a;
        }
        a.status = RevocationStatus::Good;
        return a;
    }
    if (!fresh(a.data_time, next, when)) {
        *why = "the CRL is older than the signature";
        return Answer{};
    }
    a.status = RevocationStatus::Good;
    return a;
}

/// The issuer of `cert` among `pool`, or null.
X509* find_issuer(X509* cert, STACK_OF(X509)* pool) {
    for (int i = 0; pool != nullptr && i < sk_X509_num(pool); ++i) {
        X509* c = sk_X509_value(pool, i);
        if (X509_cmp(c, cert) != 0 && X509_check_issued(c, cert) == X509_V_OK) {
            return c;
        }
    }
    return nullptr;
}

/// The URLs in a certificate's AIA (OCSP) or CRL distribution points.
std::vector<std::string> ocsp_urls(X509* cert) {
    std::vector<std::string> out;
    STACK_OF(OPENSSL_STRING)* list = X509_get1_ocsp(cert);
    for (int i = 0; list != nullptr && i < sk_OPENSSL_STRING_num(list); ++i) {
        out.emplace_back(sk_OPENSSL_STRING_value(list, i));
    }
    X509_email_free(list);  // frees an OPENSSL_STRING stack, whatever its name
    return out;
}

std::vector<std::string> crl_urls(X509* cert) {
    std::vector<std::string> out;
    auto* points = static_cast<STACK_OF(DIST_POINT)*>(
        X509_get_ext_d2i(cert, NID_crl_distribution_points, nullptr, nullptr));
    for (int i = 0; points != nullptr && i < sk_DIST_POINT_num(points); ++i) {
        const DIST_POINT* dp = sk_DIST_POINT_value(points, i);
        if (dp->distpoint == nullptr || dp->distpoint->type != 0) {
            continue;  // a relative name, not a URL
        }
        const GENERAL_NAMES* names = dp->distpoint->name.fullname;
        for (int k = 0; k < sk_GENERAL_NAME_num(names); ++k) {
            const GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, k);
            if (gn->type == GEN_URI) {
                const ASN1_IA5STRING* uri = gn->d.uniformResourceIdentifier;
                out.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri)),
                                 static_cast<std::size_t>(ASN1_STRING_length(uri)));
            }
        }
    }
    sk_DIST_POINT_pop_free(points, DIST_POINT_free);
    return out;
}

bool http_url(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

/// Every certificate of the signature or token `der`, with the signer (and,
/// for a signature, its timestamp authority) first. Null on unparsable DER.
struct Parsed {
    X509StackPtr certs;
    std::vector<X509*> signers;  ///< owned by `certs`
};

Parsed parse_signers(const Bytes& der) {
    Parsed out;
    const unsigned char* p = der.data();
    detail::CmsPtr cms{d2i_CMS_ContentInfo(nullptr, &p, static_cast<long>(der.size()))};
    if (!cms || OBJ_obj2nid(CMS_get0_type(cms.get())) != NID_pkcs7_signed) {
        ERR_clear_error();
        return out;
    }
    out.certs.reset(CMS_get1_certs(cms.get()));
    if (!out.certs) {
        out.certs.reset(sk_X509_new_null());
    }
    STACK_OF(CMS_SignerInfo)* infos = CMS_get0_SignerInfos(cms.get());
    for (int i = 0; infos != nullptr && i < sk_CMS_SignerInfo_num(infos); ++i) {
        CMS_SignerInfo* si = sk_CMS_SignerInfo_value(infos, i);
        for (int k = 0; k < sk_X509_num(out.certs.get()); ++k) {
            X509* c = sk_X509_value(out.certs.get(), k);
            if (CMS_SignerInfo_cert_cmp(si, c) == 0) {
                out.signers.push_back(c);
                break;
            }
        }
        // A B-T signature's timestamp token: its authority too.
        const int idx = CMS_unsigned_get_attr_by_NID(si, NID_id_smime_aa_timeStampToken, -1);
        const ASN1_TYPE* type = idx >= 0 ? X509_ATTRIBUTE_get0_type(CMS_unsigned_get_attr(si, idx), 0)
                                         : nullptr;
        if (type != nullptr && type->type == V_ASN1_SEQUENCE) {
            const Bytes token(type->value.sequence->data,
                              type->value.sequence->data + type->value.sequence->length);
            Parsed inner = parse_signers(token);
            for (int k = 0; inner.certs && k < sk_X509_num(inner.certs.get()); ++k) {
                X509* c = sk_X509_value(inner.certs.get(), k);
                X509_up_ref(c);
                sk_X509_push(out.certs.get(), c);
            }
            out.signers.insert(out.signers.end(), inner.signers.begin(), inner.signers.end());
        }
    }
    ERR_clear_error();
    return out;
}

/// The chain of every signer in `signatures`, each certificate once, anchors
/// left out: (certificate, issuer) pairs, owned by `keep`.
std::vector<std::pair<X509*, X509*>> chains(const std::vector<Bytes>& signatures,
                                            const TrustStore& trust, const RevocationData& embedded,
                                            std::vector<X509StackPtr>* keep) {
    std::vector<std::pair<X509*, X509*>> out;
    std::set<std::string> seen;
    for (const Bytes& der : signatures) {
        Parsed parsed = parse_signers(der);
        if (!parsed.certs) {
            continue;
        }
        detail::add_certs(embedded, parsed.certs.get());
        for (X509* signer : parsed.signers) {
            X509StackPtr chain;
            std::string why;
            (void)detail::evaluate_chain(signer, parsed.certs.get(), trust,
                                         static_cast<std::int64_t>(std::time(nullptr)), &chain,
                                         &why);
            // The issuer of the last certificate may be missing from a partial
            // chain; the pool may still hold it.
            for (int i = 0; chain && i < sk_X509_num(chain.get()); ++i) {
                X509* c = sk_X509_value(chain.get(), i);
                if (self_signed(c)) {
                    continue;
                }
                X509* issuer = i + 1 < sk_X509_num(chain.get()) ? sk_X509_value(chain.get(), i + 1)
                                                               : find_issuer(c, parsed.certs.get());
                const std::string fp = detail::info(c).sha256;
                if (issuer == nullptr || !seen.insert(fp).second) {
                    continue;
                }
                out.emplace_back(c, issuer);
            }
            keep->push_back(std::move(chain));
        }
        keep->push_back(std::move(parsed.certs));
    }
    return out;
}

}  // namespace

namespace detail {

std::vector<RevocationCheck> check_revocation(STACK_OF(X509)* chain, std::int64_t when,
                                              const RevocationData& embedded,
                                              const RevocationData& online) {
    std::vector<RevocationCheck> out;
    if (chain == nullptr || (embedded.empty() && online.empty())) {
        return out;
    }
    // Certificates a response may name its signer by.
    X509StackPtr pool{sk_X509_new_null()};
    for (int i = 0; pool && i < sk_X509_num(chain); ++i) {
        X509* c = sk_X509_value(chain, i);
        X509_up_ref(c);
        sk_X509_push(pool.get(), c);
    }
    add_certs(embedded, pool.get());
    add_certs(online, pool.get());

    const int n = sk_X509_num(chain);
    for (int i = 0; i < n; ++i) {
        X509* cert = sk_X509_value(chain, i);
        if (i == n - 1 && self_signed(cert)) {
            break;  // the anchor: trusted by being in the store, not by status
        }
        RevocationCheck check;
        check.cert = info(cert);
        X509* issuer = i + 1 < n ? sk_X509_value(chain, i + 1) : find_issuer(cert, pool.get());
        if (issuer == nullptr) {
            check.problem = "its issuer's certificate is not available";
            out.push_back(std::move(check));
            continue;
        }
        std::string why;
        bool decided = false;
        for (const auto& [data, where] :
             {std::pair{&embedded, "embedded"}, std::pair{&online, "fetched now"}}) {
            const auto use = [&](const std::optional<Answer>& a, const char* kind) {
                if (!a || a->status == RevocationStatus::Unknown) {
                    return;
                }
                // Revoked beats good, whatever order the data came in; and a
                // good answer that knows of a LATER revocation beats one that
                // does not, since it says the same and more.
                if (decided && check.status == RevocationStatus::Revoked) {
                    return;
                }
                if (decided && a->status == RevocationStatus::Good &&
                    (a->revoked_at == 0 || check.revoked_at != 0)) {
                    return;
                }
                check.status = a->status;
                check.data_time = a->data_time;
                check.revoked_at = a->revoked_at;
                check.source = std::string(kind) + ", " + where;
                decided = true;
            };
            for (const Bytes& der : data->ocsps) {
                use(from_ocsp(der, cert, issuer, pool.get(), when, &why), "OCSP");
            }
            for (const Bytes& der : data->crls) {
                use(from_crl(der, cert, issuer, when, &why), "CRL");
            }
        }
        if (!decided) {
            check.problem = why.empty() ? "no revocation data about it" : why;
        }
        out.push_back(std::move(check));
    }
    ERR_clear_error();
    return out;
}

}  // namespace detail

std::vector<RevocationQuery> revocation_queries(const std::vector<Bytes>& signatures,
                                                const TrustStore& trust,
                                                const RevocationData& embedded) {
    init(false);
    std::vector<RevocationQuery> out;
    std::vector<X509StackPtr> keep;
    std::set<std::string> asked;
    for (const auto& [cert, issuer] : chains(signatures, trust, embedded, &keep)) {
        if (out.size() >= kMaxQueries) {
            break;
        }
        const std::string subject = display_name(cert);
        for (const std::string& url : ocsp_urls(cert)) {
            if (!http_url(url)) {
                continue;
            }
            OcspReqPtr req{OCSP_REQUEST_new()};
            OCSP_CERTID* id = OCSP_cert_to_id(EVP_sha1(), cert, issuer);
            if (!req || id == nullptr || OCSP_request_add0_id(req.get(), id) == nullptr) {
                OCSP_CERTID_free(id);
                continue;
            }
            unsigned char* der = nullptr;
            const int len = i2d_OCSP_REQUEST(req.get(), &der);
            if (len <= 0) {
                continue;
            }
            RevocationQuery q;
            q.kind = RevocationQuery::Kind::Ocsp;
            q.url = url;
            q.request.assign(der, der + len);
            q.subject = subject;
            OPENSSL_free(der);
            if (asked.insert(q.url + detail::hex(q.request.data(), q.request.size())).second) {
                out.push_back(std::move(q));
            }
            break;  // one responder per certificate is enough
        }
        // CRLs as well: some CAs publish no OCSP, and a CRL is the fallback
        // when a responder is down.
        for (const std::string& url : crl_urls(cert)) {
            if (http_url(url) && asked.insert(url).second) {
                RevocationQuery q;
                q.kind = RevocationQuery::Kind::Crl;
                q.url = url;
                q.subject = subject;
                out.push_back(std::move(q));
                break;
            }
        }
    }
    ERR_clear_error();
    return out;
}

std::vector<FetchedRevocation> fetch_revocation(const std::vector<RevocationQuery>& queries,
                                                int timeout_seconds) {
    init();
    std::vector<FetchedRevocation> out;
    for (const RevocationQuery& q : queries) {
        FetchedRevocation f;
        f.kind = q.kind;
        f.url = q.url;
        detail::HttpRequest http;
        http.url = q.url;
        http.timeout_seconds = timeout_seconds;
        if (q.kind == RevocationQuery::Kind::Ocsp) {
            http.what = "the OCSP responder";
            http.post = &q.request;
            http.content_type = "application/ocsp-request";
            http.expected_type = "application/ocsp-response";
            http.expect_asn1 = true;
            http.max_size = kMaxOcsp;
        } else {
            // CRL servers disagree on the content type, so none is required;
            // the bytes decide. But it is read as one ASN.1 object: otherwise
            // OpenSSL hands back a stream, and a slow server's first empty
            // read looks like the end of it.
            http.what = "the CRL distribution point";
            http.expect_asn1 = true;
            http.max_size = kMaxCrl;
        }
        try {
            f.body = detail::http_transfer(http);
        } catch (const Error& e) {
            f.error = e.what();
        }
        out.push_back(std::move(f));
    }
    return out;
}

RevocationData validation_data(const std::vector<Bytes>& signatures, const TrustStore& trust,
                               const std::vector<FetchedRevocation>& fetched,
                               const RevocationData& embedded) {
    init(false);
    RevocationData out;
    std::set<Bytes> have(embedded.certs.begin(), embedded.certs.end());
    have.insert(embedded.ocsps.begin(), embedded.ocsps.end());
    have.insert(embedded.crls.begin(), embedded.crls.end());
    const auto add = [&](std::vector<Bytes>& to, Bytes der) {
        if (have.insert(der).second) {
            to.push_back(std::move(der));
        }
    };

    std::vector<X509StackPtr> keep;
    for (const auto& [cert, issuer] : chains(signatures, trust, embedded, &keep)) {
        add(out.certs, detail::to_der(cert));
        add(out.certs, detail::to_der(issuer));
    }
    for (const FetchedRevocation& f : fetched) {
        if (f.body.empty()) {
            continue;
        }
        const unsigned char* p = f.body.data();
        if (f.kind == RevocationQuery::Kind::Ocsp) {
            OcspRespPtr resp{d2i_OCSP_RESPONSE(nullptr, &p, static_cast<long>(f.body.size()))};
            if (!resp || OCSP_response_status(resp.get()) != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
                continue;
            }
            // A delegated responder's certificate travels with the response;
            // keep it in /Certs too, where validators look for it.
            OcspBasicPtr bs{OCSP_response_get1_basic(resp.get())};
            if (!bs) {
                continue;
            }
            const STACK_OF(X509)* carried = OCSP_resp_get0_certs(bs.get());
            for (int i = 0; carried != nullptr && i < sk_X509_num(carried); ++i) {
                add(out.certs, detail::to_der(sk_X509_value(carried, i)));
            }
            add(out.ocsps, f.body);
        } else {
            CrlPtr crl{d2i_X509_CRL(nullptr, &p, static_cast<long>(f.body.size()))};
            if (crl) {
                add(out.crls, f.body);
            }
        }
    }
    ERR_clear_error();
    return out;
}

}  // namespace leht::crypto
