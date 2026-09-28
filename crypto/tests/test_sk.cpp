// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Smart-ID and Mobile-ID (queue M6): the pure parts against SK's own
// published examples, then every flow against mock SK servers on localhost
// (sk_mock.hpp) -- ending in PDFs that verify with our code, pdfsig and qpdf.
//
// With LEHT_SK_DEMO=1 it also talks to SK's real DEMO environment with its
// test accounts: a Mobile-ID signature, a Smart-ID certificate choice and QR
// session. Never in CI: that is SK's server, not ours.
#include "leht/context.hpp"
#include "leht/crypto/crypto.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/sign.hpp"
#include "edit_harness.hpp"
#include "prepared.hpp"
#include "ossl.hpp"
#include "sk.hpp"
#include "sk_mock.hpp"

#include <openssl/cms.h>
#include <openssl/ssl.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using leht::crypto::Cancelled;
using leht::crypto::Identity;
using leht::crypto::PhoneDialog;
using leht::crypto::Trust;
using leht::test::LocalMobileId;
using leht::test::LocalSmartId;
using leht::test::Prepared;
using leht::test::TempPath;
using leht::test::throws;
namespace sk = leht::crypto::sk;

namespace {

const leht::test::Pki& pki() {
    static const leht::test::Pki p;
    return p;
}

using Bytes = leht::crypto::Bytes;

Bytes from_hex(const std::string& hex) {
    Bytes out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }
    return out;
}

/// What a person at the screen does: scans every QR code shown (the first
/// one after `scan_after` of them), and remembers codes and statuses.
struct Person {
    LocalSmartId* phone = nullptr;
    int qr_shown = 0;
    int scan_after = 1;
    bool scanned = false;
    std::string code;
    std::vector<std::string> statuses;
    int cancel_after_polls = -1;  ///< press Cancel after this many cancel checks
    int checks = 0;

    PhoneDialog dialog(const std::string& text = "Sign test.pdf") {
        PhoneDialog d;
        d.display_text = text;
        d.show_qr = [this](const std::string& link) {
            ++qr_shown;
            if (phone != nullptr && !scanned && qr_shown >= scan_after) {
                scanned = phone->scan(link);
                CHECK(scanned);
            }
        };
        d.show_code = [this](const std::string& c) { code = c; };
        d.status = [this](const std::string& s) { statuses.push_back(s); };
        d.cancelled = [this] { return cancel_after_polls >= 0 && ++checks > cancel_after_polls; };
        return d;
    }
};

template <typename F>
bool cancelled(F&& f) {
    try {
        f();
    } catch (const Cancelled&) {
        return true;
    } catch (const leht::Error& e) {
        std::fprintf(stderr, "  expected Cancelled, got: %s\n", e.what());
    }
    return false;
}

template <typename F>
std::string error_of(F&& f) {
    try {
        f();
    } catch (const leht::Error& e) {
        return e.what();
    }
    return {};
}

bool says(const std::string& text, const std::string& part) {
    if (text.find(part) != std::string::npos) {
        return true;
    }
    std::fprintf(stderr, "  \"%s\" does not say \"%s\"\n", text.c_str(), part.c_str());
    return false;
}

/// The signature algorithm of the one SignerInfo in `der`.
int signature_nid(const Bytes& der) {
    const unsigned char* p = der.data();
    CMS_ContentInfo* cms = d2i_CMS_ContentInfo(nullptr, &p, static_cast<long>(der.size()));
    CHECK(cms != nullptr);
    CMS_SignerInfo* si = sk_CMS_SignerInfo_value(CMS_get0_SignerInfos(cms), 0);
    X509_ALGOR* alg = nullptr;
    CMS_SignerInfo_get0_algs(si, nullptr, nullptr, nullptr, &alg);
    const ASN1_OBJECT* obj = nullptr;
    X509_ALGOR_get0(&obj, nullptr, nullptr, alg);
    const int nid = OBJ_obj2nid(obj);
    CMS_ContentInfo_free(cms);
    return nid;
}

