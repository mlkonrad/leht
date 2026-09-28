// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Qualified or not: the verdict the EU trusted lists give a signature, case by
// case, with a test PKI whose CA a hand-made trusted list names. The XML side
// (reading real lists) is trustlist/tests; this is what the model means.
#include "leht/crypto/crypto.hpp"
#include "leht/error.hpp"
#include "edit_harness.hpp"
#include "prepared.hpp"
#include "test_pki.hpp"

#include <ctime>
#include <string>

using leht::crypto::CmsReport;
using leht::crypto::QualifiedReport;
using leht::crypto::SignOptions;
using leht::crypto::Trust;
using leht::crypto::TrustStore;
using leht::test::Cert;
using leht::test::Key;
using leht::test::Prepared;
using leht::trustlist::Criteria;
using leht::trustlist::Phase;
using leht::trustlist::Qualification;
using leht::trustlist::Service;
using leht::trustlist::TrustedList;
using Level = QualifiedReport::Level;

namespace {

std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

// --- qcStatements, by hand (ETSI EN 319 412-5) -------------------------------

std::string hex(const std::string& bytes) {
    static const char d[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        out += d[c >> 4];
        out += d[c & 0x0F];
    }
    return out;
}

std::string tlv(unsigned char tag, const std::string& body) {
    return std::string(1, static_cast<char>(tag)) + static_cast<char>(body.size()) + body;
}

const std::string kEtsi = std::string("\x04\x00\x8E\x46\x01", 5);  // 0.4.0.1862.1
std::string oid(const std::string& tail) { return tlv(0x06, kEtsi + tail); }
std::string statement(const std::string& body) { return tlv(0x30, body); }
const std::string kCompliance = statement(oid("\x01"));
const std::string kSscd = statement(oid("\x04"));
std::string qc_type(char t) { return statement(oid("\x06") + tlv(0x30, oid(std::string("\x06", 1) + t))); }
std::string qc(const std::string& statements) { return hex(tlv(0x30, statements)); }

// --- a PKI with a CA a trusted list can name -----------------------------------

struct World {
    Key root_key = leht::test::rsa_key();
    Cert root = leht::test::issue(root_key, {"Test State Root", true, -30, 3650});
    Key ca_key = leht::test::rsa_key();
    Cert ca = leht::test::issue(ca_key, {"Test Qualified CA", true, -30, 3650}, &root, &root_key);
    Key other_key = leht::test::rsa_key();
    Cert other_ca = leht::test::issue(other_key, {"Test Unlisted CA", true, -30, 3650}, &root,
                                      &root_key);
    Key signer_key = leht::test::rsa_key();

