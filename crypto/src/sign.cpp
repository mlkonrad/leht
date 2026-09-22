// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Signing a prepared file: the half of signing that holds the key.
#include "ossl.hpp"

#include "leht/error.hpp"

#include <openssl/err.h>

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace leht::crypto {

namespace {

using detail::fail;

void pread_all(int fd, unsigned char* p, std::size_t n, std::int64_t offset) {
    while (n > 0) {
        const ssize_t r = ::pread(fd, p, n, offset);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            throw Error(0, std::string("cannot read the file being signed: ") +
                               (r < 0 ? std::strerror(errno) : "unexpected end of file"));
        }
        p += r;
        n -= static_cast<std::size_t>(r);
        offset += r;
    }
}

void pwrite_all(int fd, const char* p, std::size_t n, std::int64_t offset) {
    while (n > 0) {
        const ssize_t w = ::pwrite(fd, p, n, offset);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            throw Error(0, std::string("cannot write the signature: ") + std::strerror(errno));
        }
        p += w;
        n -= static_cast<std::size_t>(w);
        offset += w;
    }
}

/// The check that makes the rest safe: the ranges cover the whole file but
/// the hole, and the hole is an empty hex string. Returns its capacity in DER
/// bytes. Reads the file, never the claim alone.
std::size_t check_hole(int fd, const ops::ByteRange& r) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        throw Error(0, std::string("cannot stat the file being signed: ") + std::strerror(errno));
    }
    const std::int64_t size = st.st_size;
    const auto& v = r.v;
    const bool shape = v[0] == 0 && v[1] > 0 && v[2] > v[0] + v[1] + 1 && v[3] >= 0 &&
                       v[2] <= size && v[3] <= size - v[2] && r.end() == size;
    if (!shape) {
        throw Error(0, "the signature's byte range does not cover the file around one hole");
    }
    const std::int64_t begin = r.hole_begin();
    const std::int64_t end = r.hole_end();
    const std::int64_t digits = end - begin - 2;
    if (digits < 2 || digits % 2 != 0) {
        throw Error(0, "the signature hole is not an even run of hex digits");
    }
    std::array<unsigned char, 8192> buf{};
    unsigned char c = 0;
    pread_all(fd, &c, 1, begin);
    if (c != '<') {
        throw Error(0, "the signature hole does not start with '<'");
    }
    pread_all(fd, &c, 1, end - 1);
    if (c != '>') {
        throw Error(0, "the signature hole does not end with '>'");
    }
    for (std::int64_t off = begin + 1; off < end - 1;) {
        const auto n = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(buf.size()), end - 1 - off));
        pread_all(fd, buf.data(), n, off);
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[i] != '0') {
                throw Error(0, "the signature hole is not empty");
            }
        }
        off += static_cast<std::int64_t>(n);
    }
    return static_cast<std::size_t>(digits / 2);
}

Bytes digest_ranges(int fd, const ops::ByteRange& r, const EVP_MD* md) {
    detail::MdCtxPtr ctx{EVP_MD_CTX_new()};
    if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) {
        fail("cannot start a digest");
    }
    std::array<unsigned char, 65536> buf{};
    for (int span = 0; span < 2; ++span) {
        std::int64_t off = r.v[static_cast<std::size_t>(span * 2)];
        std::int64_t left = r.v[static_cast<std::size_t>(span * 2 + 1)];
        while (left > 0) {
            const auto n = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(buf.size()), left));
            pread_all(fd, buf.data(), n, off);
            if (EVP_DigestUpdate(ctx.get(), buf.data(), n) != 1) {
                fail("digest failed");
            }
            off += static_cast<std::int64_t>(n);
            left -= static_cast<std::int64_t>(n);
        }
    }
    Bytes out(static_cast<std::size_t>(EVP_MD_get_size(md)));
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), out.data(), &len) != 1) {
        fail("digest failed");
    }
    out.resize(len);
    return out;
}