bool hole_is_empty(const Prepared& p) {
    for (const std::uint8_t b : p.der()) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

// --- SK's own examples ---------------------------------------------------------

// From "authCode calculation" and "Device link flows" in SK's RP API v3.2.3
// documentation: the same secret, token and names throughout.
const Bytes kSecret = leht::test::unb64("B98ODiVCebRedSwdTk51zFSaGYyHtY1H2A0ocAi3/Ps=");
const char* kInteractions =
    "W3sidHlwZSI6ImNvbmZpcm1hdGlvbk1lc3NhZ2UiLCJkaXNwbGF5VGV4dDIwMCI6IkxvbmdlciBkZXNjcmlwdGlvbiBv"
    "ZiB0aGUgdHJhbnNhY3Rpb24gY29udGV4dCJ9LHsidHlwZSI6ImRpc3BsYXlUZXh0QW5kUElOIiwiZGlzcGxheVRleHQ2"
    "MCI6IlNob3J0IGRlc2NyaXB0aW9uIG9mIHRoZSB0cmFuc2FjdGlvbiBjb250ZXh0In1d";

sk::LinkParts example_link(const char* type) {
    sk::LinkParts p;
    p.device_link_base = "https://smart-id.com/device-link";
    p.session_token = "wGIrqveE6AuGDATZKmR1mtAZ";
    p.session_type = type;
    p.lang = "eng";
    p.elapsed_seconds = 22;
    p.scheme_name = "smart-id";
    p.relying_party_name = "DEMO";
    p.brokered_rp_name = "Example RP";
    return p;
}

void qr_links_match_sks_examples() {
    // Certificate choice: protocol, digest and interactions empty.
    CHECK(sk::qr_link(example_link("cert"), kSecret) ==
          "https://smart-id.com/device-link?deviceLinkType=QR&elapsedSeconds=22&sessionToken="
          "wGIrqveE6AuGDATZKmR1mtAZ&sessionType=cert&version=1.0&lang=eng&authCode="
          "T5pDDPAjoMS0byqS-FoaWVjlubODXoCCF4XDB4HeYNo");
    sk::LinkParts sign = example_link("sign");
    sign.protocol = "RAW_DIGEST_SIGNATURE";
    sign.digest =
        "FNZFFya5wGLv9b27fZngaWrOBqle4tGwxZuDFRBdPl1RQvxJsfoqvTbjafd+8BcehMOQGvak6zlP+F8tga4bfQ==";
    sign.interactions = kInteractions;
    CHECK(sk::qr_link(sign, kSecret).ends_with("&authCode=5PZVhiNDTnt1MjLz8_YCjnNtR7p0iGevaL2G1ajfMco"));
    sk::LinkParts auth = example_link("auth");
    auth.protocol = "ACSP_V2";
    auth.digest = "GYS+yoah6emAcVDNIajwSs6UB/M95XrDxMzXBUkwQJ9YFDipXXzGpPc7raWcuc2+TEoRc7WvIZ/7dU/"
                  "iRXenYg==";
    auth.interactions = kInteractions;
    CHECK(sk::qr_link(auth, kSecret) ==
          "https://smart-id.com/device-link?deviceLinkType=QR&elapsedSeconds=22&sessionToken="
          "wGIrqveE6AuGDATZKmR1mtAZ&sessionType=auth&version=1.0&lang=eng&authCode="
          "OY1eHaD4UYedrBwtqUbSkpa0w7ttm4FllPkCD_3wlE0");
}

void mobile_id_codes_match_sks_example() {
    // From MID REST's documentation: 6 bits from the start, 7 from the end.
    CHECK(sk::mobile_id_code(from_hex("2f665f6a6999e0ef0752e00ec9f453adf59d8cb6")) == "1462");
    CHECK(sk::mobile_id_code(Bytes(32, 0)) == "0000");
    CHECK(sk::mobile_id_code(Bytes(32, 0xFF)) == "8191");
}

void base64_round_trips_and_refuses_junk() {
    for (std::size_t n = 0; n < 40; ++n) {
        const Bytes data = leht::test::random_bytes(n);
        CHECK(leht::crypto::detail::unbase64(leht::crypto::detail::base64(data)) == data);
    }
    for (const char* junk : {"a", "abc", "ab=c", "ab c", "abcd=", "====", "ab\ncd", "ab-_"}) {
        CHECK(throws([&] { (void)leht::crypto::detail::unbase64(junk); }));
    }
}

void hostile_session_json_is_refused_not_crashed() {
    for (const char* bad : {"", "[]", "null", "{", "{\"state\":1}", "{\"state\":\"DONE\"}",
                            "{\"state\":\"COMPLETE\"}", "{\"state\":\"COMPLETE\",\"result\":[]}",
                            "{\"state\":\"COMPLETE\",\"result\":{\"endResult\":7}}",
                            "{\"state\":\"COMPLETE\",\"result\":{\"endResult\":\"OK\"},"
                            "\"cert\":{\"value\":\"@@@\"}}",
                            "{\"state\":\"RUNNING\",\"signature\":{\"signatureAlgorithmParameters\":"
                            "{\"saltLength\":\"x\"}}}"}) {
        CHECK(throws([&] { (void)sk::parse_smart_id_session(bad); }));
    }
    CHECK(!sk::parse_smart_id_session("{\"state\":\"RUNNING\"}").complete);
    CHECK(throws([&] { (void)sk::parse_mobile_id_session("{\"state\":\"COMPLETE\"}"); }));
    CHECK(throws([&] { (void)sk::parse_mobile_id_session("{\"state\":\"RUNNING\",\"cert\":5}"); }));
    CHECK(!sk::parse_mobile_id_session("{\"state\":\"RUNNING\"}").complete);
}

void a_qr_code_holds_a_whole_link() {
    const std::string link = sk::qr_link(example_link("cert"), kSecret);
    const leht::crypto::QrCode qr = leht::crypto::qr_modules(link);
    // A version-8 or so code: 4 * version + 17 modules a side.
    CHECK(qr.size >= 45 && (qr.size - 17) % 4 == 0);
    CHECK(qr.dark.size() == static_cast<std::size_t>(qr.size * qr.size));
    // The finder pattern's corner is dark, its separator light.
    CHECK(qr.at(0, 0) && qr.at(6, 6) && !qr.at(7, 7));
    if (leht::test::have_tool("zbarimg")) {
        // Read back by a decoder that is not ours: as a PGM, 4 px a module.
        const TempPath pgm("sk_qr.pgm");
        const int scale = 4;
        const int quiet = 4;
        const int side = (qr.size + 2 * quiet) * scale;
        std::string img = "P5\n" + std::to_string(side) + " " + std::to_string(side) + "\n255\n";
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const int mx = x / scale - quiet;
                const int my = y / scale - quiet;
                const bool dark = mx >= 0 && my >= 0 && mx < qr.size && my < qr.size && qr.at(mx, my);
                img += static_cast<char>(dark ? 0 : 255);
            }
        }
        leht::test::write_file(pgm.str(), img);
        CHECK(leht::test::capture("zbarimg -q --raw '" + pgm.str() + "' 2>/dev/null") == link + "\n");
    } else {
        std::fprintf(stderr, "  SKIP zbarimg (zbar not installed)\n");
    }
}

