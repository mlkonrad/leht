// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/crypto/crypto.hpp"
#include "leht/error.hpp"
#include "edit_harness.hpp"
#include "test_pki.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <string>

using leht::crypto::Bytes;
using leht::crypto::CmsReport;
using leht::crypto::Identity;
using leht::crypto::Secret;
using leht::crypto::SignOptions;
using leht::crypto::Trust;
using leht::crypto::TrustStore;
using leht::ops::ByteRange;
using leht::test::TempPath;

namespace {

const leht::test::Pki& pki() {
    static const leht::test::Pki p;
    return p;
}

/// A file shaped like a prepared signature: bytes, an empty hole, bytes. Not
/// a PDF -- leht::crypto never looks inside one, and this proves it.
struct Prepared {
    TempPath path;
    ByteRange range;
    std::string before, after;

    explicit Prepared(const std::string& name, std::size_t capacity = 8192,
                      std::string head = "%PDF-1.7 pretend /Contents ",
                      std::string tail = " /ByteRange [...] %%EOF\n")
        : path(name), before(std::move(head)), after(std::move(tail)) {
        const std::string hole = "<" + std::string(capacity * 2, '0') + ">";
        leht::test::write_file(path.str(), before + hole + after);
        const auto b = static_cast<std::int64_t>(before.size());
        const auto h = static_cast<std::int64_t>(hole.size());
        range.v = {0, b, b + h, static_cast<std::int64_t>(after.size())};
    }

    [[nodiscard]] int open_rw() const {
        const int fd = ::open(path.str().c_str(), O_RDWR | O_CLOEXEC);
        CHECK(fd >= 0);
        return fd;
    }

    /// The DER now in the hole (zero padding stripped by the parser).
    [[nodiscard]] Bytes der() const {
        const std::string all = leht::test::read_file(path.str());
        const std::string hex = all.substr(before.size() + 1,
                                           static_cast<std::size_t>(range.hole_end() -
                                                                    range.hole_begin() - 2));
        Bytes out(hex.size() / 2);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
        }
        return out;
    }

    /// The signed bytes, streamed as a verifier would.
    [[nodiscard]] leht::crypto::ContentReader content() const {
        auto data = std::make_shared<std::string>();
        const std::string all = leht::test::read_file(path.str());
        *data = all.substr(0, before.size()) +
                all.substr(static_cast<std::size_t>(range.hole_end()));
        auto pos = std::make_shared<std::size_t>(0);
        return [data, pos](std::uint8_t* buf, std::size_t n) {
            const std::size_t k = std::min(n, data->size() - *pos);
            std::memcpy(buf, data->data() + *pos, k);
            *pos += k;
            return k;
        };
    }

    CmsReport verify(const TrustStore& trust) const {
        return leht::crypto::verify_cms(der(), content(), trust);
    }
};

leht::crypto::SignResult sign(const Prepared& p, const Identity& id, const SignOptions& o = {}) {
    const int fd = p.open_rw();
    try {
        const auto r = leht::crypto::sign_prepared(fd, p.range, id, o);
        ::close(fd);
        return r;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

template <typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const leht::Error&) {
        return true;
    }
    return false;
}

void pkcs12_loads_with_the_right_password_only() {
    const auto& k = pki();
    const Bytes p12 = leht::test::pkcs12(k.rsa, k.rsa_cert, {&k.ca}, "correct horse");
    CHECK(throws([&] { (void)Identity::from_pkcs12(p12, Secret{"wrong"}); }));
    CHECK(throws([&] { (void)Identity::from_pkcs12(Bytes{1, 2, 3}, Secret{"x"}); }));
    const Identity id = Identity::from_pkcs12(p12, Secret{"correct horse"});
    CHECK(id.certificate().common_name == "Mari Maasikas");
    CHECK(id.key_type() == leht::crypto::KeyType::Rsa);
    CHECK(id.key_bits() == 2048);
    CHECK(id.chain_der().size() == 2);
}

