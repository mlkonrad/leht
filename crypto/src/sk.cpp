// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Smart-ID (RP API v3) and Mobile-ID (MID REST), SK ID Solutions' services
// for signing with a phone. Trusted process only, like every other network
// call in Leht.
//
// What leaves the machine: the relying party's name and UUID, the person's
// identifier (their personal code, or phone number and personal code), and
// the digest of the signed attributes. Never the document. What comes back is
// JSON from SK, parsed in one place (parse_*_session) and treated as hostile:
// the certificate is parsed with OpenSSL, and the signature value is checked
// against it by sign.cpp's self_check before a byte is written.
//
// The flows, in SK's terms:
//   * Smart-ID QR: an anonymous device-link certificate-choice session, shown
//     as a QR code renewed every second (its authCode is an HMAC keyed with
//     the session secret, so the link cannot be made ahead or by anyone
//     else); then a notification signature session LINKED to it, which the
//     phone expects -- no verification code to compare.
//   * Smart-ID by personal code: a notification certificate-choice session
//     for the ETSI semantics identifier, then a notification signature
//     session whose verification code SK returns and the person compares.
//   * Mobile-ID: the certificate for a phone number and personal code, then a
//     signature session; the verification code is computed here from the hash.
#include "ossl.hpp"
#include "sk.hpp"

#include "leht/error.hpp"

#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdlib>
#include <regex>

namespace leht::crypto {

namespace detail {

std::string base64(const Bytes& data) {
    std::string out(4 * ((data.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data.data(),
                                  static_cast<int>(data.size()));
    out.resize(static_cast<std::size_t>(n < 0 ? 0 : n));
    return out;
}

Bytes unbase64(const std::string& text) {
    static const std::regex shape{"^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$"};
    if (!std::regex_match(text, shape)) {
        throw Error(0, "not Base64");
    }
    Bytes out(text.size() / 4 * 3 + 3);
    const int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(text.data()),
                                  static_cast<int>(text.size()));
    if (n < 0) {
        throw Error(0, "not Base64");
    }
    // EVP_DecodeBlock counts the padding as zero bytes.
    std::size_t len = static_cast<std::size_t>(n);
    for (auto it = text.rbegin(); it != text.rend() && *it == '='; ++it) {
        --len;
    }
    out.resize(len);
    return out;
}

}  // namespace detail

namespace sk {

using json = nlohmann::json;

std::string auth_code(const Bytes& session_secret, const std::string& payload) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (HMAC(EVP_sha256(), session_secret.data(), static_cast<int>(session_secret.size()),
             reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), mac,
             &len) == nullptr) {
        detail::fail("cannot compute the Smart-ID authCode");
    }
    std::string out = detail::base64(Bytes(mac, mac + len));
    for (char& c : out) {
        c = c == '+' ? '-' : c == '/' ? '_' : c;
    }
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

std::string qr_link(const LinkParts& p, const Bytes& session_secret) {
    const std::string link = p.device_link_base + "?deviceLinkType=QR&elapsedSeconds=" +
                             std::to_string(p.elapsed_seconds) +
                             "&sessionToken=" + p.session_token +
                             "&sessionType=" + p.session_type + "&version=1.0&lang=" + p.lang;
    auto b64 = [](const std::string& s) {
        return detail::base64(Bytes(s.begin(), s.end()));
    };
    // The QR flow has no callback URL: its field is there, empty.
    const std::string payload = p.scheme_name + "|" + p.protocol + "|" + p.digest + "|" +
                                b64(p.relying_party_name) + "|" + b64(p.brokered_rp_name) + "|" +
                                p.interactions + "||" + link;
    return link + "&authCode=" + auth_code(session_secret, payload);
}

std::string mobile_id_code(const Bytes& hash) {
    if (hash.empty()) {
        return "0000";
    }
    const unsigned code = (static_cast<unsigned>(hash.front() >> 2) << 7) |
                          static_cast<unsigned>(hash.back() & 0x7F);
    std::string out = std::to_string(code);
    return std::string(4 - out.size(), '0') + out;
}

namespace {

json parse(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        throw Error(0, "the reply is not a JSON object");
    }
    return j;
}

/// A string member, or empty when absent; not a string is an error.
std::string str(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return {};
    }
    if (!it->is_string()) {
        throw Error(0, std::string("\"") + key + "\" is not a string");
    }
    return it->get<std::string>();
}

const json* obj(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return nullptr;
    }
    if (!it->is_object()) {
        throw Error(0, std::string("\"") + key + "\" is not an object");
    }
    return &*it;
}