// --- Smart-ID against the mock ---------------------------------------------------

void smart_id_by_qr_signs_with_rsa_pss() {
    LocalSmartId sid(pki());
    Person person;
    person.phone = &sid;
    person.scan_after = 3;  // the code is renewed while nobody scans
    const Identity id = Identity::from_smart_id_qr(sid.service(), person.dialog());
    CHECK(person.scanned);
    CHECK(person.qr_shown >= 3);
    CHECK(id.on_phone() && !id.on_token());
    CHECK(id.certificate().common_name == "MAASIKAS,MARI,PNOEE-40504040001");
    CHECK(id.key_type() == leht::crypto::KeyType::Rsa && id.key_bits() == 3072);
    // The intermediate came from the certificate's caIssuers URL.
    CHECK(sid.issuer_fetches == 1);
    CHECK(id.chain_der().size() == 2);

    const Prepared p("sk_qr.bin", leht::crypto::estimate_signature_size(id, {}));
    const auto r = leht::test::sign(p, id);
    CHECK(r.digest == "SHA-256");
    CHECK(sid.signatures() == 1);
    CHECK(person.code.empty());  // the linked flow has no code to compare
    CHECK(sid.last_display_text() == "Sign test.pdf");
    CHECK(signature_nid(p.der()) == NID_rsassaPss);
    const auto report = p.verify(pki().trust());
    CHECK(report.intact());
    CHECK(report.trust == Trust::Trusted);
    CHECK(report.has_signing_certificate_v2);
    CHECK(sid.complaints.none());

    // The certificate choice links one signature; a second one asks again,
    // with a verification code.
    const Prepared again("sk_qr2.bin", leht::crypto::estimate_signature_size(id, {}));
    (void)leht::test::sign(again, id);
    CHECK(person.code == sid.last_vc() && person.code.size() == 4);
    CHECK(again.verify(pki().trust()).intact());
    CHECK(sid.complaints.none());
}

void openssl_verifies_the_pss_signature() {
    LocalSmartId sid(pki());
    Person person;
    person.phone = &sid;
    const Identity id = Identity::from_smart_id_qr(sid.service(), person.dialog());
    const Prepared p("sk_openssl.bin", leht::crypto::estimate_signature_size(id, {}));
    (void)leht::test::sign(p, id);
    if (!leht::test::have_tool("openssl")) {
        std::fprintf(stderr, "  SKIP openssl CLI\n");
        return;
    }
    const TempPath sig("sk_openssl.der");
    const TempPath content("sk_openssl.content");
    const TempPath ca("sk_openssl_ca.pem");
    const Bytes der = p.der();
    leht::test::write_file(sig.str(), std::string(der.begin(), der.end()));
    const std::string all = leht::test::read_file(p.path.str());
    leht::test::write_file(content.str(), all.substr(0, p.before.size()) +
                                              all.substr(static_cast<std::size_t>(p.range.hole_end())));
    leht::test::write_file(ca.str(), pki().ca.pem());
    const std::string out = leht::test::capture(
        "openssl cms -verify -binary -inform DER -in '" + sig.str() + "' -content '" +
        content.str() + "' -CAfile '" + ca.str() + "' -purpose any -out /dev/null 2>&1");
    CHECK(says(out, "Verification successful"));
}

void smart_id_by_personal_code_shows_sks_code() {
    LocalSmartId sid(pki());
    Person person;
    const Identity id =
        Identity::from_smart_id(sid.service(), LocalSmartId::kPerson, person.dialog("Leping.pdf"));
    CHECK(id.certificate().common_name == "MAASIKAS,MARI,PNOEE-40504040001");
    leht::crypto::SignOptions o;
    const Prepared p("sk_code.bin", leht::crypto::estimate_signature_size(id, o));
    (void)leht::test::sign(p, id, o);
    CHECK(!person.code.empty() && person.code == sid.last_vc());
    CHECK(sid.last_display_text() == "Leping.pdf");
    CHECK(p.verify(pki().trust()).intact());
    CHECK(sid.complaints.none());
}