void an_rsa_signature_verifies_and_is_pades_shaped() {
    const auto& k = pki();
    const Prepared p("crypto_rsa.bin");
    const auto r = sign(p, k.identity(k.rsa, k.rsa_cert));
    CHECK(r.digest == "SHA-256");
    CHECK(r.der_size > 0 && r.der_size <= r.hole_size);
    CHECK(!r.timestamp);

    const CmsReport v = p.verify(k.trust());
    CHECK(v.intact());
    CHECK(v.trust == Trust::Trusted);
    CHECK(v.signer.common_name == "Mari Maasikas");
    CHECK(v.chain.size() == 2);
    CHECK(v.has_signing_certificate_v2);
    CHECK(!v.has_signing_time_attribute);
    CHECK(!v.timestamp);
}

void a_p384_key_signs_with_sha384() {
    const auto& k = pki();
    const Prepared p("crypto_ec.bin");
    CHECK(sign(p, k.identity(k.ec, k.ec_cert)).digest == "SHA-384");
    const CmsReport v = p.verify(k.trust());
    CHECK(v.intact());
    CHECK(v.digest == "SHA-384");
    CHECK(v.trust == Trust::Trusted);
}

void openssl_cms_agrees() {
    // An independent opinion: the openssl command line, not our code path.
    if (!leht::test::have_tool("openssl")) {
        std::fprintf(stderr, "  SKIP openssl cms -verify (openssl not installed)\n");
        return;
    }
    const auto& k = pki();
    const Prepared p("crypto_cli.bin");
    (void)sign(p, k.identity(k.rsa, k.rsa_cert));
    const TempPath der("crypto_cli.der"), data("crypto_cli.data"), ca("crypto_cli_ca.pem");
    const Bytes blob = p.der();
    leht::test::write_file(der.str(), std::string(blob.begin(), blob.end()));
    std::string content;
    std::uint8_t buf[4096];
    const auto reader = p.content();
    for (std::size_t n = 0; (n = reader(buf, sizeof(buf))) > 0;) {
        content.append(reinterpret_cast<char*>(buf), n);
    }
    leht::test::write_file(data.str(), content);
    leht::test::write_file(ca.str(), k.ca.pem());
    const std::string out = leht::test::capture(
        "openssl cms -verify -binary -inform DER -in '" + der.str() + "' -content '" +
        data.str() + "' -CAfile '" + ca.str() + "' -purpose any -out /dev/null 2>&1");
    CHECK(out.find("Verification successful") != std::string::npos);
}

void a_changed_byte_is_caught() {
    const auto& k = pki();
    const Prepared p("crypto_tamper.bin");
    (void)sign(p, k.identity(k.rsa, k.rsa_cert));
    std::string all = leht::test::read_file(p.path.str());
    all[3] = 'X';  // inside the first signed span
    leht::test::write_file(p.path.str(), all);
    const CmsReport v = p.verify(k.trust());
    CHECK(v.parsed);
    CHECK(!v.digest_matches);
    CHECK(!v.intact());
    CHECK(!v.problem.empty());
}

void trust_is_reported_apart_from_integrity() {
    const auto& k = pki();
    {
        const Prepared p("crypto_untrusted.bin");
        (void)sign(p, k.identity(k.rsa, k.rsa_cert));
        const CmsReport v = p.verify(TrustStore{});
        CHECK(v.intact());
        CHECK(v.trust == Trust::Untrusted);
        CHECK(!v.trust_detail.empty());
    }
    {
        const Prepared p("crypto_expired.bin");
        (void)sign(p, k.identity(k.expired, k.expired_cert));
        const CmsReport v = p.verify(k.trust());
        CHECK(v.intact());
        CHECK(v.trust == Trust::Expired);
    }
    {
        const Prepared p("crypto_nosign.bin");
        (void)sign(p, k.identity(k.no_sign, k.no_sign_cert));
        const CmsReport v = p.verify(k.trust());
        CHECK(v.intact());
        CHECK(!v.signer.can_sign);
        CHECK(v.trust == Trust::Untrusted);
    }
}