Bytes b64(const json& j, const char* key) {
    const std::string s = str(j, key);
    return s.empty() ? Bytes{} : detail::unbase64(s);
}

}  // namespace

SmartIdSession parse_smart_id_session(const std::string& text) {
    const json j = parse(text);
    SmartIdSession out;
    const std::string state = str(j, "state");
    if (state != "RUNNING" && state != "COMPLETE") {
        throw Error(0, "the session state is \"" + state + "\"");
    }
    out.complete = state == "COMPLETE";
    if (const json* result = obj(j, "result")) {
        out.end_result = str(*result, "endResult");
        out.document_number = str(*result, "documentNumber");
    }
    if (const json* cert = obj(j, "cert")) {
        out.cert = b64(*cert, "value");
        out.cert_level = str(*cert, "certificateLevel");
    }
    if (const json* sig = obj(j, "signature")) {
        out.signature = b64(*sig, "value");
        out.signature_algorithm = str(*sig, "signatureAlgorithm");
        out.flow_type = str(*sig, "flowType");
        if (const json* params = obj(*sig, "signatureAlgorithmParameters")) {
            out.hash_algorithm = str(*params, "hashAlgorithm");
            if (const json* mgf = obj(*params, "maskGenAlgorithm")) {
                if (const json* mp = obj(*mgf, "parameters")) {
                    out.mgf_hash_algorithm = str(*mp, "hashAlgorithm");
                }
            }
            const auto salt = params->find("saltLength");
            if (salt != params->end()) {
                if (!salt->is_number_integer()) {
                    throw Error(0, "\"saltLength\" is not a number");
                }
                out.salt_length = salt->get<int>();
            }
        }
    }
    out.interaction_used = str(j, "interactionTypeUsed");
    if (out.complete && out.end_result.empty()) {
        throw Error(0, "a finished session without an end result");
    }
    return out;
}

MobileIdSession parse_mobile_id_session(const std::string& text) {
    const json j = parse(text);
    MobileIdSession out;
    const std::string state = str(j, "state");
    if (state != "RUNNING" && state != "COMPLETE") {
        throw Error(0, "the session state is \"" + state + "\"");
    }
    out.complete = state == "COMPLETE";
    out.result = str(j, "result");
    if (const json* sig = obj(j, "signature")) {
        out.signature = b64(*sig, "value");
        out.algorithm = str(*sig, "algorithm");
    }
    out.cert = b64(j, "cert");
    if (out.complete && out.result.empty()) {
        throw Error(0, "a finished session without a result");
    }
    return out;
}

}  // namespace sk

namespace {

using json = nlohmann::json;
using detail::fail;

/// SK's long polls wait this long at most, so Cancel and the QR code's
/// renewal are never more than about a second away.
constexpr int kPollMs = 1000;
/// Both services end a session on their own within a few minutes; this is
/// only a backstop against a server that keeps saying RUNNING.
constexpr int kMaxWaitSeconds = 360;

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? std::string(v) : std::string(fallback);
}

/// What an HTTP error status means, per service.
using StatusText = std::string (*)(int status);

std::string smart_id_status(int status) {
    switch (status) {
        case 400: return "Smart-ID refused the request as malformed";
        case 401: return "Smart-ID does not know this relying party: check its name and UUID";
        case 403: return "this relying party is not allowed to do that with Smart-ID";
        case 404: return "Smart-ID has no account for this person (or the session has ended)";
        case 471: return "this Smart-ID account cannot give a qualified signature";
        case 472: return "Smart-ID asks you to open the Smart-ID app, or its self-service "
                         "portal, first";
        case 480: return "Smart-ID no longer serves a client this old; update Leht";
        case 580: return "Smart-ID is down for maintenance; try again later";
        default: return "Smart-ID answered with HTTP status " + std::to_string(status);
    }
}