void smart_id_with_a_timestamp_is_b_t() {
    LocalSmartId sid(pki());
    leht::test::LocalTsa tsa(pki().tsa_key, pki().tsa_cert, pki().ca);
    Person person;
    const Identity id = Identity::from_smart_id(sid.service(), LocalSmartId::kPerson, person.dialog());
    leht::crypto::SignOptions o;
    o.tsa_url = tsa.url();
    const Prepared p("sk_bt.bin", leht::crypto::estimate_signature_size(id, o));
    const auto r = leht::test::sign(p, id, o);
    CHECK(r.timestamp.has_value());
    const auto report = p.verify(pki().trust());
    CHECK(report.intact() && report.timestamp.has_value() && report.timestamp->valid);
}

void smart_id_refusals_and_failures_write_nothing() {
    LocalSmartId sid(pki());
    Person person;
    const Identity id = Identity::from_smart_id(sid.service(), LocalSmartId::kPerson, person.dialog());
    const std::size_t size = leht::crypto::estimate_signature_size(id, {});

    sid.outcome = LocalSmartId::Outcome::Refuse;
    const Prepared refused("sk_refused.bin", size);
    CHECK(cancelled([&] { (void)leht::test::sign(refused, id); }));
    CHECK(hole_is_empty(refused));

    sid.outcome = LocalSmartId::Outcome::Timeout;
    const Prepared timeout("sk_timeout.bin", size);
    CHECK(says(error_of([&] { (void)leht::test::sign(timeout, id); }), "did not answer in time"));
    CHECK(hole_is_empty(timeout));

    sid.outcome = LocalSmartId::Outcome::WrongVc;
    const Prepared vc("sk_vc.bin", size);
    CHECK(says(error_of([&] { (void)leht::test::sign(vc, id); }), "wrong verification code"));

    // A phone whose key is not the certificate's: self_check catches it.
    sid.outcome = LocalSmartId::Outcome::OtherKey;
    const Prepared other("sk_otherkey.bin", size);
    CHECK(says(error_of([&] { (void)leht::test::sign(other, id); }), "does not verify"));
    CHECK(hole_is_empty(other));

    sid.outcome = LocalSmartId::Outcome::OtherCert;
    const Prepared cert("sk_othercert.bin", size);
    CHECK(says(error_of([&] { (void)leht::test::sign(cert, id); }), "different certificate"));

    // PKCS#1 v1.5 when PSS was asked for: not what the SignerInfo says.
    sid.outcome = LocalSmartId::Outcome::Pkcs1;
    const Prepared pkcs1("sk_pkcs1.bin", size);
    CHECK(says(error_of([&] { (void)leht::test::sign(pkcs1, id); }), "other parameters"));
    CHECK(hole_is_empty(pkcs1));
    CHECK(sid.complaints.none());
}

void smart_id_errors_speak_to_the_person() {
    LocalSmartId sid(pki());
    Person person;
    CHECK(says(error_of([&] {
                   (void)Identity::from_smart_id(sid.service(), "PNOEE-30303039914", person.dialog());
               }),
               "no account"));
    sid.fail_next_start = 471;
    CHECK(says(error_of([&] {
                   (void)Identity::from_smart_id(sid.service(), LocalSmartId::kPerson,
                                                 person.dialog());
               }),
               "qualified"));
    sid.fail_next_start = 580;
    CHECK(says(error_of([&] { (void)Identity::from_smart_id_qr(sid.service(), person.dialog()); }),
               "maintenance"));
    auto wrong = sid.service();
    wrong.party.name = "Somebody";
    CHECK(says(error_of([&] { (void)Identity::from_smart_id_qr(wrong, person.dialog()); }),
               "relying party"));
    for (const char* bad : {"38001085718", "PNOEE", "PNOEE-", "PNOEE-1/../x", "pnoee-1",
                            "PNOEE-1?x=1"}) {
        CHECK(says(error_of([&] { (void)Identity::from_smart_id(sid.service(), bad, person.dialog()); }),
                   "not a personal code"));
    }
    sid.refuse_choice = true;
    CHECK(cancelled([&] {
        (void)Identity::from_smart_id(sid.service(), LocalSmartId::kPerson, person.dialog());
    }));
    // Nothing listening.
    auto down = sid.service();
    down.base_url = "http://127.0.0.1:1/v3/";
    CHECK(throws([&] { (void)Identity::from_smart_id_qr(down, person.dialog()); }));
}

void cancel_stops_waiting_for_the_qr_scan() {
    LocalSmartId sid(pki());
    Person person;  // nobody scans
    person.cancel_after_polls = 3;
    CHECK(cancelled([&] { (void)Identity::from_smart_id_qr(sid.service(), person.dialog()); }));
    CHECK(person.qr_shown >= 2);
    PhoneDialog no_qr = person.dialog();
    no_qr.show_qr = nullptr;
    CHECK(throws([&] { (void)Identity::from_smart_id_qr(sid.service(), no_qr); }));
}

void cancel_while_the_phone_signs_writes_nothing() {
    LocalSmartId sid(pki());
    Person person;
    const Identity id = Identity::from_smart_id(sid.service(), LocalSmartId::kPerson, person.dialog());
    sid.polls_before_done = 1000;  // the phone never answers
    person.checks = 0;
    person.cancel_after_polls = 4;
    const Prepared p("sk_cancel.bin", leht::crypto::estimate_signature_size(id, {}));
    CHECK(cancelled([&] { (void)leht::test::sign(p, id); }));
    CHECK(hole_is_empty(p));
}

