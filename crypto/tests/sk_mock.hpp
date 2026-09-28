// SPDX-License-Identifier: AGPL-3.0-or-later
//
// SK's Smart-ID and Mobile-ID on 127.0.0.1, for tests: they check every
// request the way SK documents it, sign with the test PKI's keys the way the
// phones do (RSASSA-PSS for Smart-ID; raw r||s ECDSA or PKCS#1 v1.5 for
// Mobile-ID), and can be told to refuse, time out, or cheat.
//
// The Smart-ID mock plays the phone too: scan() takes a QR link as the app
// would, and checks its authCode against the session secret with its own
// code -- not Leht's -- before the certificate choice completes.
#pragma once

#include "test_pki.hpp"

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace leht::test {

using json = nlohmann::json;

inline std::string b64(const crypto::Bytes& data) {
    std::string out(4 * ((data.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data.data(),
                                  static_cast<int>(data.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

inline crypto::Bytes unb64(const std::string& s) {
    crypto::Bytes out(s.size() / 4 * 3 + 3);
    const int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(s.data()),
                                  static_cast<int>(s.size()));
    if (n < 0) {
        return {};
    }
    std::size_t len = static_cast<std::size_t>(n);
    for (auto it = s.rbegin(); it != s.rend() && *it == '='; ++it) {
        --len;
    }
    out.resize(len);
    return out;
}

inline crypto::Bytes der_of(X509* c) {
    unsigned char* p = nullptr;
    const int n = i2d_X509(c, &p);
    crypto::Bytes out(p, p + n);
    OPENSSL_free(p);
    return out;
}

inline crypto::Bytes random_bytes(std::size_t n) {
    crypto::Bytes out(n);
    RAND_bytes(out.data(), static_cast<int>(n));
    return out;
}

inline std::string random_id() {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : random_bytes(16)) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out.substr(0, 8) + "-" + out.substr(8, 4) + "-" + out.substr(12, 4) + "-" +
           out.substr(16, 4) + "-" + out.substr(20, 12);
}

/// Signs `hash` with `key`: PSS (SHA-256, MGF1-SHA-256, salt 32) or PKCS#1
/// v1.5 for RSA, and ECDSA as DER.
inline crypto::Bytes raw_sign(EVP_PKEY* key, const crypto::Bytes& hash, const EVP_MD* md,
                              bool pss) {
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(key, nullptr);
    EVP_PKEY_sign_init(c);
    EVP_PKEY_CTX_set_signature_md(c, md);
    if (EVP_PKEY_get_base_id(key) == EVP_PKEY_RSA) {
        if (pss) {
            EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_PSS_PADDING);
            EVP_PKEY_CTX_set_rsa_mgf1_md(c, md);
            EVP_PKEY_CTX_set_rsa_pss_saltlen(c, EVP_MD_get_size(md));
        } else {
            EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_PADDING);
        }
    }
    std::size_t len = 0;
    EVP_PKEY_sign(c, nullptr, &len, hash.data(), hash.size());
    crypto::Bytes out(len);
    if (EVP_PKEY_sign(c, out.data(), &len, hash.data(), hash.size()) != 1) {
        die("mock signing");
    }
    out.resize(len);
    EVP_PKEY_CTX_free(c);
    return out;
}

/// DER ECDSA-Sig-Value to r||s, each `half` bytes: what a SIM card returns.
inline crypto::Bytes to_raw_ecdsa(const crypto::Bytes& der, int half) {
    const unsigned char* p = der.data();
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()));
    crypto::Bytes out(static_cast<std::size_t>(2 * half));
    BN_bn2binpad(ECDSA_SIG_get0_r(sig), out.data(), half);
    BN_bn2binpad(ECDSA_SIG_get0_s(sig), out.data() + half, half);
    ECDSA_SIG_free(sig);
    return out;
}

