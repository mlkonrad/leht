// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Long-term validation on real PDFs: the /DSS revision (B-LT) and document
// timestamps (B-LTA), made with ops:: and crypto:: as the CLI does, read back
// and verified by our code, and checked by qpdf and pdfsig where they can.
#include "leht/context.hpp"
#include "leht/crypto/crypto.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/sign.hpp"
#include "edit_harness.hpp"
#include "test_pki.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::crypto::CmsReport;
using leht::crypto::RevocationData;
using leht::crypto::RevocationStatus;
using leht::crypto::SignOptions;
using leht::crypto::TimestampReport;
using leht::crypto::Trust;
using leht::ops::SignatureInfo;
using leht::ops::SignatureRequest;
using leht::test::corpus;
using leht::test::LtvPki;
using leht::test::TempPath;

namespace {

int open_out(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    return fd;
}

/// A B-T signature, as `leht sign --tsa` makes it.
void sign_bt(const LtvPki& k, const std::string& in, const std::string& out,
             SignatureRequest req = {}) {
    const Context ctx;
    Document doc = Document::open(ctx, in);
    SignOptions o;
    o.tsa_url = k.tsa.url();
    req.reserve = leht::crypto::estimate_signature_size(k.identity(), o);
    const int fd = open_out(out);
    const auto prepared = leht::ops::prepare_signature(ctx, doc, req, fd);
    (void)leht::crypto::sign_prepared(fd, prepared.range, k.identity(), o);
    ::close(fd);
}

/// A document timestamp on top of `in`, as `leht ltv --tsa` adds it.
void timestamp(const LtvPki& k, const std::string& in, const std::string& out) {
    const Context ctx;
    Document doc = Document::open(ctx, in);
    const int fd = open_out(out);
    const auto prepared = leht::ops::prepare_document_timestamp(
        ctx, doc, leht::crypto::estimate_timestamp_size(), fd);
    SignOptions o;
    o.tsa_url = k.tsa.url();
    (void)leht::crypto::timestamp_prepared(fd, prepared.range, o);
    ::close(fd);
}

}  // namespace

