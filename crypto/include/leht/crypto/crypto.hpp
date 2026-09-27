// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// leht::crypto -- the cryptography behind signing and verification.
//
// OpenSSL (libcrypto) is linked PRIVATE and no header here names an OpenSSL
// type, the same rule core/ keeps for MuPDF. This library never parses a PDF:
// it sees byte ranges of a file, and DER.
//
// Who runs what matters more than usual here:
//   * SIGNING runs in the trusted process (the viewer or the CLI), because it
//     holds the private key. It never parses the PDF; it checks one structural
//     fact about the file (the signed ranges cover every byte but the hole) and
//     signs those bytes. See sign_prepared().
//   * VERIFICATION parses hostile DER from the document, so the viewer runs it
//     inside the sandboxed worker. See verify_cms().

#include "leht/ops/sign.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace leht::crypto {

using Bytes = std::vector<std::uint8_t>;

/// Initialises OpenSSL once. With `load_config` false the system openssl.cnf
/// is not read: the sandboxed worker must not open files, and the viewer's
/// verification does not need crypto-policy tweaks. Safe to call repeatedly;
/// the first call decides.
void init(bool load_config = true);

/// Loads every algorithm verification can need, so nothing is fetched lazily
/// later. The worker calls it before its sandbox goes on: a lazy provider load
/// would try to open a file, and seccomp kills the process for that.
void preload_algorithms();

/// A string that is wiped from memory when it goes away: passwords and PINs.
class Secret {
public:
    Secret() = default;
    /// Takes `value` and wipes what the move leaves behind in it. A caller's
    /// own copy is the caller's to wipe.
    explicit Secret(std::string value);
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;
    ~Secret();

    [[nodiscard]] const std::string& str() const noexcept { return value_; }

private:
    std::string value_;
};

/// What a certificate says about itself. Times are Unix seconds (UTC).
struct CertInfo {
    std::string subject;      ///< RFC 2253, e.g. "CN=Mari Maasikas,O=...,C=EE"
    std::string common_name;  ///< CN alone, for display; empty when absent
    std::string issuer;
    std::string serial;       ///< hex
    std::int64_t not_before = 0;
    std::int64_t not_after = 0;
    std::string sha256;       ///< fingerprint of the DER, hex
    bool is_ca = false;
    /// digitalSignature or nonRepudiation (or no keyUsage extension at all).
    bool can_sign = true;
};

enum class KeyType { Rsa, Ec, Other };

/// A signing key on a PKCS#11 token -- an ID-card through OpenSC, or any
/// smartcard or HSM -- as list_token_keys() finds it, before any PIN.
struct TokenKey {
    std::string uri;          ///< RFC 7512 pkcs11: URI; pass it to Identity::from_pkcs11
    std::string token_label;  ///< e.g. "ESTEID (PIN2)"
    std::string key_label;
    CertInfo cert;
    /// keyUsage nonRepudiation: a key meant for signatures, not for logging in.
    /// An Estonian ID card has one of each; this is the PIN2 one.
    bool non_repudiation = false;
    bool pinpad = false;         ///< the PIN is entered on the reader, not typed here
    bool pin_count_low = false;  ///< a wrong PIN has been entered since the last good one
    bool pin_final_try = false;  ///< one more wrong PIN blocks it
    bool pin_locked = false;
};

/// The keys on every token present: each certificate that can sign, with the
/// token it is on. Tokens come from the PKCS#11 modules p11-kit has registered
/// (OpenSC registers itself), or from `module_path` alone when given. Signing
/// keys (nonRepudiation) come first. Needs no PIN, and returns an empty list,
/// not an error, when there is no reader or no card.
[[nodiscard]] std::vector<TokenKey> list_token_keys(const std::string& module_path = {});

/// A signing key and its certificate chain.
///
/// The key is either in memory, read from a PKCS#12 file, or stays on a
/// PKCS#11 token, which then computes the signature value itself. Nothing that
/// uses an Identity needs to know which.
class Identity {
public:
    /// Reads a .p12/.pfx. Throws leht::Error on a wrong password, a file with
    /// no key or no certificate matching it, or one encrypted with algorithms
    /// only OpenSSL's legacy provider still reads (old RC2-40 exports).
    static Identity from_pkcs12(const Bytes& p12, const Secret& password);