/// Everything a request got wrong, collected for the test to CHECK empty.
struct Complaints {
    std::mutex m;
    std::vector<std::string> list;
    void add(const std::string& s) {
        const std::lock_guard<std::mutex> lock(m);
        list.push_back(s);
        std::fprintf(stderr, "  mock SK: %s\n", s.c_str());
    }
    bool none() {
        const std::lock_guard<std::mutex> lock(m);
        return list.empty();
    }
};

class LocalSmartId : public LocalHttp {
public:
    /// What the phone does when asked to sign.
    enum class Outcome { Ok, Refuse, Timeout, WrongVc, OtherKey, OtherCert, Pkcs1 };

    static constexpr const char* kDocument = "PNOEE-40504040001-MOCK-Q";
    static constexpr const char* kPerson = "PNOEE-40504040001";
    static constexpr const char* kLinkBase = "https://sid.demo.sk.ee/device-link";

    explicit LocalSmartId(const Pki& pki)
        : intermediate_(issue(intermediate_key_, {"Leht Test Smart-ID CA", true, -10, 3000},
                              &pki.ca, &pki.ca_key)),
          signer_(issue(key_, {"MAASIKAS,MARI,PNOEE-40504040001", false, -1, 365,
                               "critical,nonRepudiation", nullptr, {}, {}, {}, {},
                               base() + "/ca.cer"},
                        &intermediate_, &intermediate_key_)),
          other_(issue(other_key_, {"Someone Else"}, &intermediate_, &intermediate_key_)) {
        start();
    }
    ~LocalSmartId() override { stop(); }

    [[nodiscard]] crypto::SmartIdService service() const {
        return {base() + "/v3/", "smart-id-demo", {"00000000-0000-4000-8000-000000000000", "DEMO"}};
    }
    [[nodiscard]] const Cert& signer() const { return signer_; }

    std::atomic<Outcome> outcome{Outcome::Ok};
    std::atomic<bool> refuse_choice{false};
    /// Polls answered RUNNING before a session completes.
    std::atomic<int> polls_before_done{2};
    /// HTTP status for the next session start (e.g. 471), then back to 200.
    std::atomic<int> fail_next_start{0};
    std::atomic<int> issuer_fetches{0};
    Complaints complaints;

    /// The phone scans `link`. False when it is not a link this server made,
    /// or its authCode or elapsedSeconds is wrong.
    bool scan(const std::string& link) {
        const std::lock_guard<std::mutex> lock(m_);
        const std::string prefix = std::string(kLinkBase) + "?deviceLinkType=QR&elapsedSeconds=";
        if (link.rfind(prefix, 0) != 0) {
            return false;
        }
        const std::size_t amp = link.find('&', prefix.size());
        const long elapsed = std::stol(link.substr(prefix.size(), amp - prefix.size()));
        const std::size_t tok = link.find("&sessionToken=");
        const std::size_t tok_end = link.find('&', tok + 1);
        const std::string token = link.substr(tok + 14, tok_end - tok - 14);
        for (auto& [id, s] : sessions_) {
            if (s.kind != Kind::CertQr || s.token != token) {
                continue;
            }
            const std::size_t at = link.find("&authCode=");
            const std::string unprotected = link.substr(0, at);
            const std::string want =
                unprotected + "&authCode=" +
                auth_code(s.secret, "smart-id-demo|||" + b64(bytes("DEMO")) + "||||" + unprotected);
            const long real = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                                    std::chrono::steady_clock::now() - s.t0)
                                                    .count());
            if (link != want || elapsed < real - 2 || elapsed > real + 1 ||
                unprotected.find("&sessionType=cert&version=1.0&lang=") == std::string::npos) {
                return false;
            }
            s.scanned = true;
            return true;
        }
        return false;
    }

    /// The digest the phone was last asked to sign, and the code SK showed.
    crypto::Bytes last_digest() {
        const std::lock_guard<std::mutex> lock(m_);
        return last_digest_;
    }
    std::string last_vc() {
        const std::lock_guard<std::mutex> lock(m_);
        return last_vc_;
    }
    std::string last_display_text() {
        const std::lock_guard<std::mutex> lock(m_);
        return last_text_;
    }
    int signatures() const { return signatures_; }