    Cert signer(const std::string& qc_hex, const std::string& policies = "",
                const Cert* issuer = nullptr) {
        leht::test::CertSpec spec{"Mari Maasikas", false, -1, 365,
                                  "critical,digitalSignature,nonRepudiation"};
        spec.qc_statements = qc_hex;
        spec.policies = policies;
        return leht::test::issue(signer_key, spec, issuer != nullptr ? issuer : &ca,
                                 issuer == &other_ca ? &other_key : &ca_key);
    }
};

World& world() {
    static World w;
    return w;
}

leht::trustlist::Bytes der(const Cert& c) {
    unsigned char* p = nullptr;
    const int n = i2d_X509(c.p, &p);
    leht::trustlist::Bytes out(p, p + n);
    OPENSSL_free(p);
    return out;
}

Phase granted(std::int64_t since, std::vector<Qualification> q = {}) {
    Phase p;
    p.since = since;
    p.granted = true;
    p.for_esig = true;
    p.qualifications = std::move(q);
    return p;
}

TrustedList list_with(const Cert& ca, std::vector<Phase> phases,
                      Service::Type type = Service::Type::CaQc) {
    TrustedList tl;
    Service s;
    s.type = type;
    s.territory = "EE";
    s.provider = "Test TSP";
    s.name = type == Service::Type::CaQc ? "Test qualified certificates" : "Test qualified TSA";
    s.certs = {der(ca)};
    s.phases = std::move(phases);
    tl.services.push_back(std::move(s));
    return tl;
}

/// Signs with `cert` (chain: the CA) and verifies with `store`; B-T when a TSA
/// URL is given.
CmsReport sign_and_verify(const Cert& cert, const TrustStore& store, const std::string& tsa = "") {
    const Cert& issuer = X509_check_issued(world().other_ca.p, cert.p) == X509_V_OK
                             ? world().other_ca
                             : world().ca;
    const auto id = leht::crypto::Identity::from_pkcs12(
        leht::test::pkcs12(world().signer_key, cert, {&issuer}, "pw"),
        leht::crypto::Secret{"pw"});
    SignOptions o;
    o.tsa_url = tsa;
    static int n = 0;
    const Prepared p("qualified_" + std::to_string(++n) + ".bin",
                     leht::crypto::estimate_signature_size(id, o));
    (void)leht::test::sign(p, id, o);
    return leht::crypto::verify_cms(p.der(), p.content(), store);
}

TrustStore store_with(TrustedList tl) {
    TrustStore s;
    s.add_trusted_list(std::move(tl));
    return s;
}

void report(const CmsReport& r) {
    std::fprintf(stderr, "    trust %d (%s), qualified %d: %s\n", static_cast<int>(r.trust),
                 r.trust_detail.c_str(), static_cast<int>(r.qualified.level),
                 r.qualified.detail.c_str());
}

// --- the cases ------------------------------------------------------------------

void without_a_list_nothing_is_said() {
    TrustStore s;
    s.add_pem(world().root.pem());
    const CmsReport r = sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x01'))), s);
    CHECK(r.trust == Trust::Trusted && r.qualified.level == Level::NotChecked);
}

void a_qualified_certificate_on_a_qscd_is_a_qes() {
    const auto s = store_with(list_with(world().ca, {granted(now() - 86400 * 30)}));
    const CmsReport r = sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x01'))), s);
    if (r.qualified.level != Level::Qes) {
        report(r);
    }
    // Trusted through the list alone: no root was added.
    CHECK(r.trust == Trust::Trusted);
    CHECK(r.qualified.level == Level::Qes);
    CHECK(r.qualified.territory == "EE" && r.qualified.service == "Test qualified certificates");
}

void a_seal_is_a_seal() {
    const auto s = store_with(list_with(world().ca, {granted(now() - 86400)}));
    const CmsReport r = sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x02'))), s);
    CHECK(r.qualified.level == Level::QualifiedSeal);
}

void no_qscd_is_advanced_with_a_qualified_certificate() {
    const auto s = store_with(list_with(world().ca, {granted(now() - 86400)}));
    const CmsReport r = sign_and_verify(world().signer(qc(kCompliance + qc_type('\x01'))), s);
    CHECK(r.qualified.level == Level::AdvancedQc);
}

void the_list_can_qualify_what_the_certificate_does_not_say() {
    // No QC statements at all; the list says QCStatement, QCForESig and
    // QCWithQSCD for certificates with nonRepudiation -- which this one has.
    Qualification q;
    q.qualifiers = leht::trustlist::QcStatement | leht::trustlist::QcForEsig |
                   leht::trustlist::QcWithQscd;
    q.criteria.assert = Criteria::Assert::AtLeastOne;
    q.criteria.key_usage = {{{"nonRepudiation", true}}};
    const auto s = store_with(list_with(world().ca, {granted(now() - 86400, {q})}));
    CHECK(sign_and_verify(world().signer(""), s).qualified.level == Level::Qes);

    // The same qualifiers for a policy this certificate does not carry.
    Qualification other = q;
    other.criteria.key_usage.clear();
    other.criteria.policy_sets = {{"1.2.3.4.5"}};
    const auto s2 = store_with(list_with(world().ca, {granted(now() - 86400, {other})}));
    CHECK(sign_and_verify(world().signer(""), s2).qualified.level == Level::NotQualified);
    // ... and does carry.
    CHECK(sign_and_verify(world().signer("", "1.2.3.4.5"), s2).qualified.level == Level::Qes);

    // A criterion Leht cannot check: the qualifier is not applied.
    Qualification unknown = q;
    unknown.criteria.unknown = true;
    const auto s3 = store_with(list_with(world().ca, {granted(now() - 86400, {unknown})}));
    CHECK(sign_and_verify(world().signer(""), s3).qualified.level == Level::NotQualified);
}

void the_list_can_take_away_the_qscd() {
    Qualification q;
    q.qualifiers = leht::trustlist::QcNoQscd;
    q.criteria.assert = Criteria::Assert::AtLeastOne;
    q.criteria.key_usage = {{{"digitalSignature", true}}};
    const auto s = store_with(list_with(world().ca, {granted(now() - 86400, {q})}));
    const CmsReport r = sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x01'))), s);
    CHECK(r.qualified.level == Level::AdvancedQc);
}

void withdrawn_before_signing_is_neither_trusted_nor_qualified() {
    Phase withdrawn;
    withdrawn.since = now() - 3600;
    withdrawn.granted = false;
    const auto s = store_with(list_with(world().ca, {withdrawn, granted(now() - 86400 * 365)}));
    const CmsReport r = sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x01'))), s);
    CHECK(r.trust == Trust::Untrusted);
    CHECK(r.qualified.level == Level::NotQualified);
}

void withdrawn_after_signing_changes_nothing() {
    // The history as a list would show it later: granted, then withdrawn in
    // an hour. At the signing time -- now -- it was granted.
    Phase withdrawn;
    withdrawn.since = now() + 3600;
    withdrawn.granted = false;
    const auto s = store_with(list_with(world().ca, {withdrawn, granted(now() - 86400)}));
    const CmsReport r = sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x01'))), s);
    CHECK(r.trust == Trust::Trusted && r.qualified.level == Level::Qes);
}

void an_unlisted_ca_is_not_qualified_whatever_it_claims() {
    const auto s = store_with(list_with(world().ca, {granted(now() - 86400)}));
    TrustStore also_root = s;
    also_root.add_pem(world().root.pem());
    const Cert& c = world().other_ca;
    const CmsReport r = sign_and_verify(
        world().signer(qc(kCompliance + kSscd + qc_type('\x01')), "", &c), also_root);
    CHECK(r.trust == Trust::Trusted);  // through the root
    CHECK(r.qualified.level == Level::NotQualified &&
          r.qualified.detail.find("no EU trusted list") != std::string::npos);
    // Without the root it is not even trusted: the list anchors only its own.
    const CmsReport bare =
        sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x01')), "", &c), s);
    CHECK(bare.trust == Trust::Untrusted && bare.qualified.level == Level::NotQualified);
}

void a_timestamp_from_a_listed_tsa_is_qualified() {
    const leht::test::Pki pki;
    const leht::test::LocalTsa tsa(pki.tsa_key, pki.tsa_cert, pki.ca);
    TrustedList tl = list_with(world().ca, {granted(now() - 86400)});
    TrustedList tsas = list_with(pki.tsa_cert, {granted(now() - 86400)}, Service::Type::TsaQtst);
    tl.services.push_back(tsas.services.front());
    const auto s = store_with(tl);
    const CmsReport r =
        sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x01'))), s, tsa.url());
    CHECK(r.timestamp && r.timestamp->valid);
    CHECK(r.timestamp->trust == Trust::Trusted);  // through the list: no CA added
    CHECK(r.timestamp->qualified.level == Level::QualifiedTimestamp);
    CHECK(r.qualified.level == Level::Qes);

    // A qualified TSA is no signer: listing it does not trust a signature
    // chain that ends at it, nor a CA for timestamps.
    const auto only_tsa = store_with(tsas);
    const CmsReport r2 =
        sign_and_verify(world().signer(qc(kCompliance + kSscd + qc_type('\x01'))), only_tsa,
                        tsa.url());
    CHECK(r2.trust == Trust::Untrusted);
    CHECK(r2.timestamp && r2.timestamp->qualified.level == Level::QualifiedTimestamp);
}

}  // namespace

int main() {
    leht::crypto::init();
    RUN(without_a_list_nothing_is_said);
    RUN(a_qualified_certificate_on_a_qscd_is_a_qes);
    RUN(a_seal_is_a_seal);
    RUN(no_qscd_is_advanced_with_a_qualified_certificate);
    RUN(the_list_can_qualify_what_the_certificate_does_not_say);
    RUN(the_list_can_take_away_the_qscd);
    RUN(withdrawn_before_signing_is_neither_trusted_nor_qualified);
    RUN(withdrawn_after_signing_changes_nothing);
    RUN(an_unlisted_ca_is_not_qualified_whatever_it_claims);
    RUN(a_timestamp_from_a_listed_tsa_is_qualified);
    return 0;
}