    /// Asked for the PIN once the token is found, with what is known about it:
    /// whether the reader has a keypad (then the result is not used -- say
    /// "enter the PIN on the reader" instead) and whether the PIN is close to
    /// blocking. Throw from it to cancel.
    using PinSource = std::function<Secret(const TokenKey& key)>;

    /// Logs in to the token `uri` names (a TokenKey::uri) and finds the private
    /// key there and the certificate with the same CKA_ID. The Identity keeps
    /// the PIN until it is destroyed, because a signing key may demand it
    /// again for each signature (CKA_ALWAYS_AUTHENTICATE, as ID-card PIN2 keys
    /// do). Throws leht::Error with a message meant for the person holding the
    /// card: a wrong PIN and how close it is to blocking, a blocked PIN, no
    /// such key, a card that was removed.
    static Identity from_pkcs11(const std::string& uri, const PinSource& pin,
                                const std::string& module_path = {});

    /// True when the key is on a token; false for a PKCS#12 key.
    [[nodiscard]] bool on_token() const;

    Identity(Identity&&) noexcept;
    Identity& operator=(Identity&&) noexcept;
    ~Identity();

    [[nodiscard]] CertInfo certificate() const;
    /// The signer's certificate first, then any others the file carried.
    [[nodiscard]] std::vector<Bytes> chain_der() const;
    [[nodiscard]] KeyType key_type() const;
    /// Bits of the key (2048 for RSA-2048, 384 for P-384).
    [[nodiscard]] int key_bits() const;