// --- Mobile-ID against the mock ----------------------------------------------------

void mobile_id_signs_with_ecdsa_and_rsa() {
    for (const bool ec : {true, false}) {
        LocalMobileId mid(pki(), ec);
        Person person;
        const Identity id = Identity::from_mobile_id(mid.service(), LocalMobileId::kPhone,
                                                     LocalMobileId::kId, person.dialog("Leping.pdf"));
        CHECK(id.on_phone());
        CHECK(id.certificate().common_name == "O'CONNEŽ-ŠUSLIK TESTNUMBER,MARY ÄNN,60001017869");
        CHECK(id.key_type() == (ec ? leht::crypto::KeyType::Ec : leht::crypto::KeyType::Rsa));
        CHECK(id.chain_der().size() == 1);  // no caIssuers: the signer alone
        const Prepared p(ec ? "mid_ec.bin" : "mid_rsa.bin",
                         leht::crypto::estimate_signature_size(id, {}));
        const auto r = leht::test::sign(p, id);
        CHECK(r.digest == "SHA-256");
        // The code shown is the one the phone computes from what it signs.
        CHECK(person.code == sk::mobile_id_code(mid.last_hash()));
        CHECK(mid.last_request().value("language", "") == "ENG");
        CHECK(mid.last_request().value("displayText", "") == "Leping.pdf");
        CHECK(signature_nid(p.der()) ==
              (ec ? NID_ecdsa_with_SHA256 : NID_rsaEncryption));
        const auto report = p.verify(pki().trust());
        CHECK(report.intact() && report.trust == Trust::Trusted);
        CHECK(mid.complaints.none());
    }
}

void mobile_id_failures_speak_to_the_person() {
    LocalMobileId mid(pki(), true);
    Person person;
    CHECK(says(error_of([&] {
                   (void)Identity::from_mobile_id(mid.service(), "+37200000266", "60001019939",
                                                  person.dialog());
               }),
               "no active certificate"));
    CHECK(says(error_of([&] {
                   (void)Identity::from_mobile_id(mid.service(), "37268000769", LocalMobileId::kId,
                                                  person.dialog());
               }),
               "country code"));
    CHECK(says(error_of([&] {
                   (void)Identity::from_mobile_id(mid.service(), LocalMobileId::kPhone, "6000/1",
                                                  person.dialog());
               }),
               "not a personal code"));

    const Identity id = Identity::from_mobile_id(mid.service(), LocalMobileId::kPhone,
                                                 LocalMobileId::kId, person.dialog());
    const std::size_t size = leht::crypto::estimate_signature_size(id, {});
    mid.outcome = LocalMobileId::Outcome::UserCancelled;
    const Prepared c("mid_cancelled.bin", size);
    CHECK(cancelled([&] { (void)leht::test::sign(c, id); }));
    CHECK(hole_is_empty(c));
    mid.outcome = LocalMobileId::Outcome::HashMismatch;
    const Prepared h("mid_mismatch.bin", size);
    CHECK(says(error_of([&] { (void)leht::test::sign(h, id); }), "mobile operator"));
    mid.outcome = LocalMobileId::Outcome::OtherKey;
    const Prepared k("mid_otherkey.bin", size);
    CHECK(says(error_of([&] { (void)leht::test::sign(k, id); }), "does not verify"));
    CHECK(hole_is_empty(k));
    mid.outcome = LocalMobileId::Outcome::WrongAlgorithm;
    const Prepared a("mid_alg.bin", size);
    CHECK(says(error_of([&] { (void)leht::test::sign(a, id); }), "SHA512WithECEncryption"));
    CHECK(mid.complaints.none());
}

// --- TLS pinning -----------------------------------------------------------------