std::string mobile_id_status(int status) {
    switch (status) {
        case 400: return "Mobile-ID refused the request: check the phone number and personal code";
        case 401: return "Mobile-ID does not know this relying party: check its name and UUID";
        case 404: return "Mobile-ID has no such session";
        case 405: return "Mobile-ID refused the request method";
        case 500: return "Mobile-ID failed on its side; try again later";
        default: return "Mobile-ID answered with HTTP status " + std::to_string(status);
    }
}

/// A service's HTTP error messages, and the CA keys its TLS chain must hold.
struct Api {
    StatusText status_text;
    const std::vector<std::string>* pins;
};

/// One JSON exchange: POST `post` when given, else GET. The reply's body.
std::string exchange(const std::string& what, const std::string& url, const json* post,
                     int timeout_seconds, const Api& api) {
    detail::HttpRequest r;
    r.url = url;
    // Empty pins, only as a test sets them: the system's CAs alone decide.
    r.pins = api.pins != nullptr && !api.pins->empty() ? api.pins : nullptr;
    r.what = what;
    r.timeout_seconds = timeout_seconds;
    r.max_size = std::size_t{256} << 10;
    Bytes body;
    if (post != nullptr) {
        const std::string text = post->dump();
        body.assign(text.begin(), text.end());
        r.post = &body;
        r.content_type = "application/json";
    }
    // HTTP/1.1: Smart-ID's front end refuses the HTTP/1.0 OpenSSL's own
    // client speaks, and the status codes carry the message.
    const detail::HttpReply reply = detail::http11(r);
    if (reply.status != 200) {
        throw Error(0, api.status_text(reply.status));
    }
    return {reply.body.begin(), reply.body.end()};
}

void check_cancel(const PhoneDialog& d) {
    if (d.cancelled && d.cancelled()) {
        throw Cancelled("Signing was cancelled. Nothing was written.");
    }
}

void say(const PhoneDialog& d, const std::string& text) {
    if (d.status) {
        d.status(text);
    }
}

/// Long-polls `url` (a session) until it is complete: a second at a time,
/// with `tick` and the cancel check between. Returns the final body.
template <typename Session, typename Parse>
Session poll(const std::string& what, const std::string& url, const PhoneDialog& dialog,
             const Api& api, Parse parse, const std::function<void()>& tick = {}) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kMaxWaitSeconds);
    for (;;) {
        check_cancel(dialog);
        if (tick) {
            tick();
        }
        const std::string body = exchange(what, url + "?timeoutMs=" + std::to_string(kPollMs),
                                          nullptr, kPollMs / 1000 + 15, api);
        Session s;
        try {
            s = parse(body);
        } catch (const Error& e) {
            throw Error(0, what + " sent a reply Leht does not understand: " + e.what());
        }
        if (s.complete) {
            return s;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            throw Error(0, "The phone did not answer in time.");
        }
    }
}

/// A string for a URL path segment: SK's identifiers are letters, digits and
/// dashes, and nothing else may reach a URL.
std::string path_segment(const std::string& s, const char* what) {
    static const std::regex ok{"^[A-Za-z0-9-]{1,64}$"};
    if (!std::regex_match(s, ok)) {
        throw Error(0, std::string("not a valid ") + what + ": \"" + s + "\"");
    }
    return s;
}

/// The first `n` characters of UTF-8 `s` (not bytes).
std::string shorten(const std::string& s, std::size_t n) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (chars == n) {
                return s.substr(0, i);
            }
            ++chars;
        }
    }
    return s;
}

json party(const RelyingParty& p) {
    return {{"relyingPartyUUID", p.uuid}, {"relyingPartyName", p.name}};
}

std::string with_slash(std::string url) {
    if (url.empty() || url.back() != '/') {
        url += '/';
    }
    return url;
}

