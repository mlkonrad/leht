// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Signing with a key that stays on a PKCS#11 token. SoftHSM2 stands in for
// the ID card; everything here is what a card does too, except the reader.
#include "leht/context.hpp"
#include "leht/crypto/crypto.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/sign.hpp"
#include "edit_harness.hpp"
#include "softhsm.hpp"
#include "test_pki.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <string>

using leht::Context;
using leht::Document;
using leht::crypto::Identity;
using leht::crypto::Secret;
using leht::crypto::SignOptions;
using leht::crypto::TokenKey;
using leht::crypto::Trust;
using leht::ops::SignatureInfo;
using leht::test::SoftHsm;
using leht::test::TempPath;
using leht::test::corpus;

namespace {

const leht::test::Pki& pki() {
    static const leht::test::Pki p;
    return p;
}

/// A signing key and an authentication key, as an ID card has, plus an EC
/// signing key that wants its PIN for every signature. Added auth-first, so
/// the listing's order is the code's doing, not the token's.
struct Card {
    leht::test::Key auth_key = leht::test::rsa_key();
    leht::test::Cert auth_cert =
        leht::test::issue(auth_key, {"Mari Maasikas (auth)", false, -1, 365,
                                     "critical,digitalSignature,keyAgreement"},
                          &pki().ca, &pki().ca_key);
    SoftHsm hsm;

    Card() {
        hsm.add(auth_key, auth_cert, "\x01", "Authentication");
        hsm.add(pki().rsa, pki().rsa_cert, "\x02", "Signature");
        hsm.add(pki().ec, pki().ec_cert, "\x03", "Signature EC", true);
    }