/// One HTTPS reply on 127.0.0.1, with a certificate from the test PKI's
/// root, which the client is made to trust through $SSL_CERT_FILE.
class LocalTls {
public:
    explicit LocalTls(const leht::test::Pki& pki)
        : cert_(leht::test::issue(key_, {"127.0.0.1", false, -1, 30, "critical,digitalSignature",
                                         "serverAuth", {}, {}, {}, {}, {}, "IP:127.0.0.1"},
                                  &pki.ca, &pki.ca_key)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        CHECK(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
              ::listen(fd_, 4) == 0 &&
              ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        ctx_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate(ctx_, cert_.p);
        SSL_CTX_add1_chain_cert(ctx_, pki.ca.p);
        SSL_CTX_use_PrivateKey(ctx_, key_.p);
        thread_ = std::thread([this] {
            // A client that hung up must not kill the test with SIGPIPE.
            sigset_t pipe;
            sigemptyset(&pipe);
            sigaddset(&pipe, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
            for (;;) {
                const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
                if (c < 0) {
                    return;
                }
                SSL* ssl = SSL_new(ctx_);
                SSL_set_fd(ssl, c);
                if (SSL_accept(ssl) == 1) {
                    char buf[4096];
                    (void)SSL_read(ssl, buf, sizeof(buf));
                    const std::string reply =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}";
                    (void)SSL_write(ssl, reply.data(), static_cast<int>(reply.size()));
                    SSL_shutdown(ssl);
                }
                SSL_free(ssl);
                ::close(c);
            }
        });
    }
    ~LocalTls() {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        thread_.join();
        SSL_CTX_free(ctx_);
    }
    [[nodiscard]] std::string url() const {
        return "https://127.0.0.1:" + std::to_string(port_) + "/x";
    }

private:
    leht::test::Key key_ = leht::test::rsa_key();
    leht::test::Cert cert_;
    int fd_ = -1;
    int port_ = 0;
    SSL_CTX* ctx_ = nullptr;
    std::thread thread_;
};

void sk_connections_are_pinned() {
    // The client trusts the system's CAs; here, the test root instead.
    const TempPath roots("sk_tls_roots.pem");
    leht::test::write_file(roots.str(), pki().ca.pem());
    ::setenv("SSL_CERT_FILE", roots.str().c_str(), 1);
    LocalTls server(pki());
    leht::crypto::detail::HttpRequest r;
    r.url = server.url();
    r.what = "the test service";
    r.timeout_seconds = 10;

    // No pins: the system's (here the test) CAs decide alone.
    CHECK(leht::crypto::detail::http11(r).status == 200);
    // The root's key pinned: the chain holds it.
    const std::vector<std::string> good{"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
                                        leht::crypto::detail::spki_pin(pki().ca.p)};
    r.pins = &good;
    const auto reply = leht::crypto::detail::http11(r);
    CHECK(reply.status == 200 && std::string(reply.body.begin(), reply.body.end()) == "{\"ok\":true}");
    // SK's pins: a valid chain from another CA is refused before anything is sent.
    const std::vector<std::string> sk = leht::crypto::SmartIdService::demo().pins;
    CHECK(sk.size() == 2);
    r.pins = &sk;
    CHECK(says(error_of([&] { (void)leht::crypto::detail::http11(r); }),
               "does not expect for it, so nothing was sent"));
    // With pins, plain http is for this machine only.
    r.url = "http://sid.demo.sk.ee/smart-id-rp/v3/session/x";
    CHECK(says(error_of([&] { (void)leht::crypto::detail::http11(r); }), "must be reached over https"));
    ::unsetenv("SSL_CERT_FILE");

    // A server that hangs up while the request is still being sent: an
    // error, not SIGPIPE killing the process (the viewer, the CLI).
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
              ::listen(fd, 1) == 0 &&
              ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        std::thread rude([fd] {
            for (int i = 0; i < 2; ++i) {  // http11, then OpenSSL's own client
                const int c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
                // Close before a byte arrives (a clean FIN): the client's
                // first write goes out, the next meets a closed socket.
                ::close(c);
            }
        });
        const Bytes big(8 << 20, 'x');
        leht::crypto::detail::HttpRequest post;
        post.url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/";
        post.what = "a server that hangs up";
        post.post = &big;
        post.content_type = "application/json";
        post.timeout_seconds = 10;
        CHECK(throws([&] { (void)leht::crypto::detail::http11(post); }));
        CHECK(throws([&] { (void)leht::crypto::detail::http_transfer(post); }));
        rude.join();
        ::close(fd);
        sigset_t pending;
        sigpending(&pending);
        CHECK(sigismember(&pending, SIGPIPE) == 0);  // none left behind either
    }

    // The pins are what they claim: SHA-256 of a key, in Base64.
    for (const std::string& pin : sk) {
        CHECK(leht::test::unb64(pin).size() == 32);
    }
}

// --- real PDFs -------------------------------------------------------------------

/// Prepares `in` for `id`, signs it, and checks it with our code, qpdf and pdfsig.
void sign_a_pdf(const Identity& id, const std::string& out, const std::string& name) {
    const leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, leht::test::corpus("text_10p.pdf"));
    leht::ops::SignatureRequest req;
    req.name = name;
    req.reserve = leht::crypto::estimate_signature_size(id, {});
    const int fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    const auto prepared = leht::ops::prepare_signature(ctx, doc, req, fd);
    (void)leht::crypto::sign_prepared(fd, prepared.range, id, {});
    ::close(fd);

    leht::Document back = leht::Document::open(ctx, out);
    const auto sigs = leht::ops::list_signatures(ctx, back);
    CHECK(sigs.size() == 1 && sigs[0].range_ok);
    const auto report = leht::crypto::verify_cms(
        sigs[0].contents, leht::ops::signed_bytes(ctx, back, sigs[0].range), pki().trust());
    CHECK(report.intact() && report.trust == Trust::Trusted);
    CHECK(leht::test::qpdf_check(out));
    if (leht::test::have_tool("pdfsig")) {
        const std::string said = leht::test::capture(
            "SOFTHSM2_CONF=/nonexistent/softhsm2.conf pdfsig '" + out + "' 2>&1");
        CHECK(says(said, "Signature is Valid"));
    }
}

void phone_signatures_are_valid_pdf_signatures() {
    LocalSmartId sid(pki());
    Person person;
    person.phone = &sid;
    const TempPath a("sk_pdf_smartid.pdf");
    sign_a_pdf(Identity::from_smart_id_qr(sid.service(), person.dialog()), a.str(), "Mari Maasikas");

    LocalMobileId mid(pki(), true);
    const TempPath b("sk_pdf_mobileid.pdf");
    sign_a_pdf(Identity::from_mobile_id(mid.service(), LocalMobileId::kPhone, LocalMobileId::kId,
                                        person.dialog()),
               b.str(), "Mary Änn");
}