/// The issuer of `cert`, from its authorityInfoAccess caIssuers URL: SK
/// returns only the signer's certificate, and a verifier needs the
/// intermediate to reach a trusted root. Best effort: none on any failure.
detail::X509Ptr fetch_issuer(X509* cert) {
    using AiaPtr = detail::Ptr<AUTHORITY_INFO_ACCESS, AUTHORITY_INFO_ACCESS_free>;
    const AiaPtr aia{static_cast<AUTHORITY_INFO_ACCESS*>(
        X509_get_ext_d2i(cert, NID_info_access, nullptr, nullptr))};
    for (int i = 0; aia && i < sk_ACCESS_DESCRIPTION_num(aia.get()); ++i) {
        const ACCESS_DESCRIPTION* ad = sk_ACCESS_DESCRIPTION_value(aia.get(), i);
        if (OBJ_obj2nid(ad->method) != NID_ad_ca_issuers || ad->location->type != GEN_URI) {
            continue;
        }
        const ASN1_IA5STRING* uri = ad->location->d.uniformResourceIdentifier;
        const std::string url(reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri)),
                              static_cast<std::size_t>(ASN1_STRING_length(uri)));
        try {
            detail::HttpRequest r;
            r.url = url;
            r.what = "the certificate's issuer";
            r.max_size = std::size_t{64} << 10;
            r.timeout_seconds = 15;
            r.max_redirects = 1;
            const Bytes body = detail::http_transfer(r);
            const unsigned char* p = body.data();
            detail::X509Ptr issuer{d2i_X509(nullptr, &p, static_cast<long>(body.size()))};
            if (!issuer) {
                const detail::BioPtr bio{BIO_new_mem_buf(body.data(), static_cast<int>(body.size()))};
                issuer.reset(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
            }
            if (issuer && X509_check_issued(issuer.get(), cert) == X509_V_OK) {
                ERR_clear_error();
                return issuer;
            }
        } catch (const Error&) {
            // Signing still works; only a verifier without the intermediate
            // will call the chain incomplete.
        }
        ERR_clear_error();
    }
    ERR_clear_error();
    return {};
}

/// The signer's certificate as SK returned it: parsed, and fit for signing.
detail::X509Ptr signer_certificate(const Bytes& der, const char* service) {
    detail::X509Ptr cert;
    try {
        cert = detail::from_der(der);
    } catch (const Error&) {
        throw Error(0, std::string(service) + " returned something that is not a certificate");
    }
    const CertInfo info = detail::info(cert.get());
    if (!info.can_sign) {
        throw Error(0, std::string(service) + " returned a certificate that cannot sign");
    }
    const std::int64_t now = std::time(nullptr);
    if (now < info.not_before || now > info.not_after) {
        throw Error(0, std::string(service) + " returned a certificate that is not valid now");
    }
    return cert;
}

std::unique_ptr<Identity::Impl> phone_impl(detail::X509Ptr cert,
                                           std::shared_ptr<detail::Remote> remote) {
    auto impl = std::make_unique<Identity::Impl>();
    impl->key.reset(X509_get_pubkey(cert.get()));
    if (!impl->key) {
        fail("cannot read the certificate's public key");
    }
    if (detail::X509Ptr issuer = fetch_issuer(cert.get())) {
        impl->extra.push_back(std::move(issuer));
    }
    impl->cert = std::move(cert);
    impl->remote = std::move(remote);
    return impl;
}

// --- Smart-ID ---------------------------------------------------------------

constexpr const char* kSmartId = "Smart-ID";

/// Throws for every end result but OK, with what it means for the person.
void smart_id_outcome(const sk::SmartIdSession& s) {
    const std::string& r = s.end_result;
    if (r == "OK") {
        return;
    }
    if (r == "USER_REFUSED" || r == "USER_REFUSED_INTERACTION" || r == "USER_REFUSED_CERT_CHOICE") {
        throw Cancelled("You declined on your phone. Nothing was written.");
    }
    if (r == "TIMEOUT") {
        throw Error(0, "The phone did not answer in time.");
    }
    if (r == "WRONG_VC") {
        throw Error(0, "The wrong verification code was chosen on the phone.");
    }
    if (r == "DOCUMENT_UNUSABLE" || r == "ACCOUNT_UNUSABLE") {
        throw Error(0, "This Smart-ID account cannot be used right now; the Smart-ID app says why.");
    }
    if (r == "REQUIRED_INTERACTION_NOT_SUPPORTED_BY_APP") {
        throw Error(0, "The Smart-ID app is too old for this; update it.");
    }
    throw Error(0, "Smart-ID failed (" + r + "); try again.");
}

