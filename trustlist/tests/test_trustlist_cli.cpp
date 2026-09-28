// SPDX-License-Identifier: AGPL-3.0-or-later
//
// `leht trusted-list update|status` and what `leht verify` makes of the result,
// end to end: a signed LOTL and national list served from localhost, a test
// qualified CA on that list, and the real binary.
#include "leht/error.hpp"
#include "leht/trustlist/xml.hpp"
#include "edit_harness.hpp"
#include "tl_builder.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace tl = leht::test::tl;
using leht::test::Cert;
using leht::test::Key;
using leht::test::TempPath;

namespace {

/// Serves fixed bodies by path.
class LocalFiles : public leht::test::LocalHttp {
public:
    LocalFiles() { start(); }
    ~LocalFiles() override { stop(); }
    void put(const std::string& path, const std::string& body) {
        const std::lock_guard<std::mutex> lock(mutex_);
        files_[path] = body;
    }

private:
    Reply answer(const std::string& method, const std::string& path,
                 const std::string& /*body*/) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = files_.find(path);
        if (method != "GET" || it == files_.end()) {
            return {};
        }
        return {"application/vnd.etsi.tsl+xml", it->second};
    }
    std::mutex mutex_;
    std::map<std::string, std::string> files_;
};

std::string text(const leht::trustlist::Bytes& b) { return {b.begin(), b.end()}; }

struct Scheme {
    Key lotl_key = leht::test::rsa_key();
    Cert lotl_signer = leht::test::issue(lotl_key, {"Test LOTL signer", false, -1, 365});
    Key ee_key = leht::test::rsa_key();
    Cert ee_signer = leht::test::issue(ee_key, {"Test EE list signer", false, -1, 365});
    Key root_key = leht::test::rsa_key();
    Cert root = leht::test::issue(root_key, {"Test State Root", true, -30, 3650});
    Key ca_key = leht::test::rsa_key();
    Cert ca = leht::test::issue(ca_key, {"Test ESTEID CA", true, -30, 3650}, &root, &root_key);
    Key other_key = leht::test::rsa_key();
    Cert other_ca = leht::test::issue(other_key, {"Test Plain CA", true, -30, 3650}, &root,
                                      &root_key);
    Key signer_key = leht::test::rsa_key();
    Cert qualified = leht::test::issue(signer_key, {"Mari Maasikas"}, &ca, &ca_key);
    Cert plain = leht::test::issue(signer_key, {"Mari Maasikas"}, &other_ca, &other_key);
    LocalFiles server;
    const std::string oj = "https://eur-lex.test/oj/2026/1";

    [[nodiscard]] std::string lotl_url() const { return server.base() + "/lotl.xml"; }
    [[nodiscard]] std::string ee_url() const { return server.base() + "/ee.xml"; }

    Scheme() {
        server.put("/lotl.xml", text(tl::sign(
            tl::unsigned_list(tl::scheme("EU", {oj}, tl::pointers({{"EU", lotl_url(), {&lotl_signer}},
                                                                    {"EE", ee_url(), {&ee_signer}}})),
                              lotl_signer),
            lotl_key)));
        tl::ServiceSpec svc;
        svc.cert = &ca;
        svc.name = "Test ESTEID qualified certificates";
        tl::PhaseSpec phase;
        phase.additional = {"ForeSignatures"};
        phase.qualifications = {{{"QCStatement", "QCForESig", "QCWithQSCD"}, ""}};
        svc.phases = {phase};
        server.put("/ee.xml", text(tl::sign(
            tl::unsigned_list(tl::scheme("EE", {"https://ria.test/"}, "") + tl::providers({svc}),
                              ee_signer),
            ee_key)));
    }
};