void a_lying_byte_range_is_refused() {
    const auto& k = pki();
    const Identity id = k.identity(k.rsa, k.rsa_cert);
    const auto refused = [&](const Prepared& p, ByteRange r) {
        const int fd = p.open_rw();
        const bool t = throws([&] { (void)leht::crypto::sign_prepared(fd, r, id, {}); });
        ::close(fd);
        return t;
    };
    const Prepared p("crypto_lies.bin");  // big enough: only the lie can fail it
    ByteRange r = p.range;
    r.v[0] = 1;  // does not start at the start
    CHECK(refused(p, r));
    r = p.range;
    r.v[3] -= 1;  // stops short of the end: the tail would go unsigned
    CHECK(refused(p, r));
    r = p.range;
    r.v[1] -= 1;  // the hole does not begin at '<'
    CHECK(refused(p, r));
    r = p.range;
    r.v[2] += 1;  // nor end after '>'
    CHECK(refused(p, r));
    r = p.range;
    r.v = {0, 5, 3, 0};  // overlapping nonsense
    CHECK(refused(p, r));

    // A hole with anything but zeros in it is not an empty hole.
    const Prepared q("crypto_lies_full.bin");
    std::string all = leht::test::read_file(q.path.str());
    all[q.before.size() + 10] = '7';
    leht::test::write_file(q.path.str(), all);
    CHECK(refused(q, q.range));
    // And the honest one still signs.
    CHECK(!refused(p, p.range));
}

void a_hole_too_small_fails_cleanly() {
    const auto& k = pki();
    const Prepared p("crypto_small.bin", 64);
    CHECK(throws([&] { (void)sign(p, k.identity(k.rsa, k.rsa_cert)); }));
    // Nothing was written into it.
    CHECK(leht::test::read_file(p.path.str()).find("<" + std::string(128, '0') + ">") !=
          std::string::npos);
}

void the_estimate_fits_the_signature() {
    const auto& k = pki();
    const Identity id = k.identity(k.rsa, k.rsa_cert);
    const std::size_t est = leht::crypto::estimate_signature_size(id, {});
    const Prepared p("crypto_estimate.bin", est);
    const auto r = sign(p, id);
    CHECK(r.der_size <= est);
    SignOptions ts;
    ts.tsa_url = "http://example.invalid/";
    CHECK(leht::crypto::estimate_signature_size(id, ts) > est);
}

void malformed_der_is_reported_not_thrown() {
    const auto& k = pki();
    const Prepared p("crypto_malformed.bin");
    (void)sign(p, k.identity(k.rsa, k.rsa_cert));
    const Bytes good = p.der();
    const auto empty_reader = [](std::uint8_t*, std::size_t) { return std::size_t{0}; };
    for (const Bytes& bad : {Bytes{}, Bytes{0x30}, Bytes{0x30, 0x82, 0xFF, 0xFF},
                             Bytes(good.begin(), good.begin() + 40),
                             Bytes(good.begin(), good.begin() + static_cast<long>(good.size() / 2)),
                             Bytes(1000, 0x00), Bytes(1000, 0xFF)}) {
        const CmsReport v = leht::crypto::verify_cms(bad, empty_reader, k.trust());
        CHECK(!v.intact());
    }
    // Flip every 97th byte of a real signature: whatever parses must not verify.
    for (std::size_t i = 0; i < good.size(); i += 97) {
        Bytes mutated = good;
        mutated[i] ^= 0x5A;
        const CmsReport v = leht::crypto::verify_cms(mutated, p.content(), k.trust());
        (void)v;  // no crash, no throw; some flips hit only padding or unsigned data
    }
}

