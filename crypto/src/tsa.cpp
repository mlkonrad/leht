// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 3161 client for PAdES B-T. Runs only in the trusted process: the worker
// has no network, and never signs.
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/err.h>
#include <openssl/http.h>
#include <openssl/pkcs7.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/ts.h>

#include <array>
#include <cstring>

namespace leht::crypto::detail {

namespace {

using TsReqPtr = Ptr<TS_REQ, TS_REQ_free>;
using TsRespPtr = Ptr<TS_RESP, TS_RESP_free>;
using ImprintPtr = Ptr<TS_MSG_IMPRINT, TS_MSG_IMPRINT_free>;
using AlgorPtr = Ptr<X509_ALGOR, X509_ALGOR_free>;
using IntPtr = Ptr<ASN1_INTEGER, ASN1_INTEGER_free>;
using BnPtr = Ptr<BIGNUM, BN_free>;
using SslCtxPtr = Ptr<SSL_CTX, SSL_CTX_free>;

struct TlsArg {
    SSL_CTX* ctx = nullptr;
    std::string host;
};

// OSSL_HTTP_transfer's hook for https: wrap the connected socket BIO in TLS,
// verifying the server against the system store, with SNI and hostname check.
BIO* tls_wrap(BIO* bio, void* arg, int connect, int detail) {
    auto* tls = static_cast<TlsArg*>(arg);
    if (connect == 0 || detail == 0) {
        // Disconnecting, or connecting without TLS: nothing to add or remove.
        if (connect == 0 && detail != 0) {
            BIO* ssl = bio;
            bio = BIO_pop(ssl);
            BIO_free(ssl);
        }
        return bio;
    }
    BIO* sbio = BIO_new_ssl(tls->ctx, 1);
    if (sbio == nullptr) {
        return nullptr;
    }
    SSL* ssl = nullptr;
    BIO_get_ssl(sbio, &ssl);
    // SSL_set_tlsext_host_name() without its C cast.
    if (ssl == nullptr ||
        SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                 const_cast<char*>(tls->host.c_str())) != 1 ||
        SSL_set1_host(ssl, tls->host.c_str()) != 1) {
        BIO_free(sbio);
        return nullptr;
    }
    return BIO_push(sbio, bio);
}

Bytes sha256(const unsigned char* p, std::size_t n) {
    Bytes out(32);
    unsigned int len = 0;
    if (EVP_Digest(p, n, out.data(), &len, EVP_sha256(), nullptr) != 1) {
        fail("digest failed");
    }
    return out;
}

}  // namespace