/// The digest to pair with the key: an ECDSA key's curve sets the hash size
/// that gives it its full strength (P-384 with SHA-384, as ID-card keys are).
const EVP_MD* digest_for(const Identity& id) {
    if (id.key_type() == KeyType::Ec) {
        if (id.key_bits() > 384) {
            return EVP_sha512();
        }
        if (id.key_bits() > 256) {
            return EVP_sha384();
        }
    }
    return EVP_sha256();
}

}  // namespace

std::size_t estimate_signature_size(const Identity& identity, const SignOptions& options) {
    std::size_t n = 0;
    for (const Bytes& der : identity.chain_der()) {
        n += der.size();
    }
    n += 2 * static_cast<std::size_t>(EVP_PKEY_get_size(identity.impl().key.get()));
    n += 2048;  // SignedData framing, signed attributes, the ESS certificate hash
    if (!options.tsa_url.empty()) {
        n += 12288;  // a timestamp token with the TSA's chain
    }
    return (n + 1023) / 1024 * 1024;
}

SignResult sign_prepared(int fd, const ops::ByteRange& range, const Identity& identity,
                         const SignOptions& options) {
    init();
    SignResult result;
    result.hole_size = check_hole(fd, range);

    const EVP_MD* md = digest_for(identity);
    result.digest = detail::digest_name(md);
    const Bytes digest = digest_ranges(fd, range, md);

    // PAdES baseline B-B: a detached CMS SignedData whose signed attributes
    // hold content-type, message-digest and ESS signing-certificate-v2
    // (CMS_CADES), and NOT signing-time: PAdES takes the claimed time from the
    // signature dictionary's /M, and forbids a second, possibly different one.
    constexpr unsigned kFlags = CMS_DETACHED | CMS_BINARY | CMS_PARTIAL | CMS_NOSMIMECAP |
                                CMS_CADES | CMS_NO_SIGNING_TIME;
    const Identity::Impl& id = identity.impl();
    detail::CmsPtr cms{CMS_sign(nullptr, nullptr, nullptr, nullptr, kFlags)};
    if (!cms) {
        fail("cannot create a CMS signature");
    }
    CMS_SignerInfo* si = CMS_add1_signer(cms.get(), id.cert.get(), id.key.get(), md, kFlags);
    if (si == nullptr) {
        fail("cannot add the signer");
    }
    for (const detail::X509Ptr& c : id.extra) {
        if (CMS_add1_cert(cms.get(), c.get()) != 1) {
            fail("cannot add a chain certificate");
        }
    }
    if (CMS_final_digest(cms.get(), digest.data(), static_cast<unsigned>(digest.size()),
                         nullptr, kFlags) != 1) {
        fail("signing failed");
    }

    if (!options.tsa_url.empty()) {
        // PAdES B-T: a timestamp over the signature value, as the unsigned
        // attribute id-aa-signatureTimeStampToken.
        const ASN1_OCTET_STRING* value = CMS_SignerInfo_get0_signature(si);
        const detail::TimestampToken token = detail::request_timestamp(
            options.tsa_url, ASN1_STRING_get0_data(value),
            static_cast<std::size_t>(ASN1_STRING_length(value)), options.tsa_timeout_seconds);
        if (CMS_unsigned_add1_attr_by_NID(si, NID_id_smime_aa_timeStampToken, V_ASN1_SEQUENCE,
                                          token.der.data(),
                                          static_cast<int>(token.der.size())) != 1) {
            fail("cannot attach the timestamp");
        }
        result.timestamp = token.time;
    }

    unsigned char* der = nullptr;
    const int n = i2d_CMS_ContentInfo(cms.get(), &der);
    if (n <= 0) {
        fail("cannot encode the signature");
    }
    const Bytes blob(der, der + n);
    OPENSSL_free(der);
    result.der_size = blob.size();
    if (blob.size() > result.hole_size) {
        throw Error(0, "the signature is " + std::to_string(blob.size()) +
                           " bytes but its hole holds " + std::to_string(result.hole_size));
    }
    const std::string hex = detail::hex(blob.data(), blob.size());
    pwrite_all(fd, hex.data(), hex.size(), range.hole_begin() + 1);
    return result;
}

}  // namespace leht::crypto