namespace {

/// What `leht ltv` does: validation data for every signature and document
/// timestamp in `in`, appended to it as a revision in `out`.
RevocationData add_ltv(const LtvPki& k, const std::string& in, const std::string& out) {
    const Context ctx;
    Document doc = Document::open(ctx, in);
    std::vector<leht::crypto::Bytes> blobs;
    for (const SignatureInfo& s : leht::ops::list_signatures(ctx, doc)) {
        if (s.range_ok) {
            blobs.push_back(s.contents);
        }
    }
    const RevocationData embedded = leht::ops::read_dss(ctx, doc);
    const auto queries = leht::crypto::revocation_queries(blobs, k.trust(), embedded);
    const auto fetched = leht::crypto::fetch_revocation(queries, 5);
    for (const auto& f : fetched) {
        if (f.body.empty()) {
            std::fprintf(stderr, "    fetch %s failed: %s\n", f.url.c_str(), f.error.c_str());
        }
    }
    const RevocationData data = leht::crypto::validation_data(blobs, k.trust(), fetched, embedded);
    const int fd = open_out(out);
    leht::ops::add_validation_data(ctx, doc, data, fd);
    ::close(fd);
    return data;
}

struct Seen {
    std::vector<SignatureInfo> sigs;
    std::vector<CmsReport> cms;          ///< parallel; empty for document timestamps
    std::vector<TimestampReport> stamps; ///< parallel; empty for signatures
    RevocationData dss;
};

/// What `leht verify` does, offline: every signature and document timestamp,
/// with the document's own /DSS.
Seen verify(const LtvPki& k, const std::string& path, const RevocationData& online = {}) {
    const Context ctx;
    Document doc = Document::open(ctx, path);
    Seen out;
    out.dss = leht::ops::read_dss(ctx, doc);
    out.sigs = leht::ops::list_signatures(ctx, doc);
    for (const SignatureInfo& s : out.sigs) {
        CHECK(s.range_ok);
        if (s.document_timestamp) {
            out.cms.emplace_back();
            out.stamps.push_back(leht::crypto::verify_document_timestamp(
                s.contents, leht::ops::signed_bytes(ctx, doc, s.range), k.trust(), out.dss,
                online));
        } else {
            out.cms.push_back(leht::crypto::verify_cms(
                s.contents, leht::ops::signed_bytes(ctx, doc, s.range), k.trust(), 0, out.dss,
                online));
            out.stamps.emplace_back();
        }
    }
    return out;
}

bool qpdf_ok(const std::string& path) {
    if (!leht::test::have_tool("qpdf")) {
        return true;
    }
    return std::system(("qpdf --check '" + path + "' >/dev/null 2>&1").c_str()) == 0;
}

void a_document_timestamp_is_a_timestamp_not_a_broken_signature() {
    const LtvPki k;
    const TempPath ts("ltv_docts.pdf");
    timestamp(k, corpus("text_10p.pdf"), ts.str());
    const Seen v = verify(k, ts.str());
    CHECK(v.sigs.size() == 1);
    const SignatureInfo& s = v.sigs.front();
    CHECK(s.document_timestamp && s.subfilter == "ETSI.RFC3161");
    CHECK(s.covers_whole_revision && !s.changed_after_signing);
    CHECK(s.field == "Timestamp1" && s.page == 0);
    CHECK(v.stamps.front().valid && v.stamps.front().trust == Trust::Trusted);
    CHECK(v.stamps.front().authority.common_name == "Leht LTV TSA");
    CHECK(qpdf_ok(ts.str()));
}

void b_lt_embeds_what_verification_needs_offline() {
    LtvPki k;
    const TempPath bt("ltv_bt.pdf");
    const TempPath lt("ltv_lt.pdf");
    sign_bt(k, corpus("text_10p.pdf"), bt.str());
    const RevocationData added = add_ltv(k, bt.str(), lt.str());
    CHECK(!added.certs.empty() && !added.ocsps.empty() && !added.crls.empty());

    k.revocation.hang_up();  // from here the servers are gone; the file must do
    const Seen v = verify(k, lt.str());
    CHECK(v.dss.ocsps.size() == added.ocsps.size() && v.dss.crls.size() == added.crls.size());
    CHECK(v.dss.certs.size() == added.certs.size());
    const SignatureInfo& s = v.sigs.front();
    // The LTV revision came after the signature, but changed nothing it covers.
    CHECK(s.changed_after_signing && s.only_validation_data_after);
    CHECK(!s.later_signature_covers_changes);
    const CmsReport& r = v.cms.front();
    CHECK(r.intact() && r.trust == Trust::Trusted);
    CHECK(r.revocation.size() == 1 && r.revocation.front().status == RevocationStatus::Good &&
          r.revocation.front().source == "OCSP, embedded");
    CHECK(r.timestamp && r.timestamp->revocation.size() == 1 &&
          r.timestamp->revocation.front().status == RevocationStatus::Good);
    CHECK(qpdf_ok(lt.str()));
}

void b_lta_and_renewal() {
    const LtvPki k;
    const TempPath bt("ltv_bt2.pdf");
    const TempPath lt("ltv_lt2.pdf");
    const TempPath lta("ltv_lta.pdf");
    const TempPath lt2("ltv_lt3.pdf");
    const TempPath lta2("ltv_lta2.pdf");
    sign_bt(k, corpus("text_10p.pdf"), bt.str());
    (void)add_ltv(k, bt.str(), lt.str());
    timestamp(k, lt.str(), lta.str());
    // Renewal: the last timestamp's own validation data, then a new one
    // covering everything.
    (void)add_ltv(k, lta.str(), lt2.str());
    timestamp(k, lt2.str(), lta2.str());

    const Seen v = verify(k, lta2.str());
    CHECK(v.sigs.size() == 3);
    int stamps = 0;
    for (std::size_t i = 0; i < v.sigs.size(); ++i) {
        const SignatureInfo& s = v.sigs[i];
        if (s.document_timestamp) {
            ++stamps;
            CHECK(v.stamps[i].valid && v.stamps[i].trust == Trust::Trusted);
        } else {
            CHECK(v.cms[i].intact() && v.cms[i].trust == Trust::Trusted);
        }
        // Everything after each one is validation data and timestamps.
        CHECK(!s.changed_after_signing || s.only_validation_data_after);
    }
    CHECK(stamps == 2);
    // The first timestamp's authority is checked from the renewal's data.
    for (std::size_t i = 0; i < v.sigs.size(); ++i) {
        if (v.sigs[i].document_timestamp && v.sigs[i].changed_after_signing) {
            CHECK(v.stamps[i].revocation.size() == 1 &&
                  v.stamps[i].revocation.front().status == RevocationStatus::Good);
        }
    }
    CHECK(qpdf_ok(lta2.str()));
}

void a_real_change_after_ltv_is_still_a_change() {
    const LtvPki k;
    const TempPath bt("ltv_bt3.pdf");
    const TempPath lt("ltv_lt4.pdf");
    const TempPath changed("ltv_changed.pdf");
    sign_bt(k, corpus("text_10p.pdf"), bt.str());
    (void)add_ltv(k, bt.str(), lt.str());
    {
        const Context ctx;
        Document doc = Document::open(ctx, lt.str());
        leht::ops::AnnotSpec note;
        note.kind = leht::ops::AnnotKind::Note;
        note.rect = {60, 60, 80, 80};
        note.contents = "added later";
        (void)leht::ops::add_annotation(ctx, doc, 0, note);
        leht::SaveOptions o;
        o.mode = leht::SaveOptions::Mode::Incremental;
        doc.save(changed.str(), o);
    }
    const Seen v = verify(k, changed.str());
    CHECK(v.sigs.front().changed_after_signing && !v.sigs.front().only_validation_data_after);
}

void a_no_changes_certification_takes_ltv() {
    const LtvPki k;
    const TempPath cert("ltv_cert.pdf");
    const TempPath lt("ltv_cert_lt.pdf");
    const TempPath lta("ltv_cert_lta.pdf");
    SignatureRequest req;
    req.certify = 1;
    sign_bt(k, corpus("text_10p.pdf"), cert.str(), req);
    (void)add_ltv(k, cert.str(), lt.str());
    timestamp(k, lt.str(), lta.str());
    const Seen v = verify(k, lta.str());
    const SignatureInfo& s = v.sigs.front();
    CHECK(s.certification == 1 && s.changes_judged);
    if (!s.changes_permitted) {
        for (const std::string& p : s.change_problems) {
            std::fprintf(stderr, "    %s\n", p.c_str());
        }
    }
    CHECK(s.changes_permitted);
}

void revoked_after_signing_still_validates_from_the_file() {
    LtvPki k;
    const TempPath bt("ltv_bt4.pdf");
    const TempPath lt("ltv_lt5.pdf");
    sign_bt(k, corpus("text_10p.pdf"), bt.str());
    // Strictly after the timestamp, which has one-second resolution.
    const std::int64_t signed_at = static_cast<std::int64_t>(std::time(nullptr));
    while (static_cast<std::int64_t>(std::time(nullptr)) < signed_at + 2) {
        ::usleep(100000);
    }
    k.revocation.revoke(k.signer, signed_at + 1);
    (void)add_ltv(k, bt.str(), lt.str());
    const Seen v = verify(k, lt.str());
    const CmsReport& r = v.cms.front();
    CHECK(r.trust == Trust::Trusted);
    CHECK(r.revocation.size() == 1 && r.revocation.front().status == RevocationStatus::Good &&
          r.revocation.front().revoked_at == signed_at + 1);
}

void pdfsig_reads_the_result() {
    if (!leht::test::have_tool("pdfsig")) {
        std::fprintf(stderr, "  SKIP pdfsig (poppler-utils not installed)\n");
        return;
    }
    const LtvPki k;
    const TempPath bt("ltv_bt5.pdf");
    const TempPath lt("ltv_lt6.pdf");
    const TempPath lta("ltv_lta3.pdf");
    sign_bt(k, corpus("text_10p.pdf"), bt.str());
    (void)add_ltv(k, bt.str(), lt.str());
    timestamp(k, lt.str(), lta.str());
    const std::string said = leht::test::capture(
        "SOFTHSM2_CONF=/nonexistent/softhsm2.conf pdfsig '" + lta.str() + "' 2>&1");
    // poppler checks the CMS signature; what it makes of an RFC 3161
    // document timestamp varies by version, so only the signature is held
    // to "valid" here.
    if (said.find("Signature is Valid") == std::string::npos) {
        std::fprintf(stderr, "pdfsig said:\n%s\n", said.c_str());
    }
    CHECK(said.find("Signature is Valid") != std::string::npos);
}


#ifdef LEHT_CLI
/// Runs the leht binary through the shell; returns its output and exit code.
std::pair<std::string, int> leht_cli(const std::string& args) {
    const std::string out = leht::test::capture(std::string("'") + LEHT_CLI + "' " + args +
                                                " 2>&1; echo \"EXIT=$?\"");
    const std::size_t at = out.rfind("EXIT=");
    CHECK(at != std::string::npos);
    return {out.substr(0, at), std::stoi(out.substr(at + 5))};
}

bool says(const std::string& out, const char* what) {
    if (out.find(what) != std::string::npos) {
        return true;
    }
    std::fprintf(stderr, "expected \"%s\" in:\n%s\n", what, out.c_str());
    return false;
}

/// The key and CA as files, for the command line.
struct Files {
    TempPath p12{"ltv_cli_id.p12"};
    TempPath ca{"ltv_cli_ca.pem"};
    TempPath pw{"ltv_cli_pw"};
    explicit Files(const LtvPki& k) {
        const auto der = leht::test::pkcs12(k.signer_key, k.signer, {&k.ca}, "pw");
        leht::test::write_file(p12.str(), std::string(der.begin(), der.end()));
        leht::test::write_file(ca.str(), k.ca.pem());
        leht::test::write_file(pw.str(), "pw\n");
    }
    [[nodiscard]] std::string sign(const std::string& in, const std::string& out) const {
        return "sign '" + in + "' -o '" + out + "' --p12 '" + p12.str() + "' --password-fd 3 3<'" +
               pw.str() + "'";
    }
    [[nodiscard]] std::string trust() const { return " --trust '" + ca.str() + "'"; }
};

void the_cli_signs_b_lta_and_verifies_it_offline() {
    LtvPki k;
    const Files f(k);
    const TempPath out("ltv_cli_lta.pdf");
    const std::string in = corpus("text_10p.pdf");

    const auto [no_tsa, no_tsa_rc] = leht_cli(f.sign(in, out.str()) + " --ltv");
    CHECK(no_tsa_rc == 1 && says(no_tsa, "--ltv needs --tsa"));

    const auto [signed_, rc] =
        leht_cli(f.sign(in, out.str()) + " --tsa " + k.tsa.url() + " --lta" + f.trust());
    CHECK(rc == 0);
    CHECK(says(signed_, "PAdES B-LT)") && says(signed_, "PAdES B-LTA)"));
    CHECK(says(signed_, "contacting 127.0.0.1"));

    k.revocation.hang_up();
    const auto [v, v_rc] = leht_cli("verify '" + out.str() + "'" + f.trust());
    if (v_rc != 0) {
        std::fprintf(stderr, "%s\n", v.c_str());
    }
    CHECK(v_rc == 0);  // not 6: the LTV revisions change nothing signed
    CHECK(says(v, "good (OCSP, embedded"));
    CHECK(says(v, "document timestamp, 2 of 2"));
    CHECK(says(v, "validation data was added afterwards"));
    CHECK(v.find("CHANGED") == std::string::npos);

    const auto [j, j_rc] = leht_cli("verify '" + out.str() + "' --json" + f.trust());
    CHECK(j_rc == 0 && says(j, "\"document_timestamp\":true") &&
          says(j, "\"status\":\"good\""));
}

void the_cli_extends_an_existing_signature() {
    LtvPki k;
    const Files f(k);
    const TempPath bt("ltv_cli_bt.pdf");
    const TempPath lt("ltv_cli_lt.pdf");
    CHECK(leht_cli(f.sign(corpus("text_10p.pdf"), bt.str()) + " --tsa " + k.tsa.url()).second == 0);

    const auto [before, before_rc] = leht_cli("verify '" + bt.str() + "'" + f.trust());
    CHECK(before_rc == 0 && says(before, "not checked: nothing embedded"));
    // --online asks now, and says where it went.
    const auto [online, online_rc] = leht_cli("verify '" + bt.str() + "' --online" + f.trust());
    CHECK(online_rc == 0 && says(online, "fetched now") && says(online, "contacting 127.0.0.1"));

    CHECK(leht_cli("ltv '" + bt.str() + "' -o '" + bt.str() + "'").second == 1);
    const auto [ext, ext_rc] = leht_cli("ltv '" + bt.str() + "' -o '" + lt.str() + "'" + f.trust());
    CHECK(ext_rc == 0 && says(ext, "OCSP response") && says(ext, "no document timestamp"));
    CHECK(says(ext, "good (OCSP, embedded"));
}

void the_cli_reports_a_revoked_signer() {
    LtvPki k;
    const Files f(k);
    k.revocation.revoke(k.signer, static_cast<std::int64_t>(std::time(nullptr)) - 3600);
    const TempPath bt("ltv_cli_rev.pdf");
    const TempPath lt("ltv_cli_rev_lt.pdf");
    CHECK(leht_cli(f.sign(corpus("text_10p.pdf"), bt.str()) + " --tsa " + k.tsa.url()).second == 0);
    const auto [ext, ext_rc] = leht_cli("ltv '" + bt.str() + "' -o '" + lt.str() + "'" + f.trust());
    CHECK(ext_rc == 5 && says(ext, "REVOKED"));
    const auto [v, v_rc] = leht_cli("verify '" + lt.str() + "'" + f.trust());
    CHECK(v_rc == 5 && says(v, "certificate revoked") && says(v, "REVOKED on"));
}
#endif

}  // namespace