    struct Impl;
    [[nodiscard]] const Impl& impl() const { return *impl_; }

private:
    explicit Identity(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Certificates a verification may chain to.
class TrustStore {
public:
    TrustStore();
    TrustStore(const TrustStore&);
    TrustStore& operator=(const TrustStore&);
    TrustStore(TrustStore&&) noexcept;
    TrustStore& operator=(TrustStore&&) noexcept;
    ~TrustStore();

    /// The system's CA bundle (Fedora's /etc/pki/tls/certs/ca-bundle.crt, or
    /// the Debian location). Empty, not an error, when none is found.
    static TrustStore system();

    /// Adds every certificate in `pem`. Returns how many were read; throws
    /// leht::Error on text that is not PEM certificates.
    int add_pem(const std::string& pem);
    /// Adds one DER certificate.
    void add_der(const Bytes& der);

    [[nodiscard]] int size() const;
    /// All certificates as PEM: how the viewer hands the store to the worker.
    [[nodiscard]] std::string pem() const;

    struct Impl;
    [[nodiscard]] const Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// --- signing ---------------------------------------------------------------

struct SignOptions {
    /// RFC 3161 timestamp authority. Empty: PAdES B-B, no network at all.
    /// Set: B-T, the signature value is timestamped over HTTP(S).
    std::string tsa_url;
    int tsa_timeout_seconds = 20;
};

/// Hole size (bytes of DER, before hex doubling) to reserve for a signature
/// by `identity`: its chain, the signature, the signed attributes, and room
/// for a timestamp token when one is asked for. Generous on purpose: an
/// unused hole is zeros, while a hole too small costs a whole re-sign.
[[nodiscard]] std::size_t estimate_signature_size(const Identity& identity,
                                                  const SignOptions& options);

struct SignResult {
    std::size_t der_size = 0;
    std::size_t hole_size = 0;  ///< capacity in DER bytes
    std::string digest;         ///< "SHA-256", "SHA-384", ...
    /// The TSA's time (Unix seconds), when a timestamp was obtained.
    std::optional<std::int64_t> timestamp;
};

/// Signs a file the worker (or ops::prepare_signature) wrote with an empty
/// hole, in place: `fd` must be open read-write.
///
/// `range` is only a claim. Before anything is signed it is checked against the
/// bytes of the file itself: offset0 is 0, '<' opens the hole and '>' closes
/// it, only '0' lies between, and the second span ends exactly at the end of
/// the file. So the signed bytes are the whole file except the hole, whatever
/// the process that wrote it says. Throws leht::Error when the check fails,
/// the TSA fails, or the signature does not fit the hole.
SignResult sign_prepared(int fd, const ops::ByteRange& range, const Identity& identity,
                         const SignOptions& options);

// --- verification ----------------------------------------------------------

enum class Trust {
    Trusted,      ///< chains to the trust store, and was valid at the signing time
    Untrusted,    ///< a valid chain could not be built to anything trusted
    Expired,      ///< the signer's (or a chain) certificate had expired by then
    NotYetValid,
    Unknown,      ///< not evaluated: the signature itself did not verify
    Revoked,      ///< a certificate in the chain was revoked before the signing time
};

// --- long-term validation (PAdES B-LT) ---------------------------------------

/// Validation data, all DER: what a PDF's /DSS holds (ops::read_dss), or what
/// was fetched. The same type core writes, so nothing converts between them.
using RevocationData = ops::ValidationData;

enum class RevocationStatus {
    Good,     ///< revocation data valid at the time that matters says "not revoked"
    Revoked,  ///< revoked before that time
    Unknown,  ///< no usable data: none about it, too old, or not signed by its issuer
};

/// One certificate of a chain, checked for revocation at the trusted time: the
/// signature's timestamp when it has a valid one, else the verification time.
struct RevocationCheck {
    CertInfo cert;
    RevocationStatus status = RevocationStatus::Unknown;
    /// Where the answer came from: "OCSP" or "CRL", then "embedded" (the
    /// document's /DSS) or "fetched now".
    std::string source;
    /// When the data used was issued (OCSP thisUpdate, CRL lastUpdate).
    std::int64_t data_time = 0;
    /// When it was revoked: before the trusted time for Revoked; for Good, a
    /// revocation AFTER that time, which does not undo the signature.
    std::int64_t revoked_at = 0;
    std::string problem;  ///< for Unknown: why no data could be used
};

struct TimestampReport {
    bool valid = false;         ///< token signature and imprint check out
    std::int64_t time = 0;      ///< genTime, Unix seconds
    CertInfo authority;
    Trust trust = Trust::Unknown;
    std::string problem;        ///< why it is not valid or not trusted
    /// The authority's chain, checked at genTime. Empty when no revocation
    /// data at all was given to the verification.
    std::vector<RevocationCheck> revocation;
};

struct CmsReport {
    /// False when the blob is not a CMS SignedData with exactly one signer;
    /// `problem` says why, and nothing below is meaningful.
    bool parsed = false;
    std::string problem;

    std::string digest;             ///< algorithm, e.g. "SHA-256"
    bool digest_matches = false;    ///< message-digest attribute == digest of the ranges
    bool signature_valid = false;   ///< the signer's signature over the signed attributes
    CertInfo signer;
    std::vector<CertInfo> chain;    ///< as built for the trust decision, signer first
    Trust trust = Trust::Unknown;
    std::string trust_detail;       ///< OpenSSL's reason, when not Trusted

    /// PAdES baseline requires signing-certificate-v2 and forbids the CMS
    /// signing-time attribute (the claimed time is the dictionary's /M).
    bool has_signing_certificate_v2 = false;
    bool has_signing_time_attribute = false;
    std::optional<TimestampReport> timestamp;
    /// The signer's chain (the trust anchor left out), checked for revocation
    /// at the trusted time. Empty when no revocation data at all was given:
    /// then revocation was not checked, which is not the same as Unknown.
    std::vector<RevocationCheck> revocation;

    /// Integrity: the bytes are what was signed, by the key in the certificate.
    [[nodiscard]] bool intact() const { return parsed && digest_matches && signature_valid; }
};

/// Streams the signed bytes: called repeatedly with a buffer to fill; returns
/// the bytes written, 0 at the end. Throws to abort.
using ContentReader = std::function<std::size_t(std::uint8_t* buffer, std::size_t size)>;

/// Verifies a detached CMS signature over the bytes `content` yields.
///
/// `der` is attacker-controlled: it comes out of the document. Every failure is
/// reported in the result, never thrown; this only throws if `content` does.
/// The trust decision is made at the verified timestamp's time when there is
/// one, else at `now` (Unix seconds; 0 means the current time), and OpenSSL
/// is asked about chains.
///
/// Revocation is checked only against the data given: `embedded` (the
/// document's /DSS) and `online` (fetched now, see fetch_revocation()). Their
/// certificates also help build the chain. With neither, `revocation` in the
/// report stays empty: not checked. A certificate revoked before the trusted
/// time makes the trust Revoked; one revoked after it does not.
[[nodiscard]] CmsReport verify_cms(const Bytes& der, const ContentReader& content,
                                   const TrustStore& trust, std::int64_t now = 0,
                                   const RevocationData& embedded = {},
                                   const RevocationData& online = {});

/// Verifies a document timestamp (/SubFilter /ETSI.RFC3161): `der` is an RFC
/// 3161 TimeStampToken whose imprint must be the digest of the bytes `content`
/// yields. Attacker-controlled like verify_cms(), and likewise reports rather
/// than throws. The authority is trusted at the token's own time.
[[nodiscard]] TimestampReport verify_document_timestamp(const Bytes& der,
                                                        const ContentReader& content,
                                                        const TrustStore& trust,
                                                        const RevocationData& embedded = {},
                                                        const RevocationData& online = {});

/// Certificates as seen in a DER blob, for the details view.
[[nodiscard]] CertInfo describe_certificate(const Bytes& der);

// --- gathering validation data ------------------------------------------------
//
// Three steps, split so that the viewer's sandboxed worker does the parsing and
// the viewer only moves bytes: revocation_queries() (worker) says what to fetch,
// fetch_revocation() (trusted side, the only step with network) fetches it
// without looking inside, and validation_data() (worker) picks what to embed.

struct RevocationQuery {
    enum class Kind : std::uint8_t { Ocsp, Crl };
    Kind kind = Kind::Ocsp;
    std::string url;   ///< http:// or https://, from the certificate itself
    Bytes request;     ///< the OCSP request (POSTed); empty for a CRL (GET)
    std::string subject;  ///< whose status this asks, for display
};

/// What to fetch to check every certificate in the chains of `signatures`
/// (each a CMS signature or a timestamp token, as DER), the trust anchors left
/// out: an OCSP request for each certificate whose issuer is known and that
/// names a responder (AIA), and its CRL distribution points. A signature's own
/// timestamp authority is included. Hostile DER in, never throws on it.
[[nodiscard]] std::vector<RevocationQuery> revocation_queries(
    const std::vector<Bytes>& signatures, const TrustStore& trust,
    const RevocationData& embedded = {});

struct FetchedRevocation {
    RevocationQuery::Kind kind = RevocationQuery::Kind::Ocsp;
    std::string url;
    Bytes body;         ///< empty when the fetch failed
    std::string error;  ///< why it failed
};

/// Network: POSTs each OCSP request and GETs each CRL, one at a time, with no
/// redirects, http(s) only, and a size cap (1 MB for OCSP, 16 MB for a CRL).
/// Never looks inside what it fetches. A failure is reported per query.
[[nodiscard]] std::vector<FetchedRevocation> fetch_revocation(
    const std::vector<RevocationQuery>& queries, int timeout_seconds = 20);

/// What to embed for `signatures`: their chain certificates (and any OCSP
/// responder's) and every fetched response that parses -- an OCSP response
/// with status successful, or a CRL -- leaving out what `embedded` has already.
[[nodiscard]] RevocationData validation_data(const std::vector<Bytes>& signatures,
                                             const TrustStore& trust,
                                             const std::vector<FetchedRevocation>& fetched,
                                             const RevocationData& embedded = {});

/// Hole size for a document timestamp: a token with the TSA's chain.
[[nodiscard]] std::size_t estimate_timestamp_size();

struct TimestampResult {
    std::size_t der_size = 0;
    std::size_t hole_size = 0;
    std::int64_t time = 0;  ///< the TSA's
};

/// Fills a prepared document timestamp's hole (ops::prepare_document_timestamp)
/// with a token from `options.tsa_url` over the SHA-256 of everything but the
/// hole. The same check of the hole against the file's bytes as sign_prepared.
TimestampResult timestamp_prepared(int fd, const ops::ByteRange& range,
                                   const SignOptions& options);

}  // namespace leht::crypto