    [[nodiscard]] std::vector<TokenKey> keys() const {
        return leht::crypto::list_token_keys(SoftHsm::module());
    }
    [[nodiscard]] TokenKey key(const std::string& cn) const {
        for (const TokenKey& k : keys()) {
            if (k.cert.common_name == cn) {
                return k;
            }
        }
        CHECK(!"no such key on the card");
        return {};
    }
    [[nodiscard]] Identity identity(const std::string& cn, const std::string& pin = "1234") const {
        return Identity::from_pkcs11(
            key(cn).uri, [pin](const TokenKey&) { return Secret{pin}; }, SoftHsm::module());
    }
};

template <typename F>
std::string error_of(F&& f) {
    try {
        f();
    } catch (const leht::Error& e) {
        return e.what();
    }
    return {};
}

/// Prepares `in` for a signature into `out` and signs it with `id`, as the CLI does.
leht::crypto::SignResult sign_file(const Identity& id, const std::string& in,
                                   const std::string& out, const SignOptions& options = {}) {
    const Context ctx;
    Document doc = Document::open(ctx, in);
    leht::ops::SignatureRequest req;
    req.reserve = leht::crypto::estimate_signature_size(id, options);
    const int fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    try {
        const auto prepared = leht::ops::prepare_signature(ctx, doc, req, fd);
        const auto result = leht::crypto::sign_prepared(fd, prepared.range, id, options);
        ::close(fd);
        return result;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

leht::crypto::CmsReport verify(const std::string& path) {
    const Context ctx;
    Document doc = Document::open(ctx, path);
    auto sigs = leht::ops::list_signatures(ctx, doc);
    CHECK(sigs.size() == 1);
    CHECK(sigs.front().range_ok);
    return leht::crypto::verify_cms(sigs.front().contents,
                                    leht::ops::signed_bytes(ctx, doc, sigs.front().range),
                                    pki().trust());
}

bool pdfsig_says_valid(const std::string& path) {
    if (!leht::test::have_tool("pdfsig")) {
        std::fprintf(stderr, "  SKIP pdfsig (poppler-utils not installed)\n");
        return true;
    }
    // SoftHSM kept out: poppler's NSS loads every module p11-kit has
    // registered, SoftHSM's among them, and once SoftHSM starts -- a token in
    // reach, or none, as root -- pdfsig crashes (exit 139) before printing a
    // word. A config that does not exist stops SoftHSM from starting.
    const std::string out = leht::test::capture(
        "SOFTHSM2_CONF=/nonexistent/softhsm2.conf pdfsig '" + path + "' 2>&1");
    if (out.find("Signature is Valid") == std::string::npos) {
        std::fprintf(stderr, "pdfsig said:\n%s\n", out.c_str());
        return false;
    }
    return true;
}

void the_card_lists_its_keys_signing_key_first() {
    const Card card;
    const auto keys = card.keys();
    CHECK(keys.size() == 3);
    CHECK(keys[0].non_repudiation && keys[1].non_repudiation);
    CHECK(!keys[2].non_repudiation);
    CHECK(keys[2].cert.common_name == "Mari Maasikas (auth)");
    for (const TokenKey& k : keys) {
        CHECK(k.uri.rfind("pkcs11:", 0) == 0);
        CHECK(k.uri.find("type=private") != std::string::npos);
        CHECK(k.uri.find("pin-value") == std::string::npos);
        CHECK(k.token_label == card.hsm.label());
        CHECK(!k.pinpad && !k.pin_locked);
    }
}

void an_rsa_key_on_the_card_signs_a_pdf() {
    const Card card;
    const Identity id = card.identity("Mari Maasikas");
    CHECK(id.on_token());
    CHECK(id.key_type() == leht::crypto::KeyType::Rsa);
    CHECK(id.certificate().common_name == "Mari Maasikas");

    const TempPath out("pkcs11_rsa.pdf");
    const auto r = sign_file(id, corpus("text_10p.pdf"), out.str());
    CHECK(r.digest == "SHA-256");
    const auto v = verify(out.str());
    CHECK(v.intact());
    CHECK(v.trust == Trust::Trusted);
    CHECK(v.signer.common_name == "Mari Maasikas");
    CHECK(v.has_signing_certificate_v2);
    CHECK(!v.has_signing_time_attribute);
    CHECK(leht::test::qpdf_check(out.str()));
    CHECK(pdfsig_says_valid(out.str()));
}

void a_p384_key_that_wants_its_pin_every_time_signs() {
    const Card card;
    const Identity id = card.identity("Jaan Tamm");
    CHECK(id.key_type() == leht::crypto::KeyType::Ec);
    const TempPath out("pkcs11_ec.pdf");
    const auto r = sign_file(id, corpus("text_10p.pdf"), out.str());
    CHECK(r.digest == "SHA-384");
    const auto v = verify(out.str());
    CHECK(v.intact());
    CHECK(v.signer.common_name == "Jaan Tamm");
    CHECK(pdfsig_says_valid(out.str()));

    // And one Identity signs more than once: the per-signature login repeats.
    const TempPath again("pkcs11_ec_again.pdf");
    (void)sign_file(id, corpus("outlined.pdf"), again.str());
    CHECK(verify(again.str()).intact());
}

void the_token_really_demands_the_pin_again() {
    // Without this, the test above would pass whether or not Leht logs in per
    // signature. Ask SoftHSM directly, skipping that login.
    const Card card;
    CK_FUNCTION_LIST* m = p11_kit_module_load(SoftHsm::module().c_str(), 0);
    CHECK(m != nullptr && p11_kit_module_initialize(m) == CKR_OK);
    const CK_SESSION_HANDLE s = card.hsm.open(m);
    CK_ULONG n = 0;
    CK_OBJECT_CLASS cls = CKO_PRIVATE_KEY;
    char id = '\x03';
    CK_ATTRIBUTE t[] = {{CKA_CLASS, &cls, sizeof cls}, {CKA_ID, &id, 1}};
    CK_OBJECT_HANDLE key = 0;
    CHECK(m->C_FindObjectsInit(s, t, 2) == CKR_OK);
    CHECK(m->C_FindObjects(s, &key, 1, &n) == CKR_OK && n == 1);
    m->C_FindObjectsFinal(s);
    CK_MECHANISM mech{CKM_ECDSA, nullptr, 0};
    CK_BYTE hash[48] = {1};
    CK_BYTE sig[256];
    CK_ULONG len = sizeof sig;
    CHECK(m->C_SignInit(s, &mech, key) == CKR_OK);
    const CK_RV rv = m->C_Sign(s, hash, sizeof hash, sig, &len);
    CHECK(rv == CKR_USER_NOT_LOGGED_IN);
    m->C_CloseSession(s);
    p11_kit_module_finalize(m);
    p11_kit_module_release(m);
}

void a_wrong_pin_says_so_and_signs_nothing() {
    const Card card;
    const std::string e = error_of([&] { (void)card.identity("Mari Maasikas", "9999"); });
    CHECK(e.rfind("The PIN is wrong.", 0) == 0);
    // The right PIN still works afterwards.
    (void)card.identity("Mari Maasikas");
}

void the_pin_prompt_knows_the_token_and_the_signer() {
    const Card card;
    TokenKey seen;
    (void)Identity::from_pkcs11(
        card.key("Jaan Tamm").uri,
        [&](const TokenKey& k) {
            seen = k;
            return Secret{SoftHsm::kPin};
        },
        SoftHsm::module());
    CHECK(seen.token_label == card.hsm.label());
    CHECK(seen.cert.common_name == "Jaan Tamm");
    CHECK(!seen.pinpad);

    // A prompt that throws cancels, before any login.
    const std::string e = error_of([&] {
        (void)Identity::from_pkcs11(
            card.key("Jaan Tamm").uri,
            [](const TokenKey&) -> Secret { throw leht::Error(0, "cancelled"); },
            SoftHsm::module());
    });
    CHECK(e == "cancelled");
}

void a_uri_may_not_carry_a_pin() {
    const Card card;
    const std::string uri = card.key("Mari Maasikas").uri + "?pin-value=1234";
    const std::string e = error_of([&] {
        (void)Identity::from_pkcs11(uri, [](const TokenKey&) { return Secret{}; },
                                    SoftHsm::module());
    });
    CHECK(e.find("must not carry the PIN") != std::string::npos);
    CHECK(!error_of([&] {
               (void)Identity::from_pkcs11("not a uri", [](const TokenKey&) { return Secret{}; },
                                           SoftHsm::module());
           }).empty());
}

void no_card_is_a_clear_error() {
    const Card card;
    const std::string e = error_of([&] {
        (void)Identity::from_pkcs11("pkcs11:token=No%20Such%20Card;type=private",
                                    [](const TokenKey&) { return Secret{"1234"}; },
                                    SoftHsm::module());
    });
    CHECK(e.find("Is the card in the reader?") != std::string::npos);
}

void a_certificate_for_another_key_is_caught_before_writing() {
    // The token's key and the certificate beside it disagree. The signature
    // would verify against nothing; the self-check refuses to write it.
    SoftHsm hsm;
    hsm.add(pki().expired, pki().rsa_cert, "\x07", "Mismatched");
    const auto keys = leht::crypto::list_token_keys(SoftHsm::module());
    CHECK(keys.size() == 1);
    const Identity id = Identity::from_pkcs11(
        keys.front().uri, [](const TokenKey&) { return Secret{SoftHsm::kPin}; },
        SoftHsm::module());
    const TempPath out("pkcs11_mismatch.pdf");
    const std::string e =
        error_of([&] { (void)sign_file(id, corpus("text_10p.pdf"), out.str()); });
    CHECK(e.find("not the certificate's key") != std::string::npos);
    // The hole is still empty: nothing was signed into the file.
    const Context ctx;
    Document doc = Document::open(ctx, out.str());
    const auto sigs = leht::ops::list_signatures(ctx, doc);
    CHECK(sigs.size() == 1);
    for (const std::uint8_t b : sigs.front().contents) {
        CHECK(b == 0);
    }
}

void a_timestamped_card_signature_is_b_t() {
    const Card card;
    leht::test::LocalTsa tsa(pki().tsa_key, pki().tsa_cert, pki().ca);
    SignOptions options;
    options.tsa_url = tsa.url();
    const Identity id = card.identity("Jaan Tamm");
    const TempPath out("pkcs11_bt.pdf");
    const auto r = sign_file(id, corpus("text_10p.pdf"), out.str(), options);
    CHECK(r.timestamp.has_value());
    const auto v = verify(out.str());
    CHECK(v.intact());
    CHECK(v.timestamp && v.timestamp->valid);
}

#ifdef LEHT_CLI
/// Runs the leht binary through the shell; returns its output and exit code.
std::pair<std::string, int> leht(const std::string& args) {
    const std::string out = leht::test::capture(std::string("'") + LEHT_CLI + "' " + args +
                                                " 2>&1; echo \"EXIT=$?\"");
    const std::size_t at = out.rfind("EXIT=");
    CHECK(at != std::string::npos);
    return {out.substr(0, at), std::stoi(out.substr(at + 5))};
}

void the_cli_lists_and_signs_with_card_keys() {
    const Card card;
    const std::string module = " --pkcs11-module '" + SoftHsm::module() + "'";
    const auto [keys, keys_rc] = leht("keys" + module);
    CHECK(keys_rc == 0);
    CHECK(keys.find("pkcs11:") != std::string::npos);
    CHECK(keys.find("signing (nonRepudiation)") != std::string::npos);
    CHECK(keys.find("authentication, not signing") != std::string::npos);

    const TempPath out("pkcs11_cli.pdf");
    const std::string sign = "sign '" + corpus("text_10p.pdf") + "' -o '" + out.str() + "'" +
                             module + " --password-fd 0";
    // Two keys could sign here, so auto will not guess.
    const auto [ambiguous, ambiguous_rc] = leht(sign + " --pkcs11 auto < /dev/null");
    CHECK(ambiguous_rc == 1);
    CHECK(ambiguous.find("more than one key could sign") != std::string::npos);

    const std::string uri = " --pkcs11 '" + card.key("Jaan Tamm").uri + "'";
    const TempPath wrong_pin("pkcs11_cli_wrong_pin");
    const TempPath right_pin("pkcs11_cli_right_pin");
    leht::test::write_file(wrong_pin.str(), "9999\n");
    leht::test::write_file(right_pin.str(), std::string(SoftHsm::kPin) + "\n");

    const auto [wrong, wrong_rc] = leht(sign + uri + " < '" + wrong_pin.str() + "'");
    CHECK(wrong_rc == 1);
    CHECK(wrong.find("The PIN is wrong.") != std::string::npos);
    CHECK(!std::filesystem::exists(out.str()));

    const auto [signed_, signed_rc] = leht(sign + uri + " < '" + right_pin.str() + "'");
    if (signed_rc != 0) {
        std::fprintf(stderr, "%s\n", signed_.c_str());
    }
    CHECK(signed_rc == 0);
    CHECK(signed_.find("Jaan Tamm") != std::string::npos);
    CHECK(signed_.find("SHA-384") != std::string::npos);
    CHECK(verify(out.str()).intact());
    CHECK(pdfsig_says_valid(out.str()));
}
#endif

}  // namespace

int main() {
    if (!SoftHsm::available()) {
        std::printf("SKIP: SoftHSM2 is not installed (dnf install softhsm)\n");
        return 77;
    }
    RUN(the_card_lists_its_keys_signing_key_first);
    RUN(an_rsa_key_on_the_card_signs_a_pdf);
    RUN(a_p384_key_that_wants_its_pin_every_time_signs);
    RUN(the_token_really_demands_the_pin_again);
    RUN(a_wrong_pin_says_so_and_signs_nothing);
    RUN(the_pin_prompt_knows_the_token_and_the_signer);
    RUN(a_uri_may_not_carry_a_pin);
    RUN(no_card_is_a_clear_error);
    RUN(a_certificate_for_another_key_is_caught_before_writing);
    RUN(a_timestamped_card_signature_is_b_t);
#ifdef LEHT_CLI
    RUN(the_cli_lists_and_signs_with_card_keys);
#endif
    return 0;
}
