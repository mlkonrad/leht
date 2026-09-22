// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A throwaway PKI for tests, generated fresh on every run through OpenSSL
// directly, so no private key is ever committed: a root CA, signer
// certificates with RSA and ECDSA keys, deliberately broken ones, and a
// timestamp authority -- plus a TSA server on localhost that answers RFC 3161
// requests, so B-T signing is tested without the network.
#pragma once

#include "leht/crypto/crypto.hpp"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ts.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace leht::test {

inline void die(const char* what) {
    std::fprintf(stderr, "test PKI: %s\n", what);
    ERR_print_errors_fp(stderr);
    std::exit(1);
}

struct Key {
    EVP_PKEY* p = nullptr;
    Key() = default;
    explicit Key(EVP_PKEY* k) : p(k) {}
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    Key(Key&& o) noexcept : p(o.p) { o.p = nullptr; }
    ~Key() { EVP_PKEY_free(p); }
};

struct Cert {
    X509* p = nullptr;
    Cert() = default;
    explicit Cert(X509* c) : p(c) {}
    Cert(const Cert&) = delete;
    Cert& operator=(const Cert&) = delete;
    Cert(Cert&& o) noexcept : p(o.p) { o.p = nullptr; }
    ~Cert() { X509_free(p); }

    [[nodiscard]] std::string pem() const {
        BIO* b = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(b, p);
        char* d = nullptr;
        const long n = BIO_get_mem_data(b, &d);
        std::string out(d, static_cast<std::size_t>(n));
        BIO_free(b);
        return out;
    }
};

inline Key rsa_key(int bits = 2048) {
    EVP_PKEY* k = EVP_RSA_gen(static_cast<unsigned>(bits));
    if (k == nullptr) {
        die("RSA keygen");
    }
    return Key{k};
}

inline Key ec_key(const char* curve = "P-384") {
    EVP_PKEY* k = EVP_EC_gen(curve);
    if (k == nullptr) {
        die("EC keygen");
    }
    return Key{k};
}

struct CertSpec {
    std::string cn;
    bool ca = false;
    long valid_from_days = -1;  ///< relative to now
    long valid_to_days = 365;
    const char* key_usage = "critical,digitalSignature,nonRepudiation";
    const char* ext_key_usage = nullptr;
};