private:
    enum class Kind { CertQr, CertNotification, Sign };
    struct Session {
        explicit Session(Kind k = Kind::Sign, std::string t = {}, crypto::Bytes s = {})
            : kind(k), token(std::move(t)), secret(std::move(s)),
              t0(std::chrono::steady_clock::now()) {}
        Kind kind;
        std::string token;
        crypto::Bytes secret;
        std::chrono::steady_clock::time_point t0;
        bool scanned = false;
        bool linked_used = false;
        bool linked = false;
        int polls = 0;
        crypto::Bytes signature;
        Outcome outcome = Outcome::Ok;
    };

    static crypto::Bytes bytes(const std::string& s) { return {s.begin(), s.end()}; }

    static std::string auth_code(const crypto::Bytes& key, const std::string& payload) {
        unsigned char mac[32];
        unsigned int len = 0;
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), mac, &len);
        std::string out = b64(crypto::Bytes(mac, mac + len));
        for (char& c : out) {
            c = c == '+' ? '-' : c == '/' ? '_' : c;
        }
        while (!out.empty() && out.back() == '=') {
            out.pop_back();
        }
        return out;
    }

    Reply ok(const json& j) { return {"application/json;charset=UTF-8", j.dump()}; }
    static Reply status(int code) { return {"", "", code}; }

    bool party_ok(const json& j) {
        if (j.value("relyingPartyUUID", "") != "00000000-0000-4000-8000-000000000000" ||
            j.value("relyingPartyName", "") != "DEMO") {
            complaints.add("relying party missing or wrong");
            return false;
        }
        if (j.value("certificateLevel", "") != "QUALIFIED") {
            complaints.add("certificateLevel is not QUALIFIED");
        }
        return true;
    }

    Reply start_signing(const json& j, const std::string& doc, bool linked) {
        if (!party_ok(j)) {
            return status(401);
        }
        if (doc != kDocument) {
            return status(404);
        }
        if (j.value("signatureProtocol", "") != "RAW_DIGEST_SIGNATURE") {
            complaints.add("signatureProtocol");
        }
        const json& p = j.at("signatureProtocolParameters");
        const crypto::Bytes digest = unb64(p.value("digest", ""));
        if (digest.size() != 32 || p.value("signatureAlgorithm", "") != "rsassa-pss" ||
            p.at("signatureAlgorithmParameters").value("hashAlgorithm", "") != "SHA-256") {
            complaints.add("signatureProtocolParameters: " + p.dump());
        }
        const crypto::Bytes inter = unb64(j.value("interactions", ""));
        const json list = json::parse(std::string(inter.begin(), inter.end()), nullptr, false);
        if (!list.is_array() || list.empty() || list[0].value("type", "") != "displayTextAndPIN" ||
            list[0].value("displayText60", "").empty()) {
            complaints.add("interactions: " + std::string(inter.begin(), inter.end()));
        } else {
            last_text_ = list[0].value("displayText60", "");
        }
        if (linked) {
            const std::string cert_session = j.value("linkedSessionID", "");
            const auto it = sessions_.find(cert_session);
            if (it == sessions_.end() || it->second.kind != Kind::CertQr || !it->second.scanned ||
                it->second.linked_used) {
                complaints.add("linkedSessionID does not name a finished, unused QR choice");
                return status(400);
            }
            it->second.linked_used = true;
        }
        Session s{Kind::Sign};
        s.linked = linked;
        s.outcome = outcome;
        EVP_PKEY* key = s.outcome == Outcome::OtherKey ? other_key_.p : key_.p;
        s.signature = raw_sign(key, digest, EVP_sha256(), s.outcome != Outcome::Pkcs1);
        last_digest_ = digest;
        const std::string id = random_id();
        sessions_[id] = s;
        json reply = {{"sessionID", id}};
        if (!linked) {
            last_vc_ = std::to_string(1000 + (digest[0] % 9000));
            reply["vc"] = {{"type", "numeric4"}, {"value", last_vc_}};
        }
        return ok(reply);
    }

    json cert_json(const Cert& c) {
        return {{"value", b64(der_of(c.p))}, {"certificateLevel", "QUALIFIED"}};
    }

    Reply session_status(const std::string& id, const std::string& query) {
        if (query.rfind("timeoutMs=", 0) != 0) {
            complaints.add("a session poll without timeoutMs");
        }
        const auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return status(404);
        }
        Session& s = it->second;
        const json running = {{"state", "RUNNING"}};
        if (s.kind == Kind::CertQr && !s.scanned) {
            return ok(running);
        }
        if (++s.polls < polls_before_done) {
            return ok(running);
        }
        json done = {{"state", "COMPLETE"}};
        if (s.kind != Kind::Sign) {
            if (refuse_choice) {
                done["result"] = {{"endResult", "USER_REFUSED_CERT_CHOICE"}};
                return ok(done);
            }
            done["result"] = {{"endResult", "OK"}, {"documentNumber", kDocument}};
            done["cert"] = cert_json(signer_);
            if (s.kind == Kind::CertQr) {
                done["signature"] = {{"flowType", "QR"}};
            }
            return ok(done);
        }
        switch (s.outcome) {
            case Outcome::Refuse: done["result"] = {{"endResult", "USER_REFUSED_INTERACTION"}}; break;
            case Outcome::Timeout: done["result"] = {{"endResult", "TIMEOUT"}}; break;
            case Outcome::WrongVc: done["result"] = {{"endResult", "WRONG_VC"}}; break;
            default: {
                ++signatures_;
                done["result"] = {{"endResult", "OK"}, {"documentNumber", kDocument}};
                done["signatureProtocol"] = "RAW_DIGEST_SIGNATURE";
                json sig = {{"value", b64(s.signature)},
                            {"flowType", s.linked ? "QR" : "Notification"}};
                if (s.outcome == Outcome::Pkcs1) {
                    sig["signatureAlgorithm"] = "sha256WithRSAEncryption";
                } else {
                    sig["signatureAlgorithm"] = "rsassa-pss";
                    sig["signatureAlgorithmParameters"] = {
                        {"hashAlgorithm", "SHA-256"},
                        {"maskGenAlgorithm",
                         {{"algorithm", "id-mgf1"}, {"parameters", {{"hashAlgorithm", "SHA-256"}}}}},
                        {"saltLength", 32},
                        {"trailerField", "0xbc"}};
                }
                done["signature"] = sig;
                done["cert"] = cert_json(s.outcome == Outcome::OtherCert ? other_ : signer_);
                done["interactionTypeUsed"] = "displayTextAndPIN";
            }
        }
        return ok(done);
    }

    Reply answer(const std::string& method, const std::string& target,
                 const std::string& body) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const std::lock_guard<std::mutex> lock(m_);
        const std::size_t q = target.find('?');
        const std::string path = target.substr(0, q);
        const std::string query = q == std::string::npos ? "" : target.substr(q + 1);
        if (method == "GET" && path == "/ca.cer") {
            ++issuer_fetches;
            const crypto::Bytes der = der_of(intermediate_.p);
            return {"application/pkix-cert", std::string(der.begin(), der.end())};
        }
        if (method == "GET" && path.rfind("/v3/session/", 0) == 0) {
            return session_status(path.substr(12), query);
        }
        if (method != "POST") {
            return status(405);
        }
        const json j = json::parse(body, nullptr, false);
        if (!j.is_object()) {
            complaints.add("a POST body that is not JSON: " + body);
            return status(400);
        }
        if (const int code = fail_next_start.exchange(0); code != 0) {
            return status(code);
        }
        if (path == "/v3/signature/certificate-choice/device-link/anonymous") {
            if (!party_ok(j)) {
                return status(401);
            }
            Session s{Kind::CertQr, "wGIrqveE6AuGDATZKmR1mtAZ" + random_id().substr(0, 8),
                      random_bytes(32)};
            // Tokens are letters and digits only.
            s.token.erase(std::remove(s.token.begin(), s.token.end(), '-'), s.token.end());
            const std::string id = random_id();
            sessions_[id] = s;
            return ok({{"sessionID", id},
                       {"sessionToken", s.token},
                       {"sessionSecret", b64(s.secret)},
                       {"deviceLinkBase", kLinkBase}});
        }
        const std::string etsi = "/v3/signature/certificate-choice/notification/etsi/";
        if (path.rfind(etsi, 0) == 0) {
            if (!party_ok(j)) {
                return status(401);
            }
            if (path.substr(etsi.size()) != kPerson) {
                return status(404);
            }
            const std::string id = random_id();
            sessions_[id] = Session{Kind::CertNotification};
            return ok({{"sessionID", id}});
        }
        const std::string linked = "/v3/signature/notification/linked/";
        if (path.rfind(linked, 0) == 0) {
            return start_signing(j, path.substr(linked.size()), true);
        }
        const std::string document = "/v3/signature/notification/document/";
        if (path.rfind(document, 0) == 0) {
            return start_signing(j, path.substr(document.size()), false);
        }
        return status(404);
    }

    Key intermediate_key_ = rsa_key();
    Cert intermediate_;
    Key key_ = rsa_key(3072);
    Cert signer_;
    Key other_key_ = rsa_key();
    Cert other_;
    std::mutex m_;
    std::map<std::string, Session> sessions_;
    crypto::Bytes last_digest_;
    std::string last_vc_;
    std::string last_text_;
    std::atomic<int> signatures_{0};
};

