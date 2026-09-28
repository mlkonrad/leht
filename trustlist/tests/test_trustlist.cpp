// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The EU trusted lists: the real LOTL, Estonia's and Germany's lists as
// published (fixtures.tar.xz), verified from the Official Journal's digests;
// what must be refused when they are tampered with; and the pivot walk, on
// synthetic lists signed here.
#include "leht/error.hpp"
#include "leht/trustlist/model.hpp"
#include "leht/trustlist/xml.hpp"
#include "edit_harness.hpp"
#include "tl_builder.hpp"

#include <algorithm>
#include <string>

using leht::trustlist::Bytes;
using leht::trustlist::Fetched;
using leht::trustlist::Service;
using leht::trustlist::Step;

namespace tl = leht::test::tl;

namespace {

Bytes fixture(const char* name) {
    const std::string s = leht::test::read_file(std::string(LEHT_TL_FIXTURES) + "/" + name);
    return {s.begin(), s.end()};
}

std::string as_text(const Bytes& b) { return {b.begin(), b.end()}; }
Bytes as_bytes(const std::string& s) { return {s.begin(), s.end()}; }

constexpr const char* kEe = "https://sr.riik.ee/tsl/estonian-tsl.xml";
constexpr const char* kDe = "https://tl.bundesnetzagentur.de/TL-DE.xml";
constexpr std::int64_t kNow = 1790000000;  // 2026-09-21: the fixtures are frozen

template <typename F>
std::string refusal(F&& f) {
    try {
        f();
    } catch (const leht::Error& e) {
        return e.what();
    }
    return {};
}

/// The whole real update, with only Estonia and Germany fetched.
Step real_update(const Bytes& ee, const Bytes& de) {
    const auto& anchor = leht::trustlist::eu_anchor();
    const Bytes lotl = fixture("eu-lotl.xml");
    Step first = leht::trustlist::advance(anchor, lotl, {}, kNow);
    CHECK(!first.done);
    std::vector<Fetched> fetched;
    for (const std::string& url : first.need) {
        Fetched f;
        f.url = url;
        if (url == kEe) {
            f.body = ee;
        } else if (url == kDe) {
            f.body = de;
        } else {
            f.error = "not fetched in this test";
        }
        fetched.push_back(std::move(f));
    }
    return leht::trustlist::advance(anchor, lotl, fetched, kNow);
}

void the_real_lotl_verifies_against_the_official_journal() {
    const auto& anchor = leht::trustlist::eu_anchor();
    CHECK(anchor.sha256.size() == 6);
    const Bytes signer = leht::trustlist::verify_signature(
        fixture("eu-lotl.xml"), [&anchor](const Bytes& der) {
            return std::find(anchor.sha256.begin(), anchor.sha256.end(),
                             tl::sha256_hex(leht::test::Cert{[&der] {
                                 const unsigned char* p = der.data();
                                 return d2i_X509(nullptr, &p, static_cast<long>(der.size()));
                             }()})) != anchor.sha256.end();
        });
    CHECK(!signer.empty());
    // Refused when nothing is allowed to sign it.
    CHECK(!refusal([] {
              (void)leht::trustlist::verify_signature(fixture("eu-lotl.xml"),
                                                      [](const Bytes&) { return false; });
          }).empty());
}

void a_real_update_step_by_step() {
    const Step step = real_update(fixture("estonian-tsl.xml"), fixture("TL-DE.xml"));
    CHECK(step.done && step.need.empty());
    const auto& r = step.result;
    CHECK(r.lotl.verified && r.lotl.sequence == 395);
    CHECK(r.lists.size() >= 25);  // every XML list the LOTL points to
    int verified = 0;
    for (const auto& s : r.lists) {
        if (s.territory == "EE" || s.territory == "DE") {
            if (!s.verified) {
                std::fprintf(stderr, "%s: %s\n", s.territory.c_str(), s.problem.c_str());
            }
            CHECK(s.verified && s.services > 0);
            ++verified;
        } else {
            CHECK(!s.verified && s.problem == "not fetched in this test");
        }
    }
    CHECK(verified == 2);  // Germany's is RSA-PSS: the hybrid path
    // Estonia's ID-card CA: qualified, for e-signatures, on a QSCD, with history.
    bool esteid = false;
    for (const Service& s : r.services) {
        if (s.territory == "EE" && s.type == Service::Type::CaQc &&
            s.name.find("ESTEID") != std::string::npos && s.phases.size() == 1 &&
            s.phases.front().granted) {
            const auto& q = s.phases.front().qualifications;
            const bool qscd = std::any_of(q.begin(), q.end(), [](const auto& x) {
                return (x.qualifiers & leht::trustlist::QcWithQscd) != 0;
            });
            esteid = esteid || (qscd && !s.certs.empty());
        }
        CHECK(!s.phases.empty() && !s.certs.empty());
        for (std::size_t i = 1; i < s.phases.size(); ++i) {
            CHECK(s.phases[i - 1].since >= s.phases[i].since);  // newest first
        }
    }
    CHECK(esteid);
    // A German timestamp authority made it through too.
    CHECK(std::any_of(r.services.begin(), r.services.end(), [](const Service& s) {
        return s.territory == "DE" && s.type == Service::Type::TsaQtst;
    }));
}

void tampering_is_refused() {
    const std::string ee = as_text(fixture("estonian-tsl.xml"));
    const auto verdict = [](const std::string& xml) {
        const Step s = real_update(as_bytes(xml), fixture("TL-DE.xml"));
        for (const auto& l : s.result.lists) {
            if (l.territory == "EE") {
                return l.verified ? std::string("VERIFIED") : l.problem;
            }
        }
        return std::string("missing");
    };
    CHECK(verdict(ee) == "VERIFIED");

    // One byte of a service name.
    std::string changed = ee;
    const std::size_t at = changed.find("ESTEID");
    CHECK(at != std::string::npos);
    changed[at] = 'X';
    CHECK(verdict(changed).find("does not match") != std::string::npos);

    // A second signature.
    const std::size_t sig = ee.find("<ds:Signature");
    const std::size_t sig_end = ee.find("</ds:Signature>") + std::strlen("</ds:Signature>");
    CHECK(sig != std::string::npos && sig_end > sig);
    std::string twice = ee;
    twice.insert(sig_end, ee.substr(sig, sig_end - sig));
    CHECK(verdict(twice).find("more than one signature") != std::string::npos);

    // The signature moved, wrapped in another element.
    std::string moved = ee;
    moved.insert(sig_end, "</Wrapper>");
    moved.insert(sig, "<Wrapper>");
    CHECK(verdict(moved).find("not where") != std::string::npos);

    // The whole-document reference turned into one to something else.
    std::string pointed = ee;
    const std::size_t ref = pointed.find("URI=\"\"");
    CHECK(ref != std::string::npos);
    pointed.replace(ref, 6, "URI=\"#nowhere\"");
    CHECK(!verdict(pointed).empty() && verdict(pointed) != "VERIFIED");

    // A DTD, with an entity.
    std::string dtd = ee;
    const std::size_t root = dtd.find("<TrustServiceStatusList");
    CHECK(root != std::string::npos);
    dtd.insert(root, "<!DOCTYPE x [<!ENTITY e \"boo\">]>");
    CHECK(verdict(dtd).find("DTD") != std::string::npos);

    // Not a list at all.
    CHECK(verdict("<nope/>") != "VERIFIED");
}

void another_countrys_key_is_not_enough() {
    // Germany's list, fetched from Estonia's address: signed, but by a key
    // the LOTL lists for Germany, not for Estonia.
    const Step s = real_update(fixture("TL-DE.xml"), fixture("TL-DE.xml"));
    for (const auto& l : s.result.lists) {
        if (l.territory == "EE") {
            CHECK(!l.verified && l.problem.find("not allowed") != std::string::npos);
        }
    }
}

void an_unknown_official_journal_trusts_nothing() {
    auto anchor = leht::trustlist::eu_anchor();
    anchor.oj_url = "https://eur-lex.europa.eu/eli/C/2099/1/oj";
    const Step s = leht::trustlist::advance(anchor, fixture("eu-lotl.xml"), {}, kNow);
    CHECK(s.done && s.result.services.empty() && !s.result.lotl.verified);
    CHECK(s.result.lotl.problem.find("C/2026/1944") != std::string::npos);
}

void a_pivot_is_followed_by_signature() {
    using leht::test::Cert;
    using leht::test::Key;
    Key root_key = leht::test::rsa_key();
    Cert root = leht::test::issue(root_key, {"Test OJ root", true, -1, 365});
    Key old_key = leht::test::rsa_key();
    Cert old_signer = leht::test::issue(old_key, {"LOTL signer 2025", false, -1, 365});
    Key new_key = leht::test::rsa_key();
    Cert new_signer = leht::test::issue(new_key, {"LOTL signer 2026", false, -1, 365});
    (void)root;

    const std::string lotl_url = "https://lotl.test/eu-lotl.xml";
    const std::string pivot_url = "https://lotl.test/eu-lotl-pivot-9.xml";
    const std::string oj = "https://eur-lex.test/oj/1";
    leht::trustlist::Anchor anchor{lotl_url, oj, {tl::sha256_hex(old_signer)}};

    // The pivot: signed by the OJ-published key, naming the new one.
    const Bytes pivot = tl::sign(
        tl::unsigned_list(tl::scheme("EU", {oj}, tl::pointers({{"EU", lotl_url, {&new_signer}}})),
                          old_signer),
        old_key);
    // The LOTL: signed by the new key, listing the pivot before the OJ.
    const auto lotl_signed_by = [&](const Cert& c, const Key& k) {
        return tl::sign(tl::unsigned_list(
                            tl::scheme("EU", {pivot_url, oj},
                                       tl::pointers({{"EU", lotl_url, {&new_signer}}})),
                            c),
                        k);
    };
    const Bytes lotl = lotl_signed_by(new_signer, new_key);

    const Step first = leht::trustlist::advance(anchor, lotl, {}, kNow);
    CHECK(!first.done && first.need == std::vector<std::string>{pivot_url});
    const Step done = leht::trustlist::advance(anchor, lotl, {{pivot_url, pivot, ""}}, kNow);
    CHECK(done.done && done.result.lotl.verified);

    // Signed by the old key after the pivot moved on: refused.
    const Step stale =
        leht::trustlist::advance(anchor, lotl_signed_by(old_signer, old_key), {{pivot_url, pivot, ""}}, kNow);
    CHECK(stale.done && !stale.result.lotl.verified);

    // A pivot the anchor's key did not sign: refused.
    const Bytes forged = tl::sign(
        tl::unsigned_list(tl::scheme("EU", {oj}, tl::pointers({{"EU", lotl_url, {&new_signer}}})),
                          new_signer),
        new_key);
    const Step broken = leht::trustlist::advance(anchor, lotl, {{pivot_url, forged, ""}}, kNow);
    CHECK(broken.done && !broken.result.lotl.verified &&
          broken.result.lotl.problem.find("pivot") != std::string::npos);

    // A pivot somewhere else than the LOTL's own site: refused before fetching.
    const Bytes elsewhere = tl::sign(
        tl::unsigned_list(tl::scheme("EU", {"https://evil.test/p.xml", oj},
                                     tl::pointers({{"EU", lotl_url, {&new_signer}}})),
                          new_signer),
        new_key);
    const Step away = leht::trustlist::advance(anchor, elsewhere, {}, kNow);
    CHECK(away.done && away.need.empty() && !away.result.lotl.verified);
}

void the_compact_form_round_trips_and_refuses_damage() {
    const Step step = real_update(fixture("estonian-tsl.xml"), fixture("TL-DE.xml"));
    const Bytes blob = leht::trustlist::encode(step.result);
    const auto back = leht::trustlist::decode(blob);
    CHECK(back.services.size() == step.result.services.size());
    CHECK(back.lists.size() == step.result.lists.size());
    CHECK(back.lotl.sequence == 395 && back.built == kNow);
    CHECK(leht::trustlist::encode(back) == blob);
    for (const std::size_t cut : {std::size_t{0}, std::size_t{7}, blob.size() / 2, blob.size() - 1}) {
        bool threw = false;
        try {
            (void)leht::trustlist::decode(Bytes(blob.begin(), blob.begin() + static_cast<long>(cut)));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
    Bytes wrong = blob;
    wrong[4] = 99;  // another version
    bool threw = false;
    try {
        (void)leht::trustlist::decode(wrong);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("version") != std::string::npos;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    leht::trustlist::init();
    RUN(the_real_lotl_verifies_against_the_official_journal);
    RUN(a_real_update_step_by_step);
    RUN(tampering_is_refused);
    RUN(another_countrys_key_is_not_enough);
    RUN(an_unknown_official_journal_trusts_nothing);
    RUN(a_pivot_is_followed_by_signature);
    RUN(the_compact_form_round_trips_and_refuses_damage);
    return 0;
}
