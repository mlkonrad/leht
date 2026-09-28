// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Qualified or not, per the EU trusted lists: ETSI TS 119 615, simplified.
//
// A signature is a qualified electronic signature (QES) when, at the trusted
// time:
//   - its certificate was issued by a CA/QC service on a trusted list whose
//     status was "granted";
//   - the certificate is qualified: it says so itself (the QcCompliance
//     statement) or the list says so for it (the QCStatement qualifier), and
//     the list does not say NotQualified;
//   - it is for electronic signatures: its QcType says esign, or the list says
//     QCForESig, or the service issues qualified certificates for
//     e-signatures only;
//   - its key is on a qualified signature creation device (QSCD): the list
//     says QCWithQSCD, or the certificate says QcSSCD and the list does not
//     say QCNoQSCD.
// The same for e-seals makes a qualified electronic seal; qualified but not on
// a QSCD is an advanced signature with a qualified certificate. A list's
// qualifier counts only for certificates its criteria match -- key usage and
// certificate policies; a criterion Leht cannot check means the qualifier is
// not applied, never guessed at.
//
// Not done: the full TS 119 172-4 signature validation policies, and trusted
// lists outside the EU.
#include "ossl.hpp"

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <set>
#include <vector>

namespace leht::crypto::detail {

namespace {

// id-etsi-qcs-*: ETSI EN 319 412-5.
constexpr const char* kQcCompliance = "0.4.0.1862.1.1";
constexpr const char* kQcSscd = "0.4.0.1862.1.4";
constexpr const char* kQcType = "0.4.0.1862.1.6";
constexpr const char* kQcTypeEsign = "0.4.0.1862.1.6.1";
constexpr const char* kQcTypeEseal = "0.4.0.1862.1.6.2";
constexpr const char* kQcTypeWeb = "0.4.0.1862.1.6.3";

std::string oid_text(const ASN1_OBJECT* obj) {
    std::array<char, 128> buf{};
    const int n = OBJ_obj2txt(buf.data(), static_cast<int>(buf.size()), obj, 1);
    return n > 0 && n < static_cast<int>(buf.size()) ? std::string(buf.data()) : std::string{};
}

/// A bounded walk over DER: enough for qcStatements, which is a SEQUENCE of
/// SEQUENCE { OID, ANY OPTIONAL }.
struct Der {
    const unsigned char* p;
    long left;