class LocalMobileId : public LocalHttp {
public:
    enum class Outcome { Ok, UserCancelled, HashMismatch, OtherKey, WrongAlgorithm, NotMidClient };

    static constexpr const char* kPhone = "+37268000769";
    static constexpr const char* kId = "60001017869";

    /// An ECDSA P-256 key (as the demo's +37200000766 has) or an RSA one.
    LocalMobileId(const Pki& pki, bool ec)
        : key_(ec ? ec_key("P-256") : rsa_key()), other_key_(ec ? ec_key("P-256") : rsa_key()),
          ec_(ec),
          signer_(issue(key_, {"O'CONNEŽ-ŠUSLIK TESTNUMBER,MARY ÄNN,60001017869"}, &pki.ca,
                        &pki.ca_key)) {
        start();
    }
    ~LocalMobileId() override { stop(); }

    [[nodiscard]] crypto::MobileIdService service() const {
        return {base() + "/mid-api", {"00000000-0000-0000-0000-000000000000", "DEMO"}};
    }

    std::atomic<Outcome> outcome{Outcome::Ok};
    std::atomic<int> polls_before_done{2};
    Complaints complaints;

    crypto::Bytes last_hash() {
        const std::lock_guard<std::mutex> lock(m_);
        return last_hash_;
    }
    json last_request() {
        const std::lock_guard<std::mutex> lock(m_);
        return last_request_;
    }

private:
    struct Session {
        int polls = 0;
        Outcome outcome = Outcome::Ok;
        crypto::Bytes signature;
        std::string algorithm;
    };