std::pair<std::string, int> leht_cli(const std::string& cache, const std::string& args) {
    const std::string out = leht::test::capture("XDG_CACHE_HOME='" + cache + "' '" +
                                                std::string(LEHT_CLI) + "' " + args +
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

void the_cli_updates_and_verifies_qualified_signatures() {
    Scheme k;
    const TempPath cache("tl_cli_cache");
    std::filesystem::remove_all(cache.str());
    std::filesystem::create_directories(cache.str());
    const TempPath anchor("tl_cli_anchor.txt");
    leht::test::write_file(anchor.str(), "lotl " + k.lotl_url() + "\noj " + k.oj + "\nsha256 " +
                                             tl::sha256_hex(k.lotl_signer) + "\n");
    const std::string with_anchor = " --anchor '" + anchor.str() + "'";

    const auto [none, none_rc] = leht_cli(cache.str(), "trusted-list status");
    CHECK(none_rc == 1 && says(none, "no EU trusted list is cached"));

    const auto [upd, upd_rc] = leht_cli(cache.str(), "trusted-list update" + with_anchor);
    CHECK(upd_rc == 0 && says(upd, "1 of 1 verified") && says(upd, "1 qualified service\n"));
    CHECK(says(upd, "fetching the List of Trusted Lists from 127.0.0.1"));
    const auto [st, st_rc] = leht_cli(cache.str(), "trusted-list status");
    CHECK(st_rc == 0 && says(st, "EE  ok") && says(st, "1 service"));

    // A signature by a certificate from the listed CA: trusted and qualified,
    // with no --trust at all.
    const auto p12 = leht::test::pkcs12(k.signer_key, k.qualified, {&k.ca}, "pw");
    const TempPath key("tl_cli_id.p12");
    leht::test::write_file(key.str(), std::string(p12.begin(), p12.end()));
    const TempPath pw("tl_cli_pw");
    leht::test::write_file(pw.str(), "pw\n");
    const TempPath doc("tl_cli_signed.pdf");
    const auto [signed_, sign_rc] = leht_cli(
        cache.str(), "sign '" + std::string(LEHT_CORPUS_DIR) + "/text_10p.pdf' -o '" + doc.str() +
                         "' --p12 '" + key.str() + "' --password-fd 3 3<'" + pw.str() + "'");
    CHECK(sign_rc == 0);
    (void)signed_;
    const auto [v, v_rc] = leht_cli(cache.str(), "verify '" + doc.str() + "' --require-qualified");
    CHECK(v_rc == 0 && says(v, "trusted") && says(v, "qualified electronic signature (QES)"));
    const auto [j, j_rc] = leht_cli(cache.str(), "verify '" + doc.str() + "' --json");
    CHECK(j_rc == 0 && says(j, "\"level\":\"qes\"") && says(j, "\"territory\":\"EE\""));
    // Without the list it is only intact: nobody vouches for the CA.
    const auto [bare, bare_rc] = leht_cli(cache.str(), "verify '" + doc.str() + "' --no-trusted-list");
    CHECK(bare_rc == 5 && says(bare, "not trusted"));

    // A certificate from a CA not on the list, trusted by hand: exit 8 when a
    // QES is required.
    const auto p12b = leht::test::pkcs12(k.signer_key, k.plain, {&k.other_ca}, "pw");
    const TempPath keyb("tl_cli_id2.p12");
    leht::test::write_file(keyb.str(), std::string(p12b.begin(), p12b.end()));
    const TempPath root("tl_cli_root.pem");
    leht::test::write_file(root.str(), k.root.pem());
    const TempPath doc2("tl_cli_plain.pdf");
    CHECK(leht_cli(cache.str(), "sign '" + std::string(LEHT_CORPUS_DIR) + "/text_10p.pdf' -o '" +
                                    doc2.str() + "' --p12 '" + keyb.str() +
                                    "' --password-fd 3 3<'" + pw.str() + "'")
              .second == 0);
    const auto [plain, plain_rc] = leht_cli(
        cache.str(), "verify '" + doc2.str() + "' --trust '" + root.str() + "' --require-qualified");
    CHECK(plain_rc == 8 && says(plain, "not qualified") && says(plain, "no EU trusted list"));

    // An update that cannot be verified keeps the list already cached.
    const TempPath wrong("tl_cli_wrong_anchor.txt");
    leht::test::write_file(wrong.str(), "lotl " + k.lotl_url() + "\noj " + k.oj + "\nsha256 " +
                                            std::string(64, '0') + "\n");
    const auto [bad, bad_rc] = leht_cli(cache.str(), "trusted-list update --anchor '" + wrong.str() + "'");
    CHECK(bad_rc == 1 && says(bad, "not allowed"));
    CHECK(says(leht_cli(cache.str(), "trusted-list status").first, "EE  ok"));
    std::filesystem::remove_all(cache.str());
}

}  // namespace

int main() {
    leht::crypto::init();
    leht::trustlist::init();  // xmlsec1, for signing the synthetic lists
    RUN(the_cli_updates_and_verifies_qualified_signatures);
    return 0;
}