std::string interactions(const PhoneDialog& d) {
    // displayTextAndPIN: allowed in the linked flow and the notification one.
    const json list = json::array(
        {{{"type", "displayTextAndPIN"},
          {"displayText60", shorten(d.display_text.empty() ? "Sign a PDF" : d.display_text, 60)}}});
    const std::string text = list.dump();
    return detail::base64(Bytes(text.begin(), text.end()));
}

class SmartIdRemote final : public detail::Remote {
public:
    SmartIdRemote(SmartIdService service, PhoneDialog dialog, std::string document,
                  std::string linked_session, Bytes cert_der)
        : service_(std::move(service)), dialog_(std::move(dialog)), document_(std::move(document)),
          linked_(std::move(linked_session)), cert_der_(std::move(cert_der)) {}

    [[nodiscard]] const EVP_MD* digest() const override { return EVP_sha256(); }
    [[nodiscard]] bool pss() const override { return true; }
    [[nodiscard]] Api api() const { return {smart_id_status, &service_.pins}; }

    Bytes sign(const Bytes& hash) override {
        const std::string base = with_slash(service_.base_url);
        json body = party(service_.party);
        body["certificateLevel"] = "QUALIFIED";
        body["signatureProtocol"] = "RAW_DIGEST_SIGNATURE";
        body["signatureProtocolParameters"] = {
            {"digest", detail::base64(hash)},
            {"signatureAlgorithm", "rsassa-pss"},
            {"signatureAlgorithmParameters", {{"hashAlgorithm", "SHA-256"}}}};
        body["interactions"] = interactions(dialog_);
        std::string url;
        const bool linked = !linked_.empty();
        if (linked) {
            // The phone that scanned is waiting for exactly this session.
            body["linkedSessionID"] = linked_;
            url = base + "signature/notification/linked/" + document_;
            linked_.clear();  // a certificate choice links one signature only
        } else {
            url = base + "signature/notification/document/" + document_;
        }
        check_cancel(dialog_);
        const json started = sk::parse(exchange(kSmartId, url, &body, 30, api()));
        const std::string session = path_segment(sk::str(started, "sessionID"), "session ID");
        if (!linked) {
            const json* vc = sk::obj(started, "vc");
            const std::string code = vc != nullptr ? sk::str(*vc, "value") : std::string{};
            static const std::regex four{"^[0-9]{4}$"};
            if (!std::regex_match(code, four)) {
                throw Error(0, "Smart-ID sent no verification code");
            }
            // SK makes the code in API v3; it is shown as it came.
            if (dialog_.show_code) {
                dialog_.show_code(code);
            }
            say(dialog_, "Check that your phone shows the same code, then enter PIN2.");
        } else {
            say(dialog_, "Enter your Smart-ID PIN2 on the phone.");
        }
        const sk::SmartIdSession s = poll<sk::SmartIdSession>(
            kSmartId, base + "session/" + session, dialog_, api(),
            sk::parse_smart_id_session);
        smart_id_outcome(s);
        if (!s.cert.empty() && s.cert != cert_der_) {
            throw Error(0, "Smart-ID signed with a different certificate than it chose");
        }
        if (s.signature_algorithm != "rsassa-pss" ||
            (!s.hash_algorithm.empty() && s.hash_algorithm != "SHA-256") ||
            (!s.mgf_hash_algorithm.empty() && s.mgf_hash_algorithm != "SHA-256") ||
            (s.salt_length >= 0 && s.salt_length != 32)) {
            throw Error(0, "Smart-ID signed with other parameters than asked (" +
                               s.signature_algorithm + ")");
        }
        if (s.signature.empty()) {
            throw Error(0, "Smart-ID returned no signature");
        }
        return s.signature;
    }

private:
    SmartIdService service_;
    PhoneDialog dialog_;
    std::string document_;
    std::string linked_;
    Bytes cert_der_;
};

