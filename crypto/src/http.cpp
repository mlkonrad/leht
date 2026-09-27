// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The one HTTP client in Leht: timestamp authorities, OCSP responders and CRL
// distribution points. Trusted process only -- the worker has no network.
//
// Deliberately small: http:// and https:// only, one request per connection,
// no redirects (OSSL_HTTP_transfer does not follow them), a cap on the reply,
// and a timeout. What comes back is not looked at here.
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/err.h>
#include <openssl/http.h>
#include <openssl/ssl.h>

#include <array>

namespace leht::crypto::detail {

namespace {

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

}  // namespace

std::string url_host(const std::string& url) {
    char* host = nullptr;
    if (OSSL_HTTP_parse_url(url.c_str(), nullptr, nullptr, &host, nullptr, nullptr, nullptr,
                            nullptr, nullptr) != 1) {
        ERR_clear_error();
        return {};
    }
    std::string out = host;
    OPENSSL_free(host);
    return out;
}

Bytes http_transfer(const HttpRequest& r) {
    if (r.url.rfind("https://", 0) != 0 && r.url.rfind("http://", 0) != 0) {
        throw Error(0, r.what + " must be an http:// or https:// URL, not " + r.url);
    }
    int use_ssl = 0;
    char* host = nullptr;
    char* port = nullptr;
    char* path = nullptr;
    char* query = nullptr;
    if (OSSL_HTTP_parse_url(r.url.c_str(), &use_ssl, nullptr, &host, &port, nullptr, &path,
                            &query, nullptr) != 1) {
        fail("cannot parse the URL of " + r.what + ": " + r.url);
    }
    const std::string h = host;
    const std::string p = port;
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
            fail("cannot set up TLS for " + r.what);
        }
        SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_PEER, nullptr);
        tls.host = h;
        tls.ctx = ssl_ctx.get();
    }

    BioPtr body;
    if (r.post != nullptr) {
        body.reset(BIO_new_mem_buf(r.post->data(), static_cast<int>(r.post->size())));
        if (!body) {
            fail("cannot build the request to " + r.what);
        }
    }
    // A null request body makes this a GET.
    BioPtr reply{OSSL_HTTP_transfer(
        nullptr, h.c_str(), p.c_str(), target.c_str(), use_ssl, nullptr, nullptr, nullptr,
        nullptr, use_ssl != 0 ? tls_wrap : nullptr, use_ssl != 0 ? &tls : nullptr, 0, nullptr,
        r.post != nullptr ? r.content_type : nullptr, body.get(), r.expected_type,
        r.expect_asn1 ? 1 : 0, r.max_size, r.timeout_seconds, 0)};
    if (!reply) {
        fail(r.what + " at " + r.url + " did not answer");
    }
    Bytes out;
    std::array<unsigned char, 16384> buf{};
    for (;;) {
        const int n = BIO_read(reply.get(), buf.data(), static_cast<int>(buf.size()));
        if (n <= 0) {
            break;
        }
        out.insert(out.end(), buf.data(), buf.data() + n);
        if (out.size() > r.max_size) {
            throw Error(0, r.what + " at " + r.url + " sent more than " +
                               std::to_string(r.max_size) + " bytes");
        }
    }
    ERR_clear_error();
    if (out.empty()) {
        throw Error(0, r.what + " at " + r.url + " sent an empty reply");
    }
    return out;
}

}  // namespace leht::crypto::detail