    Reply ok(const json& j) { return {"application/json", j.dump()}; }
    static Reply status(int code) { return {"", "", code}; }

    bool party_ok(const json& j) {
        const bool good = j.value("relyingPartyUUID", "") == "00000000-0000-0000-0000-000000000000" &&
                          j.value("relyingPartyName", "") == "DEMO";
        if (!good) {
            complaints.add("relying party missing or wrong");
        }
        return good;
    }

    Reply answer(const std::string& method, const std::string& target,
                 const std::string& body) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const std::lock_guard<std::mutex> lock(m_);
        const std::size_t q = target.find('?');
        const std::string path = target.substr(0, q);
        if (method == "GET" && path.rfind("/mid-api/signature/session/", 0) == 0) {
            if (q == std::string::npos || target.find("timeoutMs=", q) == std::string::npos) {
                complaints.add("a session poll without timeoutMs");
            }
            const auto it = sessions_.find(path.substr(27));
            if (it == sessions_.end()) {
                return status(404);
            }
            Session& s = it->second;
            if (++s.polls < polls_before_done) {
                return ok({{"state", "RUNNING"}});
            }
            json done = {{"state", "COMPLETE"}};
            switch (s.outcome) {
                case Outcome::UserCancelled: done["result"] = "USER_CANCELLED"; break;
                case Outcome::HashMismatch: done["result"] = "SIGNATURE_HASH_MISMATCH"; break;
                default:
                    done["result"] = "OK";
                    done["signature"] = {{"value", b64(s.signature)}, {"algorithm", s.algorithm}};
                    done["cert"] = b64(der_of(signer_.p));
            }
            return ok(done);
        }
        const json j = json::parse(body, nullptr, false);
        if (method != "POST" || !j.is_object()) {
            return status(400);
        }
        if (!party_ok(j)) {
            return status(401);
        }
        last_request_ = j;
        const bool known = j.value("phoneNumber", "") == kPhone &&
                           j.value("nationalIdentityNumber", "") == kId;
        if (path == "/mid-api/certificate") {
            if (!known || outcome == Outcome::NotMidClient) {
                return ok({{"result", "NOT_FOUND"}});
            }
            return ok({{"result", "OK"}, {"cert", b64(der_of(signer_.p))}});
        }
        if (path == "/mid-api/signature") {
            if (!known) {
                return status(400);
            }
            const std::string type = j.value("hashType", "");
            const crypto::Bytes hash = unb64(j.value("hash", ""));
            const EVP_MD* md = type == "SHA256" ? EVP_sha256()
                               : type == "SHA384" ? EVP_sha384()
                               : type == "SHA512" ? EVP_sha512()
                                                  : nullptr;
            if (md == nullptr || hash.size() != static_cast<std::size_t>(EVP_MD_get_size(md))) {
                complaints.add("hash and hashType disagree: " + type);
                return status(400);
            }
            const std::string lang = j.value("language", "");
            if (lang != "EST" && lang != "ENG" && lang != "RUS" && lang != "LIT") {
                complaints.add("language " + lang);
            }
            if (j.value("displayTextFormat", "") != "UCS-2" ||
                j.value("displayText", "").empty()) {
                complaints.add("displayText or its format");
            }
            last_hash_ = hash;
            Session s;
            s.outcome = outcome;
            EVP_PKEY* key = s.outcome == Outcome::OtherKey ? other_key_.p : key_.p;
            s.signature = raw_sign(key, hash, md, false);
            if (ec_) {
                s.signature = to_raw_ecdsa(s.signature, 32);
            }
            s.algorithm = type + (ec_ ? "WithECEncryption" : "WithRSAEncryption");
            if (s.outcome == Outcome::WrongAlgorithm) {
                s.algorithm = "SHA512" + s.algorithm.substr(6);
            }
            const std::string id = random_id();
            sessions_[id] = s;
            return ok({{"sessionID", id}});
        }
        return status(404);
    }

    Key key_;
    Key other_key_;
    bool ec_;
    Cert signer_;
    std::mutex m_;
    std::map<std::string, Session> sessions_;
    crypto::Bytes last_hash_;
    json last_request_;
};

}  // namespace leht::test