/// The end of a certificate choice: the account's certificate and document
/// number, as an Identity whose signing asks that phone.
std::unique_ptr<Identity::Impl> smart_id_chosen(const sk::SmartIdSession& s,
                                                const SmartIdService& service,
                                                const PhoneDialog& dialog,
                                                std::string linked_session) {
    smart_id_outcome(s);
    const std::string document = path_segment(s.document_number, "Smart-ID document number");
    if (s.cert.empty()) {
        throw Error(0, "Smart-ID returned no certificate");
    }
    detail::X509Ptr cert = signer_certificate(s.cert, kSmartId);
    if (EVP_PKEY_get_base_id(X509_get0_pubkey(cert.get())) != EVP_PKEY_RSA) {
        throw Error(0, "this Smart-ID account's key is not RSA, which Leht cannot use yet");
    }
    auto remote = std::make_shared<SmartIdRemote>(service, dialog, document,
                                                  std::move(linked_session), s.cert);
    return phone_impl(std::move(cert), std::move(remote));
}

// --- Mobile-ID --------------------------------------------------------------

constexpr const char* kMobileId = "Mobile-ID";

void mobile_id_outcome(const sk::MobileIdSession& s) {
    const std::string& r = s.result;
    if (r == "OK") {
        return;
    }
    if (r == "USER_CANCELLED") {
        throw Cancelled("You declined on your phone. Nothing was written.");
    }
    if (r == "TIMEOUT") {
        throw Error(0, "The phone did not answer in time.");
    }
    if (r == "NOT_MID_CLIENT") {
        throw Error(0, "This phone number has no active Mobile-ID.");
    }
    if (r == "SIGNATURE_HASH_MISMATCH") {
        throw Error(0, "The Mobile-ID on this SIM card is set up differently from what SK "
                       "expects; your mobile operator can fix it.");
    }
    if (r == "PHONE_ABSENT") {
        throw Error(0, "The phone could not be reached.");
    }
    if (r == "DELIVERY_ERROR") {
        throw Error(0, "The request could not be delivered to the phone.");
    }
    if (r == "SIM_ERROR") {
        throw Error(0, "The SIM card gave an invalid answer.");
    }
    throw Error(0, "Mobile-ID failed (" + r + "); try again.");
}

std::string mid_language(const std::string& iso639_2) {
    if (iso639_2 == "est") {
        return "EST";
    }
    if (iso639_2 == "rus") {
        return "RUS";
    }
    if (iso639_2 == "lit") {
        return "LIT";
    }
    return "ENG";
}

class MobileIdRemote final : public detail::Remote {
public:
    MobileIdRemote(MobileIdService service, PhoneDialog dialog, std::string phone,
                   std::string national_id, Bytes cert_der, const EVP_MD* md, bool ec)
        : service_(std::move(service)), dialog_(std::move(dialog)), phone_(std::move(phone)),
          national_id_(std::move(national_id)), cert_der_(std::move(cert_der)), md_(md), ec_(ec) {}

    [[nodiscard]] const EVP_MD* digest() const override { return md_; }
    [[nodiscard]] Api api() const { return {mobile_id_status, &service_.pins}; }

    Bytes sign(const Bytes& hash) override {
        const std::string hash_type = EVP_MD_get_type(md_) == NID_sha512   ? "SHA512"
                                      : EVP_MD_get_type(md_) == NID_sha384 ? "SHA384"
                                                                           : "SHA256";
        json body = party(service_.party);
        body["phoneNumber"] = phone_;
        body["nationalIdentityNumber"] = national_id_;
        body["hash"] = detail::base64(hash);
        body["hashType"] = hash_type;
        body["language"] = mid_language(dialog_.language);
        // UCS-2 keeps õäöü; it allows 50 characters.
        body["displayText"] =
            shorten(dialog_.display_text.empty() ? "Sign a PDF" : dialog_.display_text, 50);
        body["displayTextFormat"] = "UCS-2";
        // Mobile-ID's code is made here, from the hash, before the phone is
        // asked -- so the person can already compare when the SIM prompt comes.
        if (dialog_.show_code) {
            dialog_.show_code(sk::mobile_id_code(hash));
        }
        say(dialog_, "Check that your phone shows the same code, then enter PIN2.");
        check_cancel(dialog_);
        const std::string base = service_.base_url;
        const json started =
            sk::parse(exchange(kMobileId, base + "/signature", &body, 30, api()));
        const std::string session = path_segment(sk::str(started, "sessionID"), "session ID");
        const sk::MobileIdSession s = poll<sk::MobileIdSession>(
            kMobileId, base + "/signature/session/" + session, dialog_, api(),
            sk::parse_mobile_id_session);
        mobile_id_outcome(s);
        if (!s.cert.empty() && s.cert != cert_der_) {
            throw Error(0, "Mobile-ID signed with a different certificate than it returned");
        }
        const std::string expected = hash_type + (ec_ ? "WithECEncryption" : "WithRSAEncryption");
        if (s.algorithm != expected) {
            throw Error(0, "Mobile-ID signed with " + s.algorithm + ", not " + expected);
        }
        if (s.signature.empty()) {
            throw Error(0, "Mobile-ID returned no signature");
        }
        return s.signature;
    }

private:
    MobileIdService service_;
    PhoneDialog dialog_;
    std::string phone_;
    std::string national_id_;
    Bytes cert_der_;
    const EVP_MD* md_;
    bool ec_;
};

}  // namespace

