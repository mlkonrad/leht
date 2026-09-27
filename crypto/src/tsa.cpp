// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 3161 client, for PAdES B-T and for document timestamps. Runs only in the
// trusted process: the worker has no network, and never signs.
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/err.h>
#include <openssl/pkcs7.h>
#include <openssl/rand.h>
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

}  // namespace

Bytes sha256(const unsigned char* p, std::size_t n) {
    Bytes out(32);
    unsigned int len = 0;
    if (EVP_Digest(p, n, out.data(), &len, EVP_sha256(), nullptr) != 1) {
        fail("digest failed");
    }
    return out;
}

TimestampToken request_timestamp(const std::string& url, const Bytes& imprint,
                                 int timeout_seconds) {
    // The request: the SHA-256 imprint (of a signature value for B-T, of the
    // document's byte ranges for a document timestamp), a 64-bit random nonce
    // so a replayed reply is caught, and certReq so the token carries the TSA
    // certificate a verifier needs.
    if (imprint.size() != 32) {
        throw Error(0, "a timestamp imprint must be a SHA-256 digest");
    }
    const Bytes& hash = imprint;
    TsReqPtr req{TS_REQ_new()};
    ImprintPtr msg{TS_MSG_IMPRINT_new()};
    AlgorPtr alg{X509_ALGOR_new()};
    std::array<unsigned char, 8> nonce_bytes{};
    if (!req || !msg || !alg || RAND_bytes(nonce_bytes.data(), 8) != 1) {
        fail("cannot build a timestamp request");
    }
    nonce_bytes[0] &= 0x7F;  // keep it positive
    BnPtr bn{BN_bin2bn(nonce_bytes.data(), 8, nullptr)};
    IntPtr nonce{BN_to_ASN1_INTEGER(bn.get(), nullptr)};
    X509_ALGOR_set_md(alg.get(), EVP_sha256());
    if (!nonce || TS_REQ_set_version(req.get(), 1) != 1 ||
        TS_MSG_IMPRINT_set_algo(msg.get(), alg.get()) != 1 ||
        TS_MSG_IMPRINT_set_msg(msg.get(), const_cast<unsigned char*>(hash.data()),
                               static_cast<int>(hash.size())) != 1 ||
        TS_REQ_set_msg_imprint(req.get(), msg.get()) != 1 ||
        TS_REQ_set_nonce(req.get(), nonce.get()) != 1 || TS_REQ_set_cert_req(req.get(), 1) != 1) {
        fail("cannot build a timestamp request");
    }
    unsigned char* req_der = nullptr;
    const int req_len = i2d_TS_REQ(req.get(), &req_der);
    if (req_len <= 0) {
        fail("cannot encode the timestamp request");
    }
    const Bytes request(req_der, req_der + req_len);
    OPENSSL_free(req_der);
    HttpRequest http;
    http.url = url;
    http.what = "the timestamp authority";
    http.post = &request;
    http.content_type = "application/timestamp-query";
    http.expected_type = "application/timestamp-reply";
    http.expect_asn1 = true;
    http.timeout_seconds = timeout_seconds;
    const Bytes reply = http_transfer(http);
    const unsigned char* rp = reply.data();
    TsRespPtr resp{d2i_TS_RESP(nullptr, &rp, static_cast<long>(reply.size()))};
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