    /// The next element's tag and content; false at the end or on damage.
    bool next(int* tag, const unsigned char** content, long* len) {
        if (left <= 0) {
            return false;
        }
        const unsigned char* q = p;
        int t = 0;
        int cls = 0;
        const int rc = ASN1_get_object(&q, len, &t, &cls, left);
        if ((rc & 0x80) != 0 || *len < 0 || *len > left - (q - p)) {
            return false;
        }
        *tag = t;
        *content = q;
        left -= (q - p) + *len;
        p = q + *len;
        return true;
    }
};

struct QcInfo {
    bool compliance = false;
    bool sscd = false;
    std::set<std::string> types;
};

QcInfo qc_statements(X509* cert) {
    QcInfo out;
    const int at = X509_get_ext_by_NID(cert, NID_qcStatements, -1);
    if (at < 0) {
        return out;
    }
    const ASN1_OCTET_STRING* value = X509_EXTENSION_get_data(X509_get_ext(cert, at));
    Der outer{ASN1_STRING_get0_data(value), ASN1_STRING_length(value)};
    int tag = 0;
    const unsigned char* seq = nullptr;
    long seq_len = 0;
    if (!outer.next(&tag, &seq, &seq_len) || tag != V_ASN1_SEQUENCE) {
        return out;
    }
    Der statements{seq, seq_len};
    const unsigned char* st = nullptr;
    long st_len = 0;
    while (statements.next(&tag, &st, &st_len)) {
        if (tag != V_ASN1_SEQUENCE) {
            continue;
        }
        Der parts{st, st_len};
        const unsigned char* oid = nullptr;
        long oid_len = 0;
        if (!parts.next(&tag, &oid, &oid_len) || tag != V_ASN1_OBJECT) {
            continue;
        }
        // Rebuild the OID with its header, as d2i wants it.
        ASN1_OBJECT* obj = nullptr;
        {
            std::vector<unsigned char> der;
            der.push_back(V_ASN1_OBJECT);
            if (oid_len < 128) {
                der.push_back(static_cast<unsigned char>(oid_len));
            } else {
                continue;  // no QC statement OID is that long
            }
            der.insert(der.end(), oid, oid + oid_len);
            const unsigned char* dp = der.data();
            obj = d2i_ASN1_OBJECT(nullptr, &dp, static_cast<long>(der.size()));
        }
        const std::string id = obj != nullptr ? oid_text(obj) : std::string{};
        ASN1_OBJECT_free(obj);
        if (id == kQcCompliance) {
            out.compliance = true;
        } else if (id == kQcSscd) {
            out.sscd = true;
        } else if (id == kQcType) {
            // statementInfo: SEQUENCE OF OBJECT IDENTIFIER
            const unsigned char* info = nullptr;
            long info_len = 0;
            if (parts.next(&tag, &info, &info_len) && tag == V_ASN1_SEQUENCE) {
                Der types{info, info_len};
                const unsigned char* t = nullptr;
                long t_len = 0;
                while (types.next(&tag, &t, &t_len)) {
                    if (tag != V_ASN1_OBJECT || t_len >= 128) {
                        continue;
                    }
                    std::vector<unsigned char> der{V_ASN1_OBJECT,
                                                   static_cast<unsigned char>(t_len)};
                    der.insert(der.end(), t, t + t_len);
                    const unsigned char* dp = der.data();
                    ASN1_OBJECT* o = d2i_ASN1_OBJECT(nullptr, &dp, static_cast<long>(der.size()));
                    if (o != nullptr) {
                        out.types.insert(oid_text(o));
                        ASN1_OBJECT_free(o);
                    }
                }
            }
        }
    }
    ERR_clear_error();
    return out;
}

std::set<std::string> policies(X509* cert) {
    std::set<std::string> out;
    auto* pols = static_cast<CERTIFICATEPOLICIES*>(
        X509_get_ext_d2i(cert, NID_certificate_policies, nullptr, nullptr));
    for (int i = 0; pols != nullptr && i < sk_POLICYINFO_num(pols); ++i) {
        out.insert(oid_text(sk_POLICYINFO_value(pols, i)->policyid));
    }
    CERTIFICATEPOLICIES_free(pols);
    ERR_clear_error();
    return out;
}

/// KeyUsageBit names (TS 119 612) to OpenSSL's flags.
std::uint32_t key_usage_flag(const std::string& name) {
    if (name == "digitalSignature") return KU_DIGITAL_SIGNATURE;
    if (name == "nonRepudiation" || name == "contentCommitment") return KU_NON_REPUDIATION;
    if (name == "keyEncipherment") return KU_KEY_ENCIPHERMENT;
    if (name == "dataEncipherment") return KU_DATA_ENCIPHERMENT;
    if (name == "keyAgreement") return KU_KEY_AGREEMENT;
    if (name == "keyCertSign") return KU_KEY_CERT_SIGN;
    if (name == "crlSign") return KU_CRL_SIGN;
    if (name == "encipherOnly") return KU_ENCIPHER_ONLY;
    if (name == "decipherOnly") return KU_DECIPHER_ONLY;
    return 0;
}

struct Subject {
    std::uint32_t key_usage;
    std::set<std::string> policies;
};

bool matches(const trustlist::Criteria& c, const Subject& s, int depth = 0) {
    if (c.unknown || depth > 8) {
        return false;
    }
    std::vector<bool> results;
    for (const auto& ku : c.key_usage) {
        bool ok = !ku.empty();
        for (const auto& [bit, value] : ku) {
            const std::uint32_t flag = key_usage_flag(bit);
            if (flag == 0) {
                return false;  // a bit Leht does not know: cannot check
            }
            ok = ok && (((s.key_usage & flag) != 0) == value);
        }
        results.push_back(ok);
    }
    for (const auto& set : c.policy_sets) {
        results.push_back(!set.empty() && std::all_of(set.begin(), set.end(), [&](const auto& oid) {
                              return s.policies.count(oid) != 0;
                          }));
    }
    for (const trustlist::Criteria& n : c.nested) {
        if (n.unknown) {
            return false;
        }
        results.push_back(matches(n, s, depth + 1));
    }
    if (results.empty()) {
        return false;  // criteria that say nothing select nothing
    }
    switch (c.assert) {
        case trustlist::Criteria::Assert::All:
            return std::all_of(results.begin(), results.end(), [](bool b) { return b; });
        case trustlist::Criteria::Assert::AtLeastOne:
            return std::any_of(results.begin(), results.end(), [](bool b) { return b; });
        case trustlist::Criteria::Assert::None:
            return std::none_of(results.begin(), results.end(), [](bool b) { return b; });
    }
    return false;
}

std::string when_text(std::int64_t t) {
    const auto tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::array<char, 32> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%d", &tm);
    return buf.data();
}

/// The listed service whose certificate issued `cert` (or is it), of `type`.
const TrustStore::Impl::ListCert* listed_issuer(X509* cert, const TrustStore& trust,
                                                trustlist::Service::Type type, bool self) {
    for (const TrustStore::Impl::ListCert& lc : trust.impl().list_certs) {
        if (lc.service->type != type) {
            continue;
        }
        if (self) {
            if (X509_cmp(lc.cert.get(), cert) == 0) {
                return &lc;
            }
            continue;
        }
        EVP_PKEY* key = X509_get0_pubkey(lc.cert.get());
        if (X509_check_issued(lc.cert.get(), cert) == X509_V_OK && key != nullptr &&
            X509_verify(cert, key) == 1) {
            return &lc;
        }
    }
    ERR_clear_error();
    return nullptr;
}

/// A phase in words, for "why not": "withdrawn since 2021-05-21".
std::string not_granted(const trustlist::Service& s, std::int64_t when) {
    const trustlist::Phase* p = s.at(when);
    if (p == nullptr) {
        return "it was not yet on the list at the signing time";
    }
    return "its qualified status had ended by the signing time (since " + when_text(p->since) + ")";
}

}  // namespace

QualifiedReport qualify_signer(X509* signer, STACK_OF(X509)* chain, const TrustStore& trust,
                               std::int64_t when) {
    using Level = QualifiedReport::Level;
    QualifiedReport r;
    if (trust.trusted_list() == nullptr) {
        return r;
    }
    r.level = Level::NotQualified;
    (void)chain;
    const TrustStore::Impl::ListCert* lc =
        listed_issuer(signer, trust, trustlist::Service::Type::CaQc, false);
    if (lc == nullptr) {
        r.detail = "the certificate's issuer is on no EU trusted list as a qualified CA";
        return r;
    }
    const trustlist::Service& svc = *lc->service;
    r.service = svc.name;
    r.territory = svc.territory;
    const trustlist::Phase* phase = svc.at(when);
    if (phase == nullptr || !phase->granted) {
        r.detail = "the issuing CA is on " + svc.territory + "'s list, but " +
                   not_granted(svc, when);
        return r;
    }

    const QcInfo qc = qc_statements(signer);
    const Subject subject{X509_get_key_usage(signer), policies(signer)};
    std::uint32_t q = 0;
    for (const trustlist::Qualification& x : phase->qualifications) {
        if (matches(x.criteria, subject)) {
            q |= x.qualifiers;
        }
    }
    using trustlist::Qualifier;
    const bool qualified = (qc.compliance || (q & Qualifier::QcStatement) != 0) &&
                           (q & Qualifier::NotQualified) == 0;
    if (!qualified) {
        r.detail = (q & Qualifier::NotQualified) != 0
                       ? "the trusted list says this certificate is not qualified"
                       : "the certificate is not a qualified certificate";
        return r;
    }

    // What it is for: the certificate's own QcType first, then the list.
    bool esig = qc.types.count(kQcTypeEsign) != 0 || (q & Qualifier::QcForEsig) != 0;
    bool eseal = qc.types.count(kQcTypeEseal) != 0 || (q & Qualifier::QcForEseal) != 0;
    const bool web = qc.types.count(kQcTypeWeb) != 0 || (q & Qualifier::QcForWsa) != 0;
    if (!esig && !eseal && !web) {
        // Neither says: the service's own scope decides, when it has one.
        esig = phase->for_esig && !phase->for_eseal;
        eseal = phase->for_eseal && !phase->for_esig;
    }
    if (!esig && !eseal) {
        r.detail = web ? "the certificate is qualified for website authentication, not signing"
                       : "neither the certificate nor the list says what it is qualified for";
        return r;
    }
    if (esig && eseal) {
        r.detail = "the certificate claims to be both for signatures and for seals";
        return r;
    }

    // On a QSCD: the list's word first, then the certificate's.
    bool qscd = qc.sscd;
    if ((q & Qualifier::QcWithQscd) != 0) {
        qscd = true;
    } else if ((q & Qualifier::QcNoQscd) != 0) {
        qscd = false;
    }
    if (!qscd) {
        r.level = Level::AdvancedQc;
        r.detail = "a qualified certificate, but the key is not on a qualified signature "
                   "creation device";
        return r;
    }
    r.level = esig ? Level::Qes : Level::QualifiedSeal;
    r.detail = std::string("qualified certificate for ") + (esig ? "e-signatures" : "e-seals") +
               ", key on a QSCD, from " + svc.name + " (" + svc.territory + ")";
    return r;
}

QualifiedReport qualify_timestamp(X509* tsa, const TrustStore& trust, std::int64_t when) {
    using Level = QualifiedReport::Level;
    QualifiedReport r;
    if (trust.trusted_list() == nullptr) {
        return r;
    }
    r.level = Level::NotQualified;
    // A TSA/QTST service lists either the TSA's own certificate or the CA
    // that issues the TSA's certificates.
    const TrustStore::Impl::ListCert* lc =
        listed_issuer(tsa, trust, trustlist::Service::Type::TsaQtst, true);
    if (lc == nullptr) {
        lc = listed_issuer(tsa, trust, trustlist::Service::Type::TsaQtst, false);
    }
    if (lc == nullptr) {
        r.detail = "the timestamp authority is on no EU trusted list as qualified";
        return r;
    }
    const trustlist::Service& svc = *lc->service;
    r.service = svc.name;
    r.territory = svc.territory;
    const trustlist::Phase* phase = svc.at(when);
    if (phase == nullptr || !phase->granted) {
        r.detail = "the timestamp authority is on " + svc.territory + "'s list, but " +
                   not_granted(svc, when);
        return r;
    }
    r.level = Level::QualifiedTimestamp;
    r.detail = "from " + svc.name + " (" + svc.territory + ")";
    return r;
}

}  // namespace leht::crypto::detail