// --- public -------------------------------------------------------------------

std::vector<std::string> sk_pins() {
    // SK's servers, demo and live alike (sid.demo.sk.ee, tsp.demo.sk.ee,
    // rp-api.smart-id.com, mid.sk.ee, checked 28 September 2026), are
    // certified by DigiCert: the issuing CA, then its root. Their own
    // certificates are renewed every year -- the demo Smart-ID one expires on
    // 10 October 2026 -- so pinning those would break Leht on SK's schedule;
    // the CA keys last until 2031 and 2038. SHA-256 of the SPKI, Base64.
    return {
        "Wec45nQiFwKvHtuHxSAMGkt19k+uPSw9JlEkxhvYPHk=",  // DigiCert Global G2 TLS RSA SHA256 2020 CA1
        "i7WTqTvh0OioIruIfFR4kMPnBqrS2rdiVPl/s2uC/CY=",  // DigiCert Global Root G2
    };
}

SmartIdService SmartIdService::demo() {
    return {env_or("LEHT_SMARTID_URL", "https://sid.demo.sk.ee/smart-id-rp/v3/"),
            "smart-id-demo",
            {"00000000-0000-4000-8000-000000000000", "DEMO"},
            sk_pins()};
}

MobileIdService MobileIdService::demo() {
    return {env_or("LEHT_MOBILEID_URL", "https://tsp.demo.sk.ee/mid-api"),
            {"00000000-0000-0000-0000-000000000000", "DEMO"},
            sk_pins()};
}

bool Identity::on_phone() const { return impl_->remote != nullptr; }

Identity Identity::from_smart_id_qr(const SmartIdService& service, const PhoneDialog& dialog) {
    init();
    if (!dialog.show_qr) {
        throw Error(0, "Smart-ID by QR code needs somewhere to show the code");
    }
    const std::string base = with_slash(service.base_url);
    json body = party(service.party);
    body["certificateLevel"] = "QUALIFIED";
    check_cancel(dialog);
    const json started = sk::parse(exchange(
        kSmartId, base + "signature/certificate-choice/device-link/anonymous", &body, 30,
        Api{smart_id_status, &service.pins}));
    // elapsedSeconds counts from the moment this reply arrived.
    const auto t0 = std::chrono::steady_clock::now();
    const std::string session = path_segment(sk::str(started, "sessionID"), "session ID");
    sk::LinkParts link;
    link.session_token = sk::str(started, "sessionToken");
    link.device_link_base = sk::str(started, "deviceLinkBase");
    const Bytes secret = detail::unbase64(sk::str(started, "sessionSecret"));
    static const std::regex token_ok{"^[A-Za-z0-9]{16,128}$"};
    static const std::regex base_ok{"^https?://[A-Za-z0-9.:/_-]+$"};
    if (!std::regex_match(link.session_token, token_ok) ||
        !std::regex_match(link.device_link_base, base_ok) || secret.size() < 16) {
        throw Error(0, "Smart-ID sent a device link Leht does not understand");
    }
    link.session_type = "cert";
    link.lang = dialog.language.empty() ? "eng" : dialog.language;
    link.scheme_name = service.scheme_name;
    link.relying_party_name = service.party.name;
    say(dialog, "Scan the QR code with the Smart-ID app.");
    const sk::SmartIdSession s = poll<sk::SmartIdSession>(
        kSmartId, base + "session/" + session, dialog, Api{smart_id_status, &service.pins},
        sk::parse_smart_id_session, [&] {
            link.elapsed_seconds = static_cast<long>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            dialog.show_qr(sk::qr_link(link, secret));
        });
    if (s.end_result == "OK" && !s.flow_type.empty() && s.flow_type != "QR") {
        throw Error(0, "Smart-ID says the account was chosen through " + s.flow_type +
                           ", not the QR code shown");
    }
    return Identity{smart_id_chosen(s, service, dialog, session)};
}