/// Issues a certificate for `key` from `issuer` (self-signed when null).
inline Cert issue(const Key& key, const CertSpec& spec, const Cert* issuer = nullptr,
                  const Key* issuer_key = nullptr) {
    X509* c = X509_new();
    X509_set_version(c, 2);
    static std::atomic<long> serial{1000};
    ASN1_INTEGER_set(X509_get_serialNumber(c), serial++);
    X509_gmtime_adj(X509_getm_notBefore(c), spec.valid_from_days * 86400L);
    X509_gmtime_adj(X509_getm_notAfter(c), spec.valid_to_days * 86400L);
    X509_set_pubkey(c, key.p);
    X509_NAME* name = X509_get_subject_name(c);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("EE"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("Leht Test PKI"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                               reinterpret_cast<const unsigned char*>(spec.cn.c_str()), -1, -1, 0);
    X509_set_issuer_name(c, issuer != nullptr ? X509_get_subject_name(issuer->p) : name);

    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer != nullptr ? issuer->p : c, c, nullptr, nullptr, 0);
    const auto add = [&](int nid, const char* value) {
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
        if (ext == nullptr) {
            die("extension");
        }
        X509_add_ext(c, ext, -1);
        X509_EXTENSION_free(ext);
    };
    add(NID_basic_constraints, spec.ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
    add(NID_key_usage, spec.ca ? "critical,keyCertSign,cRLSign" : spec.key_usage);
    if (spec.ext_key_usage != nullptr) {
        add(NID_ext_key_usage, spec.ext_key_usage);
    }
    add(NID_subject_key_identifier, "hash");
    if (X509_sign(c, issuer_key != nullptr ? issuer_key->p : key.p, EVP_sha256()) == 0) {
        die("X509_sign");
    }
    return Cert{c};
}

inline crypto::Bytes pkcs12(const Key& key, const Cert& cert, const std::vector<const Cert*>& chain,
                            const std::string& password) {
    STACK_OF(X509)* ca = sk_X509_new_null();
    for (const Cert* c : chain) {
        sk_X509_push(ca, c->p);
    }
    // Modern defaults: AES-256 and PBKDF2, what current exports produce.
    PKCS12* p12 = PKCS12_create(password.c_str(), "signer", key.p, cert.p, ca, 0, 0, 0, 0, 0);
    sk_X509_free(ca);
    if (p12 == nullptr) {
        die("PKCS12_create");
    }
    unsigned char* der = nullptr;
    const int n = i2d_PKCS12(p12, &der);
    PKCS12_free(p12);
    crypto::Bytes out(der, der + n);
    OPENSSL_free(der);
    return out;
}

/// A root CA and the certificates the tests use.
struct Pki {
    Key ca_key = rsa_key();
    Cert ca = issue(ca_key, {"Leht Test Root", true, -30, 3650});
    Key rsa = rsa_key();
    Cert rsa_cert = issue(rsa, {"Mari Maasikas"}, &ca, &ca_key);
    Key ec = ec_key("P-384");
    Cert ec_cert = issue(ec, {"Jaan Tamm"}, &ca, &ca_key);
    Key expired = rsa_key();
    Cert expired_cert = issue(expired, {"Expired Signer", false, -400, -30}, &ca, &ca_key);
    Key no_sign = rsa_key();
    Cert no_sign_cert = issue(no_sign, {"Encryption Only", false, -1, 365,
                                        "critical,keyEncipherment"}, &ca, &ca_key);
    Key tsa_key = rsa_key();
    Cert tsa_cert = issue(tsa_key, {"Leht Test TSA", false, -1, 365, "critical,digitalSignature",
                                    "critical,timeStamping"}, &ca, &ca_key);

    [[nodiscard]] crypto::TrustStore trust() const {
        crypto::TrustStore t;
        t.add_pem(ca.pem());
        return t;
    }
    [[nodiscard]] crypto::Identity identity(const Key& k, const Cert& c) const {
        return crypto::Identity::from_pkcs12(pkcs12(k, c, {&ca}, "pw"), crypto::Secret{"pw"});
    }
};

/// An RFC 3161 timestamp authority on 127.0.0.1, one request per connection,
/// in a background thread. Good enough for tests; not a server.
class LocalTsa {
public:
    LocalTsa(const Key& key, const Cert& cert, const Cert& ca) : key_(key), cert_(cert), ca_(ca) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        if (fd_ < 0 || ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(fd_, 4) != 0 ||
            ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            die("TSA socket");
        }
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    LocalTsa(const LocalTsa&) = delete;
    LocalTsa& operator=(const LocalTsa&) = delete;
    ~LocalTsa() {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        thread_.join();
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/tsa";
    }
    /// Answer with a wrong nonce from now on: a replayed reply.
    void corrupt_nonce() { corrupt_nonce_ = true; }

private:
    static ASN1_INTEGER* serial_cb(TS_RESP_CTX*, void*) {
        static std::atomic<long> n{1};
        ASN1_INTEGER* i = ASN1_INTEGER_new();
        ASN1_INTEGER_set(i, n++);
        return i;
    }

    void serve() {
        while (!stop_) {
            const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) {
                return;
            }
            handle(c);
            ::close(c);
        }
    }

    void handle(int c) {
        std::string in;
        char buf[4096];
        std::size_t want = std::string::npos;
        for (;;) {
            const ssize_t n = ::read(c, buf, sizeof(buf));
            if (n <= 0) {
                return;
            }
            in.append(buf, static_cast<std::size_t>(n));
            const std::size_t head = in.find("\r\n\r\n");
            if (head != std::string::npos && want == std::string::npos) {
                const std::size_t cl = in.find("Content-Length:");
                const std::size_t cl2 = in.find("content-length:");
                const std::size_t at = cl != std::string::npos ? cl : cl2;
                if (at == std::string::npos) {
                    return;
                }
                want = head + 4 + std::strtoul(in.c_str() + at + 15, nullptr, 10);
            }
            if (want != std::string::npos && in.size() >= want) {
                break;
            }
        }
        const std::string body = in.substr(in.find("\r\n\r\n") + 4);

        TS_RESP_CTX* ctx = TS_RESP_CTX_new();
        TS_RESP_CTX_set_signer_cert(ctx, cert_.p);
        TS_RESP_CTX_set_signer_key(ctx, key_.p);
        STACK_OF(X509)* certs = sk_X509_new_null();
        sk_X509_push(certs, ca_.p);
        TS_RESP_CTX_set_certs(ctx, certs);
        sk_X509_free(certs);
        ASN1_OBJECT* policy = OBJ_txt2obj("1.3.6.1.4.1.99999.1", 1);
        TS_RESP_CTX_set_def_policy(ctx, policy);
        ASN1_OBJECT_free(policy);
        TS_RESP_CTX_add_md(ctx, EVP_sha256());
        TS_RESP_CTX_set_serial_cb(ctx, serial_cb, nullptr);
        TS_RESP_CTX_set_signer_digest(ctx, EVP_sha256());
        TS_RESP_CTX_add_flags(ctx, TS_ESS_CERT_ID_CHAIN);

        std::string req = body;
        if (corrupt_nonce_) {
            // Re-encode the request with a different nonce, so the reply
            // answers a question the client never asked.
            const unsigned char* p = reinterpret_cast<const unsigned char*>(req.data());
            TS_REQ* r = d2i_TS_REQ(nullptr, &p, static_cast<long>(req.size()));
            ASN1_INTEGER* other = ASN1_INTEGER_new();
            ASN1_INTEGER_set(other, 42);
            TS_REQ_set_nonce(r, other);
            ASN1_INTEGER_free(other);
            unsigned char* der = nullptr;
            const int n = i2d_TS_REQ(r, &der);
            req.assign(reinterpret_cast<char*>(der), static_cast<std::size_t>(n));
            OPENSSL_free(der);
            TS_REQ_free(r);
        }
        BIO* rb = BIO_new_mem_buf(req.data(), static_cast<int>(req.size()));
        TS_RESP* resp = TS_RESP_create_response(ctx, rb);
        BIO_free(rb);
        unsigned char* der = nullptr;
        const int n = resp != nullptr ? i2d_TS_RESP(resp, &der) : 0;
        TS_RESP_free(resp);
        TS_RESP_CTX_free(ctx);
        if (n <= 0) {
            return;
        }
        const std::string head = "HTTP/1.0 200 OK\r\nContent-Type: application/timestamp-reply\r\n"
                                 "Content-Length: " + std::to_string(n) + "\r\n\r\n";
        (void)!::write(c, head.data(), head.size());
        (void)!::write(c, der, static_cast<std::size_t>(n));
        OPENSSL_free(der);
    }

    const Key& key_;
    const Cert& cert_;
    const Cert& ca_;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> corrupt_nonce_{false};
    std::thread thread_;
};

}  // namespace leht::test
