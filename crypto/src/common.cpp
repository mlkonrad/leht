// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <array>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>

namespace leht::crypto {

namespace detail {

std::string drain_errors() {
    std::string out;
    unsigned long e = 0;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!out.empty()) {
            out += "; ";
        }
        out += buf;
    }
    return out;
}

void fail(const std::string& what) {
    const std::string why = drain_errors();
    throw Error(0, why.empty() ? what : what + " (" + why + ")");
}

X509Ptr up_ref(X509* cert) {
    if (cert != nullptr) {
        X509_up_ref(cert);
    }
    return X509Ptr{cert};
}

Bytes to_der(X509* cert) {
    unsigned char* out = nullptr;
    const int n = i2d_X509(cert, &out);
    if (n <= 0) {
        fail("cannot encode a certificate");
    }
    Bytes der(out, out + n);
    OPENSSL_free(out);
    return der;
}

X509Ptr from_der(const Bytes& der) {
    const unsigned char* p = der.data();
    X509Ptr cert{d2i_X509(nullptr, &p, static_cast<long>(der.size()))};
    if (!cert) {
        fail("not a DER certificate");
    }
    return cert;
}

std::string hex(const unsigned char* p, std::size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[p[i] >> 4];
        out += digits[p[i] & 0x0F];
    }
    return out;
}

std::int64_t to_unix(const ASN1_TIME* t) {
    std::tm tm{};
    if (t == nullptr || ASN1_TIME_to_tm(t, &tm) != 1) {
        return 0;
    }
    return static_cast<std::int64_t>(timegm(&tm));
}

std::string digest_name(const EVP_MD* md) {
    switch (EVP_MD_get_type(md)) {
        case NID_sha1: return "SHA-1";
        case NID_sha224: return "SHA-224";
        case NID_sha256: return "SHA-256";
        case NID_sha384: return "SHA-384";
        case NID_sha512: return "SHA-512";
        default: {
            const char* name = EVP_MD_get0_name(md);
            return name != nullptr ? name : "unknown";
        }
    }
}

namespace {

std::string name_string(const X509_NAME* name) {
    BioPtr bio{BIO_new(BIO_s_mem())};
    if (!bio || X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB) < 0) {
        return {};
    }
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio.get(), &data);
    return n > 0 ? std::string(data, static_cast<std::size_t>(n)) : std::string{};
}

std::string common_name(const X509_NAME* name) {
    const int i = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if (i < 0) {
        return {};
    }
    const ASN1_STRING* s = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, i));
    unsigned char* utf8 = nullptr;
    const int n = ASN1_STRING_to_UTF8(&utf8, s);
    if (n < 0) {
        return {};
    }
    std::string out(reinterpret_cast<char*>(utf8), static_cast<std::size_t>(n));
    OPENSSL_free(utf8);
    return out;
}

}  // namespace

CertInfo info(X509* cert) {
    CertInfo out;
    if (cert == nullptr) {
        return out;
    }
    out.subject = name_string(X509_get_subject_name(cert));
    out.common_name = common_name(X509_get_subject_name(cert));
    out.issuer = name_string(X509_get_issuer_name(cert));
    if (const ASN1_INTEGER* serial = X509_get0_serialNumber(cert); serial != nullptr) {
        out.serial = hex(ASN1_STRING_get0_data(serial),
                         static_cast<std::size_t>(ASN1_STRING_length(serial)));
    }
    out.not_before = to_unix(X509_get0_notBefore(cert));
    out.not_after = to_unix(X509_get0_notAfter(cert));
    std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
    unsigned int n = 0;
    if (X509_digest(cert, EVP_sha256(), md.data(), &n) == 1) {
        out.sha256 = hex(md.data(), n);
    }
    out.is_ca = X509_check_ca(cert) > 0;
    const std::uint32_t usage = X509_get_key_usage(cert);
    // UINT32_MAX means "no keyUsage extension", which restricts nothing.
    out.can_sign = usage == UINT32_MAX || (usage & (KU_DIGITAL_SIGNATURE | KU_NON_REPUDIATION)) != 0;
    ERR_clear_error();
    return out;
}

StorePtr make_store(const TrustStore& trust) {
    StorePtr store{X509_STORE_new()};
    if (!store) {
        fail("cannot create a certificate store");
    }
    for (const X509Ptr& c : trust.impl().certs) {
        (void)X509_STORE_add_cert(store.get(), c.get());  // a duplicate is not an error
    }
    ERR_clear_error();
    return store;
}

}  // namespace detail

