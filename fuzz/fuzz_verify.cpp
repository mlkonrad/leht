// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fuzz target for signature verification.
//
// A signature's CMS blob is attacker-controlled: it is a string inside the
// document. It reaches OpenSSL's ASN.1 parser, and then our own walk over
// attributes, certificates and timestamp tokens. That is why verification runs
// inside the sandboxed worker -- and why it gets a fuzz target of its own.
//
// The invariant under test is the one verify_cms() promises: whatever the bytes
// are, it returns a report. It never throws, and never reads out of bounds.

#include "leht/crypto/crypto.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

/// The "document" the signature claims to cover: derived from the input, so a
/// fuzzer can hit the case where the digest actually matches.
std::size_t content_reader(const std::uint8_t* data, std::size_t size, std::uint8_t* buf,
                           std::size_t want, std::size_t* pos) {
    const std::size_t left = size > *pos ? size - *pos : 0;
    const std::size_t n = want < left ? want : left;
    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = static_cast<std::uint8_t>(data[*pos + i] ^ 0x5A);
    }
    *pos += n;
    return n;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const bool ready = [] {
        leht::crypto::init(/*load_config=*/false);
        leht::crypto::preload_algorithms();
        return true;
    }();
    (void)ready;
    if (size == 0) {
        return 0;
    }

    // A trust store from the same bytes: mostly nonsense, occasionally a
    // certificate, and either way another parser reached with hostile input.
    leht::crypto::TrustStore trust;
    try {
        (void)trust.add_pem(std::string(reinterpret_cast<const char*>(data), size));
    } catch (const std::exception&) {
        // Expected for almost every input.
    }

    const leht::crypto::Bytes der(data, data + size);
    std::size_t pos = 0;
    const auto report = leht::crypto::verify_cms(
        der,
        [&](std::uint8_t* buf, std::size_t want) {
            return content_reader(data, size, buf, want, &pos);
        },
        trust);
    // A report that says it is intact must have parsed a signer certificate.
    if (report.intact() && report.signer.subject.empty() && report.signer.issuer.empty()) {
        std::fprintf(stderr, "fuzz_verify: intact signature with no signer\n");
        std::abort();
    }
    return 0;
}
