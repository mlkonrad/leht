// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The one HTTP client in Leht: timestamp authorities, OCSP responders, CRL
// distribution points, the EU trusted lists -- and, over HTTP/1.1, SK's
// Smart-ID and Mobile-ID. Trusted process only: the worker has no network.
//
// Deliberately small: http:// and https:// only, one request per connection,
// no redirects (OSSL_HTTP_transfer does not follow them), a cap on the reply,
// and a timeout. What comes back is not looked at here.
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/err.h>
#include <openssl/http.h>
#include <openssl/ssl.h>

#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>

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

/// Writing to a connection the server has already closed raises SIGPIPE,
/// which kills the process -- the CLI or the viewer -- unless someone ignores
/// it. A library must not change that for the whole process, so each
/// exchange blocks SIGPIPE on its own thread, and takes back any it caused
/// before unblocking: the write then just fails with EPIPE.
class NoSigpipe {
public:
    NoSigpipe() {
        sigemptyset(&pipe_);
        sigaddset(&pipe_, SIGPIPE);
        sigset_t pending;
        sigpending(&pending);
        // One already pending is someone else's: leave it alone.
        was_pending_ = sigismember(&pending, SIGPIPE) == 1;
        blocked_ = pthread_sigmask(SIG_BLOCK, &pipe_, &old_) == 0;
    }
    NoSigpipe(const NoSigpipe&) = delete;
    NoSigpipe& operator=(const NoSigpipe&) = delete;
    ~NoSigpipe() {
        if (!blocked_) {
            return;
        }
        if (!was_pending_) {
            const timespec zero{0, 0};
            while (sigtimedwait(&pipe_, nullptr, &zero) == SIGPIPE) {
            }
        }
        pthread_sigmask(SIG_SETMASK, &old_, nullptr);
    }

private:
    sigset_t pipe_{};
    sigset_t old_{};
    bool was_pending_ = false;
    bool blocked_ = false;
};

/// A reply's body, "chunked" as HTTP/1.1 allows; nullopt until it is whole.
std::optional<Bytes> dechunk(const unsigned char* p, std::size_t n) {
    Bytes out;
    std::size_t at = 0;
    for (;;) {
        const unsigned char* eol = static_cast<const unsigned char*>(
            std::memchr(p + at, '\n', n - std::min(at, n)));
        if (at >= n || eol == nullptr) {
            return std::nullopt;
        }
        // The size in hex, perhaps with ";extensions".
        std::size_t size = 0;
        std::size_t i = at;
        int digits = 0;
        for (; i < n && std::isxdigit(p[i]) != 0; ++i, ++digits) {
            const int c = p[i];
            size = size * 16 + static_cast<std::size_t>(std::isdigit(c) != 0 ? c - '0'
                                                                             : (c | 0x20) - 'a' + 10);
            if (digits > 12) {
                throw Error(0, "a chunk too large");
            }
        }
        if (digits == 0) {
            throw Error(0, "a malformed chunked reply");
        }
        at = static_cast<std::size_t>(eol - p) + 1;
        if (size == 0) {
            return out;  // trailers, if any, do not matter
        }
        if (n - at < size + 2) {
            return std::nullopt;
        }
        out.insert(out.end(), p + at, p + at + size);
        at += size + 2;  // the chunk and its CRLF
    }
}

}  // namespace

std::string spki_pin(X509* cert) {
    unsigned char* der = nullptr;
    const int n = i2d_PUBKEY(X509_get0_pubkey(cert), &der);
    if (n <= 0) {
        ERR_clear_error();
        return {};
    }
    const Bytes hash = sha256(der, static_cast<std::size_t>(n));
    OPENSSL_free(der);
    unsigned char out[64];
    const int len = EVP_EncodeBlock(out, hash.data(), static_cast<int>(hash.size()));
    return {reinterpret_cast<const char*>(out), static_cast<std::size_t>(len)};
}