void init(bool load_config) {
    static std::once_flag once;
    std::call_once(once, [load_config] {
        const std::uint64_t opts = load_config ? OPENSSL_INIT_LOAD_CONFIG
                                               : OPENSSL_INIT_NO_LOAD_CONFIG;
        if (OPENSSL_init_crypto(opts, nullptr) != 1) {
            detail::fail("cannot initialise OpenSSL");
        }
    });
}

void preload_algorithms() {
    init(false);
    for (const char* name : {"SHA1", "SHA2-224", "SHA2-256", "SHA2-384", "SHA2-512"}) {
        EVP_MD_free(EVP_MD_fetch(nullptr, name, nullptr));
    }
    for (const char* name : {"RSA", "RSA-PSS", "EC", "ED25519", "ED448"}) {
        EVP_KEYMGMT_free(EVP_KEYMGMT_fetch(nullptr, name, nullptr));
    }
    for (const char* name : {"RSA", "ECDSA", "ED25519", "ED448"}) {
        EVP_SIGNATURE_free(EVP_SIGNATURE_fetch(nullptr, name, nullptr));
    }
    ERR_clear_error();
}

// --- Secret ----------------------------------------------------------------

Secret::Secret(Secret&& other) noexcept : value_(std::move(other.value_)) {
    other.value_.clear();
}

Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) {
        OPENSSL_cleanse(value_.data(), value_.size());
        value_ = std::move(other.value_);
        other.value_.clear();
    }
    return *this;
}

Secret::~Secret() { OPENSSL_cleanse(value_.data(), value_.size()); }

// --- TrustStore ------------------------------------------------------------

TrustStore::TrustStore() : impl_(std::make_unique<Impl>()) {}
TrustStore::TrustStore(const TrustStore& other) : impl_(std::make_unique<Impl>()) {
    for (const detail::X509Ptr& c : other.impl_->certs) {
        impl_->certs.push_back(detail::up_ref(c.get()));
    }
}
TrustStore& TrustStore::operator=(const TrustStore& other) {
    if (this != &other) {
        TrustStore copy(other);
        std::swap(impl_, copy.impl_);
    }
    return *this;
}
TrustStore::TrustStore(TrustStore&&) noexcept = default;
TrustStore& TrustStore::operator=(TrustStore&&) noexcept = default;
TrustStore::~TrustStore() = default;

TrustStore TrustStore::system() {
    TrustStore store;
    for (const char* path : {"/etc/pki/tls/certs/ca-bundle.crt",   // Fedora, RHEL
                             "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Arch
                             "/etc/ssl/ca-bundle.pem",              // openSUSE
                             "/etc/ssl/cert.pem"}) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            continue;
        }
        const std::string pem{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        try {
            if (store.add_pem(pem) > 0) {
                break;
            }
        } catch (const Error&) {
            // A damaged bundle is not fatal: fall through to the next location.
        }
    }
    return store;
}

int TrustStore::add_pem(const std::string& pem) {
    detail::BioPtr bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
    if (!bio) {
        detail::fail("cannot read PEM");
    }
    int count = 0;
    for (;;) {
        X509* cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
        if (cert == nullptr) {
            break;
        }
        impl_->certs.emplace_back(cert);
        ++count;
    }
    // The loop always ends on "no start line"; anything else is damage.
    const unsigned long e = ERR_peek_last_error();
    const bool clean_end = e == 0 || ERR_GET_REASON(e) == PEM_R_NO_START_LINE;
    ERR_clear_error();
    if (!clean_end || (count == 0 && pem.find("-----BEGIN") != std::string::npos)) {
        throw Error(0, "the PEM text holds a damaged certificate");
    }
    return count;
}

void TrustStore::add_der(const Bytes& der) { impl_->certs.push_back(detail::from_der(der)); }

int TrustStore::size() const { return static_cast<int>(impl_->certs.size()); }

std::string TrustStore::pem() const {
    detail::BioPtr bio{BIO_new(BIO_s_mem())};
    for (const detail::X509Ptr& c : impl_->certs) {
        if (PEM_write_bio_X509(bio.get(), c.get()) != 1) {
            detail::fail("cannot write PEM");
        }
    }
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio.get(), &data);
    return n > 0 ? std::string(data, static_cast<std::size_t>(n)) : std::string{};
}

CertInfo describe_certificate(const Bytes& der) {
    const detail::X509Ptr cert = detail::from_der(der);
    return detail::info(cert.get());
}

}  // namespace leht::crypto
