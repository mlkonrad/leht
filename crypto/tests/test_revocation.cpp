// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Revocation and document timestamps (PAdES B-LT, B-LTA), against an OCSP
// responder, a CRL and a TSA on localhost: what to fetch, what to embed, and
// what a verifier must make of it -- including everything it must refuse.
#include "leht/crypto/crypto.hpp"
#include "leht/error.hpp"
#include "edit_harness.hpp"
#include "prepared.hpp"
#include "test_pki.hpp"

#include <ctime>
#include <string>

using leht::crypto::Bytes;
using leht::crypto::CmsReport;
using leht::crypto::RevocationCheck;
using leht::crypto::RevocationData;
using leht::crypto::RevocationQuery;
using leht::crypto::RevocationStatus;
using leht::crypto::SignOptions;
using leht::crypto::Trust;
using leht::test::LocalRevocation;
using leht::test::LtvPki;
using leht::test::Prepared;
using leht::test::sign;
using leht::test::throws;

namespace {

std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

SignOptions bt(const LtvPki& k) {
    SignOptions o;
    o.tsa_url = k.tsa.url();
    return o;
}

std::size_t bt_size(const LtvPki& k) {
    return leht::crypto::estimate_signature_size(k.identity(), bt(k));
}

/// A B-T signature by `k`'s signer, into `p`.
void sign_bt(const LtvPki& k, const Prepared& p) { (void)sign(p, k.identity(), bt(k)); }

/// What `leht ltv` does, in memory: ask, fetch, keep what parses.
RevocationData gather(const LtvPki& k, const Bytes& signature) {
    const auto queries = leht::crypto::revocation_queries({signature}, k.trust());
    const auto fetched = leht::crypto::fetch_revocation(queries, 5);
    for (const auto& f : fetched) {
        if (f.body.empty()) {
            std::fprintf(stderr, "    fetch %s failed: %s\n", f.url.c_str(), f.error.c_str());
        }
    }
    return leht::crypto::validation_data({signature}, k.trust(), fetched);
}

CmsReport verify(const LtvPki& k, const Prepared& p, const RevocationData& embedded,
                 const RevocationData& online = {}) {
    return leht::crypto::verify_cms(p.der(), p.content(), k.trust(), 0, embedded, online);
}

const RevocationCheck* about(const std::vector<RevocationCheck>& checks, const std::string& cn) {
    for (const RevocationCheck& c : checks) {
        if (c.cert.common_name == cn) {
            return &c;
        }
    }
    return nullptr;
}

void dump(const std::vector<RevocationCheck>& checks) {
    for (const RevocationCheck& c : checks) {
        std::fprintf(stderr, "    %s: status %d, %s %s\n", c.cert.common_name.c_str(),
                     static_cast<int>(c.status), c.source.c_str(), c.problem.c_str());
    }
}

void without_data_nothing_is_checked() {
    LtvPki k;
    const Prepared p("rev_none.bin", bt_size(k));
    sign_bt(k, p);
    const CmsReport r = verify(k, p, {});
    CHECK(r.intact() && r.trust == Trust::Trusted);
    CHECK(r.revocation.empty());  // not checked, which is not Unknown
    CHECK(k.revocation.served() == 0);  // and no network, ever, by default
}

void the_queries_cover_signer_and_timestamp_authority() {
    LtvPki k;
    const Prepared p("rev_queries.bin", bt_size(k));
    sign_bt(k, p);
    const auto q = leht::crypto::revocation_queries({p.der()}, k.trust());
    int ocsp = 0;
    int crl = 0;
    bool signer = false;
    bool tsa = false;
    for (const RevocationQuery& x : q) {
        (x.kind == RevocationQuery::Kind::Ocsp ? ocsp : crl) += 1;
        signer = signer || x.subject == "Kati Karu";
        tsa = tsa || x.subject == "Leht LTV TSA";
        CHECK(x.kind == RevocationQuery::Kind::Crl ? x.request.empty() : !x.request.empty());
    }
    CHECK(signer && tsa);
    CHECK(ocsp == 2);  // one each; the root is trusted by being trusted
    CHECK(crl == 1);   // both name the same CRL: fetched once
    // Hostile input asks for nothing, and says so by not throwing.
    CHECK(leht::crypto::revocation_queries({Bytes{0x30, 0x03, 0x02, 0x01}}, k.trust()).empty());
}

void embedded_ocsp_says_good_offline() {
    LtvPki k;
    const Prepared p("rev_good.bin", bt_size(k));
    sign_bt(k, p);
    const RevocationData data = gather(k, p.der());
    CHECK(!data.ocsps.empty() && !data.crls.empty() && !data.certs.empty());

    k.revocation.hang_up();  // everything from here is offline
    const CmsReport r = verify(k, p, data);
    CHECK(r.trust == Trust::Trusted);
    const RevocationCheck* s = about(r.revocation, "Kati Karu");
    CHECK(s != nullptr && s->status == RevocationStatus::Good);
    CHECK(s != nullptr && s->source == "OCSP, embedded");
    CHECK(r.revocation.size() == 1);  // the root is not checked
    CHECK(r.timestamp && r.timestamp->revocation.size() == 1 &&
          r.timestamp->revocation.front().status == RevocationStatus::Good);
}

void revoked_before_the_timestamp_is_revoked() {
    LtvPki k;
    k.revocation.revoke(k.signer, now() - 3600);
    const Prepared p("rev_before.bin", bt_size(k));
    sign_bt(k, p);
    const CmsReport r = verify(k, p, gather(k, p.der()));
    CHECK(r.intact());  // the bytes are fine; the name is not
    CHECK(r.trust == Trust::Revoked);
    const RevocationCheck* s = about(r.revocation, "Kati Karu");
    CHECK(s != nullptr && s->status == RevocationStatus::Revoked && s->revoked_at != 0);
    CHECK(r.trust_detail.find("revoked") != std::string::npos);
}

void revoked_after_the_timestamp_still_stands() {
    LtvPki k;
    const Prepared p("rev_after.bin", bt_size(k));
    sign_bt(k, p);
    // Revoked later than the signature was timestamped: what LTV is for.
    k.revocation.revoke(k.signer, now() + 3600);
    const CmsReport r = verify(k, p, {}, gather(k, p.der()));
    CHECK(r.trust == Trust::Trusted);
    const RevocationCheck* s = about(r.revocation, "Kati Karu");
    CHECK(s != nullptr && s->status == RevocationStatus::Good && s->revoked_at != 0);
    CHECK(s != nullptr && s->source == "OCSP, fetched now");
}

void news_of_a_later_revocation_is_not_lost() {
    // Embedded data from before the revocation says good; fetched now it says
    // revoked -- after the timestamp. Still good, and the report says why.
    LtvPki k;
    const Prepared p("rev_news.bin", bt_size(k));
    sign_bt(k, p);
    const RevocationData before = gather(k, p.der());
    k.revocation.revoke(k.signer, now() + 3600);
    const CmsReport r = verify(k, p, before, gather(k, p.der()));
    CHECK(r.trust == Trust::Trusted);
    const RevocationCheck* s = about(r.revocation, "Kati Karu");
    CHECK(s != nullptr && s->status == RevocationStatus::Good && s->revoked_at != 0);
}

void a_crl_answers_when_the_responder_is_down() {
    LtvPki k;
    k.revocation.ocsp_down(true);
    const Prepared p("rev_crl.bin", bt_size(k));
    sign_bt(k, p);
    RevocationData data = gather(k, p.der());
    CHECK(data.ocsps.empty() && data.crls.size() == 1);
    const CmsReport good = verify(k, p, data);
    const RevocationCheck* s = about(good.revocation, "Kati Karu");
    CHECK(s != nullptr && s->status == RevocationStatus::Good && s->source == "CRL, embedded");

    k.revocation.revoke(k.signer, now() - 60);
    data.crls = {k.revocation.crl()};
    const CmsReport bad = verify(k, p, data);
    CHECK(bad.trust == Trust::Revoked);
}

void stale_answers_decide_nothing() {
    LtvPki k;
    // Issued three days ago, valid for one: nothing it says covers today.
    k.revocation.answers(3 * 86400, 86400);
    const Prepared p("rev_stale.bin", bt_size(k));
    sign_bt(k, p);
    const CmsReport r = verify(k, p, gather(k, p.der()));
    const RevocationCheck* s = about(r.revocation, "Kati Karu");
    CHECK(s != nullptr && s->status == RevocationStatus::Unknown && !s->problem.empty());
    CHECK(r.trust == Trust::Trusted);  // Unknown is reported, not a failure
    // ...but pre-computed answers whose window covers the signature count.
    k.revocation.answers(600, 86400);
    const CmsReport w = verify(k, p, gather(k, p.der()));
    const RevocationCheck* g = about(w.revocation, "Kati Karu");
    CHECK(g != nullptr && g->status == RevocationStatus::Good);
}

void only_the_issuer_may_answer_for_its_certificates() {
    LtvPki k;
    k.revocation.ocsp_down(false);
    const Prepared p("rev_who.bin", bt_size(k));
    sign_bt(k, p);
    const auto ocsp_only = [&] {
        RevocationData d = gather(k, p.der());
        d.crls.clear();
        return d;
    };

    k.revocation.sign_as(LocalRevocation::Signer::Delegated);
    const RevocationCheck* s = nullptr;
    CmsReport r = verify(k, p, ocsp_only());
    s = about(r.revocation, "Kati Karu");
    CHECK(s != nullptr && s->status == RevocationStatus::Good);

    for (const auto who : {LocalRevocation::Signer::DelegatedNoEku,
                           LocalRevocation::Signer::Stranger}) {
        // A revoked answer from someone with no say must not count either.
        k.revocation.revoke(k.signer, now() - 60);
        k.revocation.sign_as(who);
        r = verify(k, p, ocsp_only());
        s = about(r.revocation, "Kati Karu");
        if (s == nullptr || s->status != RevocationStatus::Unknown) {
            dump(r.revocation);
        }
        CHECK(s != nullptr && s->status == RevocationStatus::Unknown && !s->problem.empty());
        CHECK(r.trust == Trust::Trusted);
    }
}

void a_crl_not_signed_by_the_issuer_is_ignored() {
    LtvPki k;
    LtvPki other;  // a CA with the very same name, but another key
    other.revocation.revoke(k.signer, now() - 60);
    const Prepared p("rev_forged_crl.bin", bt_size(k));
    sign_bt(k, p);
    RevocationData forged;
    forged.crls = {other.revocation.crl()};
    const CmsReport r = verify(k, p, forged);
    CHECK(r.trust == Trust::Trusted);
    const RevocationCheck* s = about(r.revocation, "Kati Karu");
    CHECK(s != nullptr && s->status == RevocationStatus::Unknown &&
          s->problem.find("not signed") != std::string::npos);
}

void fetch_failures_are_reported_per_query() {
    LtvPki k;
    const Prepared p("rev_down.bin", bt_size(k));
    sign_bt(k, p);
    const auto queries = leht::crypto::revocation_queries({p.der()}, k.trust());
    k.revocation.hang_up();
    const auto fetched = leht::crypto::fetch_revocation(queries, 5);
    CHECK(fetched.size() == queries.size());
    for (const auto& f : fetched) {
        CHECK(f.body.empty() && !f.error.empty());
    }
    RevocationQuery ftp;
    ftp.kind = RevocationQuery::Kind::Crl;
    ftp.url = "ftp://127.0.0.1/crl";
    const auto refused = leht::crypto::fetch_revocation({ftp}, 5);
    CHECK(refused.size() == 1 && refused.front().body.empty());
}

void a_document_timestamp_covers_the_bytes() {
    LtvPki k;
    Prepared p("rev_doc_ts.bin", leht::crypto::estimate_timestamp_size());
    SignOptions o;
    o.tsa_url = k.tsa.url();
    const int fd = p.open_rw();
    const auto r = leht::crypto::timestamp_prepared(fd, p.range, o);
    ::close(fd);
    CHECK(r.time > now() - 60 && r.der_size > 0 && r.der_size <= r.hole_size);

    const auto ts = leht::crypto::verify_document_timestamp(p.der(), p.content(), k.trust());
    CHECK(ts.valid && ts.trust == Trust::Trusted && ts.time == r.time);
    CHECK(ts.authority.common_name == "Leht LTV TSA");
    CHECK(ts.revocation.empty());

    // Its authority's revocation, from the data a later extension embeds.
    const RevocationData data = gather(k, p.der());
    const auto with = leht::crypto::verify_document_timestamp(p.der(), p.content(), k.trust(), data);
    CHECK(with.revocation.size() == 1 && with.revocation.front().status == RevocationStatus::Good);

    // One byte of the covered content changed: the imprint no longer matches.
    std::string all = leht::test::read_file(p.path.str());
    all[3] = all[3] == 'X' ? 'Y' : 'X';
    leht::test::write_file(p.path.str(), all);
    const auto bad = leht::crypto::verify_document_timestamp(p.der(), p.content(), k.trust());
    CHECK(!bad.valid && !bad.problem.empty());

    // A CMS signature is not a timestamp token, and garbage is reported.
    const Prepared s("rev_not_ts.bin", bt_size(k));
    sign_bt(k, s);
    CHECK(!leht::crypto::verify_document_timestamp(s.der(), s.content(), k.trust()).valid);
    CHECK(!leht::crypto::verify_document_timestamp(Bytes{1, 2, 3}, s.content(), k.trust()).valid);
    CHECK(throws([&] {
        SignOptions none;
        const int f = p.open_rw();
        try {
            (void)leht::crypto::timestamp_prepared(f, p.range, none);
        } catch (...) {
            ::close(f);
            throw;
        }
        ::close(f);
    }));
}

}  // namespace

int main() {
    leht::crypto::init();
    RUN(without_data_nothing_is_checked);
    RUN(the_queries_cover_signer_and_timestamp_authority);
    RUN(embedded_ocsp_says_good_offline);
    RUN(revoked_before_the_timestamp_is_revoked);
    RUN(revoked_after_the_timestamp_still_stands);
    RUN(news_of_a_later_revocation_is_not_lost);
    RUN(a_crl_answers_when_the_responder_is_down);
    RUN(stale_answers_decide_nothing);
    RUN(only_the_issuer_may_answer_for_its_certificates);
    RUN(a_crl_not_signed_by_the_issuer_is_ignored);
    RUN(fetch_failures_are_reported_per_query);
    RUN(a_document_timestamp_covers_the_bytes);
    return 0;
}