/// `test_pdf_ltv --write-seed OUT`: a B-LTA file for the fuzz corpus, made
/// with this file's local TSA and revocation servers (tests/corpus/generate.sh
/// calls it: nothing else on a build machine can answer OCSP).
int write_seed(const std::string& out) {
    const LtvPki k;
    const TempPath bt("ltv_seed_bt.pdf");
    const TempPath lt("ltv_seed_lt.pdf");
    sign_bt(k, corpus("text_10p.pdf"), bt.str());
    (void)add_ltv(k, bt.str(), lt.str());
    timestamp(k, lt.str(), out);
    return 0;
}

int main(int argc, char** argv) {
    leht::crypto::init();
    if (argc == 3 && std::string(argv[1]) == "--write-seed") {
        return write_seed(argv[2]);
    }
    RUN(a_document_timestamp_is_a_timestamp_not_a_broken_signature);
    RUN(b_lt_embeds_what_verification_needs_offline);
    RUN(b_lta_and_renewal);
    RUN(a_real_change_after_ltv_is_still_a_change);
    RUN(a_no_changes_certification_takes_ltv);
    RUN(revoked_after_signing_still_validates_from_the_file);
    RUN(pdfsig_reads_the_result);
#ifdef LEHT_CLI
    RUN(the_cli_signs_b_lta_and_verifies_it_offline);
    RUN(the_cli_extends_an_existing_signature);
    RUN(the_cli_reports_a_revoked_signer);
#endif
    return 0;
}