// --- the CLI ----------------------------------------------------------------------

#ifdef LEHT_CLI

/// `leht` running with its output (stdout and stderr) readable line by line
/// as it comes, so the test can act on it: scan a QR link, press Ctrl-C.
struct Cli {
    pid_t pid = -1;
    FILE* out = nullptr;
    std::string all;

    Cli(const std::vector<std::string>& argv, const std::vector<std::string>& env) {
        int fds[2];
        CHECK(::pipe(fds) == 0);
        pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            ::dup2(fds[1], 1);
            ::dup2(fds[1], 2);
            ::close(fds[0]);
            ::close(fds[1]);
            for (const std::string& e : env) {
                ::putenv(const_cast<char*>(e.c_str()));
            }
            std::vector<char*> args;
            args.push_back(const_cast<char*>(LEHT_CLI));
            for (const std::string& a : argv) {
                args.push_back(const_cast<char*>(a.c_str()));
            }
            args.push_back(nullptr);
            ::execv(LEHT_CLI, args.data());
            ::_exit(127);
        }
        ::close(fds[1]);
        out = ::fdopen(fds[0], "r");
    }

    /// The next line, or false at the end.
    bool line(std::string* l) {
        char buf[8192];
        if (std::fgets(buf, sizeof(buf), out) == nullptr) {
            return false;
        }
        *l = buf;
        all += *l;
        return true;
    }

    /// Reads to the end; the exit status.
    int wait() {
        std::string l;
        while (line(&l)) {
        }
        std::fclose(out);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
};

std::string cli_env(const char* name, const std::string& value) {
    return std::string(name) + "=" + value;
}

void the_cli_signs_with_smart_id_by_qr() {
    LocalSmartId sid(pki());
    const TempPath out("sk_cli_qr.pdf");
    const TempPath ca("sk_cli_ca.pem");
    leht::test::write_file(ca.str(), pki().ca.pem());
    Cli cli({"sign", leht::test::corpus("text_10p.pdf"), "-o", out.str(), "--smart-id", "qr"},
            {cli_env("LEHT_SMARTID_URL", sid.service().base_url), "LANG=et_EE.UTF-8"});
    std::string l;
    bool scanned = false;
    while (cli.line(&l)) {
        const std::string tag = "leht: Smart-ID link: ";
        if (!scanned && l.rfind(tag, 0) == 0) {
            // Not a terminal: the link comes as text; the phone scans it.
            scanned = sid.scan(l.substr(tag.size(), l.size() - tag.size() - 1));
            CHECK(scanned);
        }
    }
    CHECK(cli.wait() == 0);
    CHECK(says(cli.all, "signer:    CN=MAASIKAS\\,MARI\\,PNOEE-40504040001"));
    CHECK(says(cli.all, "never the document"));
    CHECK(sid.last_display_text() == "Sign text_10p.pdf");
    CHECK(sid.complaints.none());
    const std::string verified =
        leht::test::capture(std::string(LEHT_CLI) + " verify '" + out.str() + "' --trust '" +
                            ca.str() + "' --no-trusted-list 2>&1; echo \"exit=$?\"");
    CHECK(says(verified, "exit=0"));
}

void the_cli_signs_with_smart_id_and_mobile_id_by_code() {
    LocalSmartId sid(pki());
    const TempPath a("sk_cli_code.pdf");
    Cli one({"sign", leht::test::corpus("text_10p.pdf"), "-o", a.str(), "--smart-id",
             "ee:40504040001"},
            {cli_env("LEHT_SMARTID_URL", sid.service().base_url)});
    CHECK(one.wait() == 0);
    CHECK(says(one.all, "Verification code: " + sid.last_vc()));
    CHECK(sid.complaints.none());

    LocalMobileId mid(pki(), true);
    const TempPath b("sk_cli_mid.pdf");
    Cli two({"sign", leht::test::corpus("text_10p.pdf"), "-o", b.str(), "--mobile-id",
             std::string(LocalMobileId::kPhone) + ":" + LocalMobileId::kId},
            {cli_env("LEHT_MOBILEID_URL", mid.service().base_url), "LANG=et_EE.UTF-8"});
    CHECK(two.wait() == 0);
    CHECK(says(two.all, "Verification code: " + sk::mobile_id_code(mid.last_hash())));
    CHECK(mid.last_request().value("language", "") == "EST");
    CHECK(mid.complaints.none());

    // Refused on the phone: exit 3, and no file.
    mid.outcome = LocalMobileId::Outcome::UserCancelled;
    const TempPath c("sk_cli_refused.pdf");
    Cli three({"sign", leht::test::corpus("text_10p.pdf"), "-o", c.str(), "--mobile-id",
               std::string(LocalMobileId::kPhone) + ":" + LocalMobileId::kId},
              {cli_env("LEHT_MOBILEID_URL", mid.service().base_url)});
    CHECK(three.wait() == 3);
    CHECK(says(three.all, "declined on your phone"));
    CHECK(!std::filesystem::exists(c.str()));

    // Two ways to sign at once, or none: refused before anything happens.
    Cli four({"sign", leht::test::corpus("text_10p.pdf"), "-o", c.str(), "--mobile-id", "x:y",
              "--smart-id", "qr"},
             {});
    CHECK(four.wait() == 1);
    CHECK(says(four.all, "needs one of"));
}