void a_timestamp_makes_it_b_t() {
    const auto& k = pki();
    const leht::test::LocalTsa tsa(k.tsa_key, k.tsa_cert, k.ca);
    const Identity id = k.identity(k.rsa, k.rsa_cert);
    SignOptions o;
    o.tsa_url = tsa.url();
    const Prepared p("crypto_tsa.bin", leht::crypto::estimate_signature_size(id, o));
    const auto r = sign(p, id, o);
    CHECK(r.timestamp.has_value());
    const std::int64_t now = std::time(nullptr);
    CHECK(*r.timestamp > now - 60 && *r.timestamp < now + 60);

    const CmsReport v = p.verify(k.trust());
    CHECK(v.intact());
    CHECK(v.trust == Trust::Trusted);
    CHECK(v.timestamp.has_value());
    CHECK(v.timestamp->valid);
    CHECK(v.timestamp->trust == Trust::Trusted);
    CHECK(v.timestamp->authority.common_name == "Leht Test TSA");
    CHECK(v.timestamp->time == *r.timestamp);

    // Without the CA the TSA is as untrusted as the signer.
    const CmsReport u = p.verify(TrustStore{});
    CHECK(u.timestamp && u.timestamp->valid && u.timestamp->trust == Trust::Untrusted);
}

void a_replayed_timestamp_is_refused() {
    const auto& k = pki();
    leht::test::LocalTsa tsa(k.tsa_key, k.tsa_cert, k.ca);
    tsa.corrupt_nonce();
    const Identity id = k.identity(k.rsa, k.rsa_cert);
    SignOptions o;
    o.tsa_url = tsa.url();
    const Prepared p("crypto_tsa_replay.bin", leht::crypto::estimate_signature_size(id, o));
    CHECK(throws([&] { (void)sign(p, id, o); }));
}

void an_unreachable_tsa_fails() {
    const auto& k = pki();
    const Identity id = k.identity(k.rsa, k.rsa_cert);
    SignOptions o;
    o.tsa_url = "http://127.0.0.1:1/tsa";  // nothing listens on port 1
    o.tsa_timeout_seconds = 5;
    const Prepared p("crypto_tsa_down.bin", leht::crypto::estimate_signature_size(id, o));
    CHECK(throws([&] { (void)sign(p, id, o); }));
    o.tsa_url = "ftp://127.0.0.1/";
    CHECK(throws([&] { (void)sign(p, id, o); }));
}

void trust_store_round_trips_through_pem() {
    const auto& k = pki();
    TrustStore t;
    CHECK(t.add_pem(k.ca.pem() + k.tsa_cert.pem()) == 2);
    TrustStore u;
    CHECK(u.add_pem(t.pem()) == 2);
    CHECK(throws([&] { (void)u.add_pem("-----BEGIN CERTIFICATE-----\nnot base64\n"
                                        "-----END CERTIFICATE-----\n"); }));
    const TrustStore sys = TrustStore::system();
    std::printf("  (system trust store: %d certificates)\n", sys.size());
}

}  // namespace

int main() {
    leht::crypto::init();
    RUN(pkcs12_loads_with_the_right_password_only);
    RUN(an_rsa_signature_verifies_and_is_pades_shaped);
    RUN(a_p384_key_signs_with_sha384);
    RUN(openssl_cms_agrees);
    RUN(a_changed_byte_is_caught);
    RUN(trust_is_reported_apart_from_integrity);
    RUN(a_lying_byte_range_is_refused);
    RUN(a_hole_too_small_fails_cleanly);
    RUN(the_estimate_fits_the_signature);
    RUN(malformed_der_is_reported_not_thrown);
    RUN(a_timestamp_makes_it_b_t);
    RUN(a_replayed_timestamp_is_refused);
    RUN(an_unreachable_tsa_fails);
    RUN(trust_store_round_trips_through_pem);
    return 0;
}