TimestampToken request_timestamp(const std::string& url, const unsigned char* data,
                                 std::size_t size, int timeout_seconds) {
    // The request: SHA-256 imprint of the signature value, a 64-bit random
    // nonce so a replayed reply is caught, and certReq so the token carries
    // the TSA certificate a verifier needs.
    const Bytes hash = sha256(data, size);
    TsReqPtr req{TS_REQ_new()};
    ImprintPtr imprint{TS_MSG_IMPRINT_new()};
    AlgorPtr alg{X509_ALGOR_new()};
    std::array<unsigned char, 8> nonce_bytes{};
    if (!req || !imprint || !alg || RAND_bytes(nonce_bytes.data(), 8) != 1) {
        fail("cannot build a timestamp request");
    }
    nonce_bytes[0] &= 0x7F;  // keep it positive
    BnPtr bn{BN_bin2bn(nonce_bytes.data(), 8, nullptr)};
    IntPtr nonce{BN_to_ASN1_INTEGER(bn.get(), nullptr)};
    X509_ALGOR_set_md(alg.get(), EVP_sha256());
    if (!nonce || TS_REQ_set_version(req.get(), 1) != 1 ||
        TS_MSG_IMPRINT_set_algo(imprint.get(), alg.get()) != 1 ||
        TS_MSG_IMPRINT_set_msg(imprint.get(), const_cast<unsigned char*>(hash.data()),
                               static_cast<int>(hash.size())) != 1 ||
        TS_REQ_set_msg_imprint(req.get(), imprint.get()) != 1 ||
        TS_REQ_set_nonce(req.get(), nonce.get()) != 1 || TS_REQ_set_cert_req(req.get(), 1) != 1) {
        fail("cannot build a timestamp request");
    }
    BioPtr req_bio{BIO_new(BIO_s_mem())};
    if (!req_bio || i2d_TS_REQ_bio(req_bio.get(), req.get()) != 1) {
        fail("cannot encode the timestamp request");
    }

    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        throw Error(0, "the timestamp authority must be an http:// or https:// URL");
    }
    int use_ssl = 0;
    char* host = nullptr;
    char* port = nullptr;
    char* path = nullptr;
    char* query = nullptr;
    if (OSSL_HTTP_parse_url(url.c_str(), &use_ssl, nullptr, &host, &port, nullptr, &path,
                            &query, nullptr) != 1) {
        fail("cannot parse the timestamp authority URL " + url);
    }
    const std::string h = host, p = port;
    std::string target = path;
    if (query != nullptr && *query != '\0') {
        target += std::string("?") + query;
    }
    OPENSSL_free(host);
    OPENSSL_free(port);
    OPENSSL_free(path);
    OPENSSL_free(query);

    TlsArg tls;
    SslCtxPtr ssl_ctx;
    if (use_ssl != 0) {
        ssl_ctx.reset(SSL_CTX_new(TLS_client_method()));
        if (!ssl_ctx || SSL_CTX_set_default_verify_paths(ssl_ctx.get()) != 1) {
            fail("cannot set up TLS for the timestamp authority");
        }
        SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_PEER, nullptr);
        tls.host = h;
        tls.ctx = ssl_ctx.get();
    }

    BioPtr reply{OSSL_HTTP_transfer(nullptr, h.c_str(), p.c_str(), target.c_str(), use_ssl,
                                    nullptr, nullptr, nullptr, nullptr,
                                    use_ssl != 0 ? tls_wrap : nullptr,
                                    use_ssl != 0 ? &tls : nullptr, 0, nullptr,
                                    "application/timestamp-query", req_bio.get(),
                                    "application/timestamp-reply", 1, std::size_t{1} << 20,
                                    timeout_seconds, 0)};
    if (!reply) {
        fail("the timestamp authority at " + url + " did not answer");
    }
    TsRespPtr resp{d2i_TS_RESP_bio(reply.get(), nullptr)};
    if (!resp) {
        fail("the timestamp authority's reply is not a timestamp response");
    }

    TS_STATUS_INFO* status = TS_RESP_get_status_info(resp.get());
    const long code = ASN1_INTEGER_get(TS_STATUS_INFO_get0_status(status));
    if (code != 0 && code != 1) {  // granted, grantedWithMods
        throw Error(0, "the timestamp authority refused the request (status " +
                           std::to_string(code) + ")");
    }
    PKCS7* token = TS_RESP_get_token(resp.get());
    TS_TST_INFO* tst = TS_RESP_get_tst_info(resp.get());
    if (token == nullptr || tst == nullptr) {
        throw Error(0, "the timestamp authority's reply holds no token");
    }
    // What was stamped must be what we asked for, and the reply must be the
    // answer to this request, not a replay.
    const ASN1_OCTET_STRING* got = TS_MSG_IMPRINT_get_msg(TS_TST_INFO_get_msg_imprint(tst));
    if (got == nullptr || static_cast<std::size_t>(ASN1_STRING_length(got)) != hash.size() ||
        std::memcmp(ASN1_STRING_get0_data(got), hash.data(), hash.size()) != 0) {
        throw Error(0, "the timestamp does not cover this signature");
    }
    const ASN1_INTEGER* got_nonce = TS_TST_INFO_get_nonce(tst);
    if (got_nonce == nullptr || ASN1_INTEGER_cmp(got_nonce, nonce.get()) != 0) {
        throw Error(0, "the timestamp reply's nonce does not match the request");
    }
    // The token's own signature, against the certificate it carries.
    // Whether that authority is trusted is the verifier's call.
    StorePtr empty{X509_STORE_new()};
    if (PKCS7_verify(token, nullptr, empty.get(), nullptr, nullptr, PKCS7_NOVERIFY) != 1) {
        fail("the timestamp token's signature does not verify");
    }

    TimestampToken out;
    out.time = to_unix(TS_TST_INFO_get_time(tst));
    unsigned char* der = nullptr;
    const int n = i2d_PKCS7(token, &der);
    if (n <= 0) {
        fail("cannot encode the timestamp token");
    }
    out.der.assign(der, der + n);
    OPENSSL_free(der);
    ERR_clear_error();
    return out;
}

}  // namespace leht::crypto::detail