HttpReply http11(const HttpRequest& r) {
    const NoSigpipe quiet;
    int use_ssl = 0;
    char* host = nullptr;
    char* port = nullptr;
    char* path = nullptr;
    char* query = nullptr;
    if ((r.url.rfind("https://", 0) != 0 && r.url.rfind("http://", 0) != 0) ||
        OSSL_HTTP_parse_url(r.url.c_str(), &use_ssl, nullptr, &host, &port, nullptr, &path,
                            &query, nullptr) != 1) {
        OPENSSL_free(host);
        OPENSSL_free(port);
        OPENSSL_free(path);
        OPENSSL_free(query);
        throw Error(0, r.what + " must be an http:// or https:// URL, not " + r.url);
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
    const int timeout = r.timeout_seconds > 0 ? r.timeout_seconds : 60;
    const time_t deadline = std::time(nullptr) + timeout;
    const std::string where = r.what + " at " + r.url;
    if (r.pins != nullptr && use_ssl == 0 && h != "127.0.0.1" && h != "localhost" &&
        h != "[::1]" && h != "::1") {
        // A pinned service over plain HTTP would pin nothing.
        throw Error(0, r.what + " must be reached over https://, not " + r.url);
    }

    // Connect with a timeout, then block with one on every read and write.
    BioPtr bio{BIO_new_connect((h + ":" + p).c_str())};
    if (!bio || BIO_set_nbio(bio.get(), 1) != 1 ||
        BIO_do_connect_retry(bio.get(), timeout, 100) != 1) {
        ERR_clear_error();
        throw Error(0, where + " did not answer");
    }
    int fd = -1;
    BIO_get_fd(bio.get(), &fd);
    const timeval tv{timeout, 0};
    if (fd < 0 || BIO_socket_nbio(fd, 0) != 1 ||
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        fail("cannot set up the connection to " + r.what);
    }
    SslCtxPtr ssl_ctx;
    if (use_ssl != 0) {
        ssl_ctx.reset(SSL_CTX_new(TLS_client_method()));
        if (!ssl_ctx || SSL_CTX_set_default_verify_paths(ssl_ctx.get()) != 1) {
            fail("cannot set up TLS for " + r.what);
        }
        SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_PEER, nullptr);
        // The body's length is known from the reply; a server that then just
        // closes, without TLS's goodbye, has still said everything.
        SSL_CTX_set_options(ssl_ctx.get(), SSL_OP_IGNORE_UNEXPECTED_EOF);
        TlsArg tls;
        tls.host = h;
        tls.ctx = ssl_ctx.get();
        BIO* chain = tls_wrap(bio.get(), &tls, 1, 1);
        if (chain == nullptr) {
            fail("cannot set up TLS for " + r.what);
        }
        (void)bio.release();
        bio.reset(chain);
        if (BIO_do_handshake(bio.get()) != 1) {
            fail("TLS with " + where + " failed");
        }
        if (r.pins != nullptr) {
            // Verified already against the system's CAs, with the host name
            // checked; now it must also be a chain Leht expects for this
            // service. Any certificate in it may match: the pins are CA keys,
            // which outlive the server's own certificate.
            SSL* ssl = nullptr;
            BIO_get_ssl(bio.get(), &ssl);
            STACK_OF(X509)* verified = ssl != nullptr ? SSL_get0_verified_chain(ssl) : nullptr;
            bool pinned = false;
            for (int i = 0; verified != nullptr && !pinned && i < sk_X509_num(verified); ++i) {
                const std::string pin = spki_pin(sk_X509_value(verified, i));
                pinned = std::find(r.pins->begin(), r.pins->end(), pin) != r.pins->end();
            }
            if (!pinned) {
                throw Error(0, r.what + " at " + h +
                                   " presented a certificate from a certificate authority Leht "
                                   "does not expect for it, so nothing was sent. Either the "
                                   "connection is being intercepted, or the service has changed "
                                   "its certificate authority and Leht needs updating.");
            }
        }
    }

    std::string head = (r.post != nullptr ? "POST " : "GET ") + target + " HTTP/1.1\r\nHost: " + h +
                       "\r\nUser-Agent: Leht\r\nAccept: application/json\r\nConnection: close\r\n";
    if (r.post != nullptr) {
        head += std::string("Content-Type: ") +
                (r.content_type != nullptr ? r.content_type : "application/octet-stream") +
                "\r\nContent-Length: " + std::to_string(r.post->size()) + "\r\n";
    }
    head += "\r\n";
    Bytes request(head.begin(), head.end());
    if (r.post != nullptr) {
        request.insert(request.end(), r.post->begin(), r.post->end());
    }
    for (std::size_t sent = 0; sent < request.size();) {
        std::size_t n = 0;
        if (BIO_write_ex(bio.get(), request.data() + sent, request.size() - sent, &n) != 1) {
            fail("cannot send the request to " + where);
        }
        sent += n;
    }
    (void)BIO_flush(bio.get());

    // Read until the reply is whole (by Content-Length, or the last chunk),
    // or the server closes.
    Bytes in;
    std::array<unsigned char, 16384> buf{};
    HttpReply reply;
    std::size_t body_at = 0;
    std::optional<std::size_t> length;
    bool chunked = false;
    for (;;) {
        if (body_at == 0) {
            static const char kEnd[] = "\r\n\r\n";
            const auto it = std::search(in.begin(), in.end(), kEnd, kEnd + 4);
            if (it != in.end()) {
                body_at = static_cast<std::size_t>(it - in.begin()) + 4;
                std::string headers(in.begin(), it);
                // "HTTP/1.1 200 OK"
                if (headers.rfind("HTTP/1.", 0) != 0 || headers.size() < 12) {
                    throw Error(0, where + " did not answer in HTTP");
                }
                reply.status = std::atoi(headers.c_str() + 9);
                for (char& c : headers) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (const std::size_t cl = headers.find("\ncontent-length:"); cl != std::string::npos) {
                    length = std::strtoull(headers.c_str() + cl + 16, nullptr, 10);
                }
                chunked = headers.find("\ntransfer-encoding: chunked") != std::string::npos;
            }
        }
        if (body_at != 0) {
            if (chunked) {
                if (auto body = dechunk(in.data() + body_at, in.size() - body_at)) {
                    reply.body = std::move(*body);
                    break;
                }
            } else if (length && in.size() - body_at >= *length) {
                reply.body.assign(in.begin() + static_cast<std::ptrdiff_t>(body_at),
                                  in.begin() + static_cast<std::ptrdiff_t>(body_at + *length));
                break;
            }
        }
        std::size_t n = 0;
        if (BIO_read_ex(bio.get(), buf.data(), buf.size(), &n) != 1 || n == 0) {
            if (BIO_should_retry(bio.get()) != 0 && std::time(nullptr) < deadline) {
                continue;
            }
            ERR_clear_error();
            if (body_at != 0 && !chunked && !length) {
                // No length given: the body is what came before the close.
                reply.body.assign(in.begin() + static_cast<std::ptrdiff_t>(body_at), in.end());
                break;
            }
            throw Error(0, where + (std::time(nullptr) >= deadline ? " took too long to answer"
                                                                    : " broke off"));
        }
        in.insert(in.end(), buf.data(), buf.data() + n);
        if (in.size() > r.max_size + 65536) {
            throw Error(0, where + " sent more than " + std::to_string(r.max_size) + " bytes");
        }
        if (std::time(nullptr) > deadline) {
            throw Error(0, where + " took too long to answer");
        }
    }
    if (reply.body.size() > r.max_size) {
        throw Error(0, where + " sent more than " + std::to_string(r.max_size) + " bytes");
    }
    ERR_clear_error();
    return reply;
}

Bytes http_transfer(const HttpRequest& r) {
    const NoSigpipe quiet;
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