Identity Identity::from_smart_id(const SmartIdService& service, const std::string& semantics_id,
                                 const PhoneDialog& dialog) {
    init();
    static const std::regex etsi{"^(PNO|IDC|PAS)[A-Z]{2}-[A-Za-z0-9-]{1,40}$"};
    if (!std::regex_match(semantics_id, etsi)) {
        throw Error(0, "\"" + semantics_id +
                           "\" is not a personal code as Smart-ID wants it, e.g. PNOEE-38001085718");
    }
    const std::string base = with_slash(service.base_url);
    json body = party(service.party);
    body["certificateLevel"] = "QUALIFIED";
    check_cancel(dialog);
    const json started = sk::parse(
        exchange(kSmartId, base + "signature/certificate-choice/notification/etsi/" + semantics_id,
                 &body, 30, Api{smart_id_status, &service.pins}));
    const std::string session = path_segment(sk::str(started, "sessionID"), "session ID");
    say(dialog, "Choose the Smart-ID account on your phone.");
    const sk::SmartIdSession s = poll<sk::SmartIdSession>(
        kSmartId, base + "session/" + session, dialog, Api{smart_id_status, &service.pins},
        sk::parse_smart_id_session);
    return Identity{smart_id_chosen(s, service, dialog, {})};
}

Identity Identity::from_mobile_id(const MobileIdService& service, const std::string& phone,
                                  const std::string& national_id, const PhoneDialog& dialog) {
    init();
    static const std::regex phone_ok{"^\\+[0-9]{7,15}$"};
    static const std::regex id_ok{"^[0-9]{6,20}$"};
    if (!std::regex_match(phone, phone_ok)) {
        throw Error(0, "\"" + phone + "\" is not a phone number with its country code, e.g. "
                                     "+37268000769");
    }
    if (!std::regex_match(national_id, id_ok)) {
        throw Error(0, "\"" + national_id + "\" is not a personal code");
    }
    json body = party(service.party);
    body["phoneNumber"] = phone;
    body["nationalIdentityNumber"] = national_id;
    check_cancel(dialog);
    say(dialog, "Asking Mobile-ID for the certificate.");
    const json reply = sk::parse(
        exchange(kMobileId, service.base_url + "/certificate", &body, 30, Api{mobile_id_status, &service.pins}));
    const std::string result = sk::str(reply, "result");
    if (result == "NOT_FOUND") {
        throw Error(0, "Mobile-ID has no active certificate for this phone number and personal "
                       "code.");
    }
    if (result != "OK") {
        throw Error(0, "Mobile-ID answered " + result);
    }
    Bytes der;
    try {
        der = detail::unbase64(sk::str(reply, "cert"));
    } catch (const Error&) {
        throw Error(0, "Mobile-ID returned something that is not a certificate");
    }
    detail::X509Ptr cert = signer_certificate(der, kMobileId);
    const EVP_PKEY* key = X509_get0_pubkey(cert.get());
    const int type = key != nullptr ? EVP_PKEY_get_base_id(key) : 0;
    if (type != EVP_PKEY_EC && type != EVP_PKEY_RSA) {
        throw Error(0, "this Mobile-ID key is neither RSA nor ECDSA");
    }
    const bool ec = type == EVP_PKEY_EC;
    const int bits = EVP_PKEY_get_bits(key);
    const EVP_MD* md = !ec ? EVP_sha256() : bits > 384 ? EVP_sha512() : bits > 256 ? EVP_sha384()
                                                                                    : EVP_sha256();
    auto remote = std::make_shared<MobileIdRemote>(service, dialog, phone, national_id, der, md, ec);
    return Identity{phone_impl(std::move(cert), std::move(remote))};
}

}  // namespace leht::crypto