void ctrl_c_cancels_and_writes_nothing() {
    LocalMobileId mid(pki(), true);
    mid.polls_before_done = 100000;  // the phone never answers
    const TempPath out("sk_cli_interrupted.pdf");
    Cli cli({"sign", leht::test::corpus("text_10p.pdf"), "-o", out.str(), "--mobile-id",
             std::string(LocalMobileId::kPhone) + ":" + LocalMobileId::kId},
            {cli_env("LEHT_MOBILEID_URL", mid.service().base_url)});
    std::string l;
    while (cli.line(&l)) {
        if (l.rfind("Verification code:", 0) == 0) {
            // Give it a moment to be waiting on the phone, then Ctrl-C.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            ::kill(cli.pid, SIGINT);
            break;
        }
    }
    CHECK(cli.wait() == 3);
    CHECK(says(cli.all, "cancelled"));
    CHECK(!std::filesystem::exists(out.str()));
    // Nor the temporary file beside it.
    for (const auto& e : std::filesystem::directory_iterator(
             std::filesystem::path(out.str()).parent_path())) {
        CHECK(e.path().filename().string().find("sk_cli_interrupted.pdf.leht-") ==
              std::string::npos);
    }
}

#endif

// --- SK's real demo environment, by hand only ---------------------------------------

void sks_demo_answers_when_asked() {
    if (std::getenv("LEHT_SK_DEMO") == nullptr) {
        std::fprintf(stderr, "  SKIP SK demo (set LEHT_SK_DEMO=1 to talk to SK's DEMO)\n");
        return;
    }
    leht::crypto::init();
    Person person;
    person.statuses.clear();
    // Mobile-ID's demo numbers answer by themselves after a few seconds.
    const Identity mid = Identity::from_mobile_id(leht::crypto::MobileIdService::demo(),
                                                  "+37268000769", "60001017869",
                                                  person.dialog("Leht test"));
    std::fprintf(stderr, "  Mobile-ID demo: %s\n", mid.certificate().subject.c_str());
    const Prepared a("sk_demo_mid.bin", leht::crypto::estimate_signature_size(mid, {}));
    (void)leht::test::sign(a, mid);
    std::fprintf(stderr, "  Mobile-ID demo signed, code %s\n", person.code.c_str());
    CHECK(a.verify(leht::crypto::TrustStore{}).intact());

    // Smart-ID: SK's MOCK demo accounts finish a certificate choice by
    // themselves, but (September 2026) no longer a v3 signature -- that needs
    // SK's demo app on a real phone. So: the certificate choice, with the
    // intermediate from the certificate's caIssuers URL, and a QR session.
    const Identity sid = Identity::from_smart_id(leht::crypto::SmartIdService::demo(),
                                                 "PNOEE-61101019999", person.dialog("Leht test"));
    std::fprintf(stderr, "  Smart-ID demo: %s (%zu certificates)\n",
                 sid.certificate().subject.c_str(), sid.chain_der().size());
    CHECK(sid.on_phone() && sid.key_type() == leht::crypto::KeyType::Rsa);
    CHECK(sid.chain_der().size() == 2);
    Person nobody;
    nobody.cancel_after_polls = 3;
    CHECK(cancelled([&] {
        (void)Identity::from_smart_id_qr(leht::crypto::SmartIdService::demo(), nobody.dialog());
    }));
    CHECK(nobody.qr_shown >= 2);
}

}  // namespace

int main() {
    leht::crypto::init();
    RUN(qr_links_match_sks_examples);
    RUN(mobile_id_codes_match_sks_example);
    RUN(base64_round_trips_and_refuses_junk);
    RUN(hostile_session_json_is_refused_not_crashed);
    RUN(a_qr_code_holds_a_whole_link);
    RUN(smart_id_by_qr_signs_with_rsa_pss);
    RUN(openssl_verifies_the_pss_signature);
    RUN(smart_id_by_personal_code_shows_sks_code);
    RUN(smart_id_with_a_timestamp_is_b_t);
    RUN(smart_id_refusals_and_failures_write_nothing);
    RUN(smart_id_errors_speak_to_the_person);
    RUN(cancel_stops_waiting_for_the_qr_scan);
    RUN(cancel_while_the_phone_signs_writes_nothing);
    RUN(mobile_id_signs_with_ecdsa_and_rsa);
    RUN(mobile_id_failures_speak_to_the_person);
    RUN(sk_connections_are_pinned);
    RUN(phone_signatures_are_valid_pdf_signatures);
#ifdef LEHT_CLI
    RUN(the_cli_signs_with_smart_id_by_qr);
    RUN(the_cli_signs_with_smart_id_and_mobile_id_by_code);
    RUN(ctrl_c_cancels_and_writes_nothing);
#endif
    RUN(sks_demo_answers_when_asked);
    return 0;
}
