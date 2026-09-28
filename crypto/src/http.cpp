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
#include <ctime>

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
        // Disconnecting, or connecting without TLS: leave the chain as it is.
        // It must not be taken apart here: for a reply that is streamed, not
        // read whole, OpenSSL disconnects before the caller has read it, and
        // the TLS BIO freed here is the one the caller is about to read from.
        // OpenSSL frees the chain itself.
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

}  // namespace leht::crypto::detail

namespace leht::crypto {

Bytes http_get(const std::string& url, std::size_t max_size, int timeout_seconds) {
    detail::HttpRequest r;
    r.url = url;
    r.what = "the server";
    r.max_size = max_size;
    r.timeout_seconds = timeout_seconds;
    // The trusted lists' addresses come from the signed LOTL, and what they
    // serve is verified by signature: a redirect cannot smuggle anything in.
    r.max_redirects = 3;
    return detail::http_transfer(r);
}

}  // namespace leht::crypto

namespace leht::crypto::detail {

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

namespace {

struct CtxClose {
    void operator()(OSSL_HTTP_REQ_CTX* c) const noexcept { (void)OSSL_HTTP_close(c, 1); }
};
using ReqCtxPtr = std::unique_ptr<OSSL_HTTP_REQ_CTX, CtxClose>;

/// Reads a reply to the end: a memory BIO for ASN.1, a stream otherwise, where
/// a slow server's first empty read is not its end.
Bytes read_reply(BIO* reply, const HttpRequest& r, const std::string& url) {
    Bytes out;
    std::array<unsigned char, 16384> buf{};
    const time_t deadline = std::time(nullptr) + (r.timeout_seconds > 0 ? r.timeout_seconds : 60);
    // A memory BIO (an ASN.1 reply, read whole) says "retry" at its end by
    // default; its end is simply the end.
    const bool memory = BIO_method_type(reply) == BIO_TYPE_MEM;
    for (;;) {
        const int n = BIO_read(reply, buf.data(), static_cast<int>(buf.size()));
        if (n <= 0) {
            if (!memory && BIO_should_retry(reply) != 0) {
                const int ready = BIO_wait(reply, deadline, 100);
                if (ready > 0) {
                    continue;
                }
                throw Error(0, r.what + " at " + url +
                                   (ready == 0 ? " took too long to answer" : " broke off"));
            }
            break;
        }
        out.insert(out.end(), buf.data(), buf.data() + n);
        if (out.size() > r.max_size) {
            throw Error(0, r.what + " at " + url + " sent more than " +
                               std::to_string(r.max_size) + " bytes");
        }
    }
    ERR_clear_error();
    if (out.empty()) {
        throw Error(0, r.what + " at " + url + " sent an empty reply");
    }
    return out;
}

/// One request to `url`. Returns the body, or sets `*redirect` and returns
/// nothing when the server points elsewhere.
Bytes one_hop(const HttpRequest& r, const std::string& url, std::string* redirect) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        throw Error(0, r.what + " must be an http:// or https:// URL, not " + url);
    }
    int use_ssl = 0;
    char* host = nullptr;
    char* port = nullptr;
    char* path = nullptr;
    char* query = nullptr;
    if (OSSL_HTTP_parse_url(url.c_str(), &use_ssl, nullptr, &host, &port, nullptr, &path, &query,
                            nullptr) != 1) {
        fail("cannot parse the URL of " + r.what + ": " + url);
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
    ReqCtxPtr rctx{OSSL_HTTP_open(h.c_str(), p.c_str(), nullptr, nullptr, use_ssl, nullptr,
                                  nullptr, use_ssl != 0 ? tls_wrap : nullptr,
                                  use_ssl != 0 ? &tls : nullptr, 0, r.timeout_seconds)};
    if (!rctx) {
        fail(r.what + " at " + url + " did not answer");
    }
    BioPtr body;
    if (r.post != nullptr) {
        body.reset(BIO_new_mem_buf(r.post->data(), static_cast<int>(r.post->size())));
        if (!body) {
            fail("cannot build the request to " + r.what);
        }
    }
    // A null request body makes this a GET.
    if (OSSL_HTTP_set1_request(rctx.get(), target.c_str(), nullptr,
                               r.post != nullptr ? r.content_type : nullptr, body.get(),
                               r.expected_type, r.expect_asn1 ? 1 : 0, r.max_size,
                               r.timeout_seconds, 0) != 1) {
        fail("cannot send the request to " + r.what);
    }
    char* location = nullptr;
    // OSSL_HTTP_exchange() hands back its own reference to the reply BIO:
    // ours to release with BIO_free (never BIO_free_all -- the chain beneath
    // a streamed reply is the connection, which the context frees on close).
    // Declared after rctx, so released before the context closes.
    struct Unref {
        void operator()(BIO* b) const noexcept { BIO_free(b); }
    };
    const std::unique_ptr<BIO, Unref> reply{
        OSSL_HTTP_exchange(rctx.get(), r.max_redirects > 0 ? &location : nullptr)};
    if (!reply) {
        if (location != nullptr) {
            *redirect = location;
            OPENSSL_free(location);
            ERR_clear_error();
            return {};
        }
        fail(r.what + " at " + url + " did not answer");
    }
    // Read before the context closes: a streamed reply is its connection.
    return read_reply(reply.get(), r, url);
}

}  // namespace

Bytes http_transfer(const HttpRequest& r) {
    std::string url = r.url;
    for (int hop = 0;; ++hop) {
        std::string redirect;
        Bytes out = one_hop(r, url, &redirect);
        if (redirect.empty()) {
            return out;
        }
        if (hop >= r.max_redirects) {
            throw Error(0, r.what + " at " + r.url + " redirects too often");
        }
        if (redirect.front() == '/') {
            // A path on the same server: keep its scheme, host and port.
            const std::size_t start = url.find("://");
            const std::size_t end = url.find('/', start + 3);
            redirect = url.substr(0, end) + redirect;
        }
        url = redirect;
    }
}

}  // namespace leht::crypto::detail
