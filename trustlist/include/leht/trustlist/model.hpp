// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// leht::trustlist -- what the EU trusted lists say, once verified.
//
// This half has no XML in it. It is the model the verifier reads: qualified
// certificate authorities and timestamp authorities, each with its status
// history and what it was qualified for, plus a compact binary form of all of
// it (the "blob") that is cached on disk and handed to the sandboxed worker.
// Reading and verifying the lists' XML is xml.hpp, a separate library, so
// nothing that only verifies documents links libxml2.

#include <cstdint>
#include <string>
#include <vector>

namespace leht::trustlist {

using Bytes = std::vector<std::uint8_t>;

/// ETSI TS 119 612 criteria for which certificates a qualification applies to:
/// a key-usage pattern, sets of certificate policies, and nested lists.
/// Anything else a list may use is recorded as `unknown`, and a criteria list
/// with an unknown part never matches -- a qualifier Leht cannot check is not
/// applied.
struct Criteria {
    enum class Assert : std::uint8_t { All, AtLeastOne, None };
    Assert assert = Assert::All;
    /// KeyUsageBit name ("digitalSignature", "nonRepudiation", ...) and the
    /// value it must have. One entry per bit; each KeyUsage element is one
    /// criterion holding all of its bits.
    std::vector<std::vector<std::pair<std::string, bool>>> key_usage;
    /// Each PolicySet is one criterion: the certificate must carry every OID.
    std::vector<std::vector<std::string>> policy_sets;
    std::vector<Criteria> nested;
    bool unknown = false;
};

/// Qualifier URIs as flags (ETSI TS 119 612 5.5.9.2.3).
enum Qualifier : std::uint32_t {
    QcStatement = 1U << 0,       ///< QCStatement: the certificate is qualified
    NotQualified = 1U << 1,      ///< NotQualified
    QcWithQscd = 1U << 2,        ///< QCWithQSCD (or the older QCWithSSCD)
    QcNoQscd = 1U << 3,          ///< QCNoQSCD (or QCNoSSCD)
    QcQscdStatusAsInCert = 1U << 4,
    QcQscdManagedOnBehalf = 1U << 5,
    QcForLegalPerson = 1U << 6,
    QcForEsig = 1U << 7,
    QcForEseal = 1U << 8,
    QcForWsa = 1U << 9,
};

struct Qualification {
    std::uint32_t qualifiers = 0;
    Criteria criteria;
};

/// One period of a service's life: from `since` until the next newer one.
struct Phase {
    std::int64_t since = 0;  ///< Unix seconds (StatusStartingTime)
    bool granted = false;    ///< "granted" (or the pre-eIDAS "accredited" etc.)
    /// AdditionalServiceInformation: what the CA issues qualified certificates
    /// for. All false when the list says nothing (then the certificate decides).
    bool for_esig = false;
    bool for_eseal = false;
    bool for_web = false;
    std::vector<Qualification> qualifications;
};

struct Service {
    enum class Type : std::uint8_t { CaQc, TsaQtst };
    Type type = Type::CaQc;
    std::string territory;  ///< "EE"
    std::string provider;   ///< the trust service provider's name
    std::string name;       ///< the service's name
    std::vector<Bytes> certs;  ///< its digital identities (DER)
    /// Newest first: phases[0] is in force now, each older one until the
    /// `since` of the one before it.
    std::vector<Phase> phases;

    /// The phase in force at `when`, or null before the first one.
    [[nodiscard]] const Phase* at(std::int64_t when) const;
};

/// How one list fared, for `leht trusted-list status`.
struct ListStatus {
    std::string territory;
    std::string url;
    bool verified = false;
    std::string problem;       ///< why not, when not
    std::int64_t issued = 0;
    std::int64_t next_update = 0;
    std::uint32_t sequence = 0;
    std::uint32_t services = 0;  ///< qualified services kept from it
};

struct TrustedList {
    std::int64_t built = 0;  ///< when the lists were verified (Unix seconds)
    ListStatus lotl;
    std::vector<ListStatus> lists;
    std::vector<Service> services;

    [[nodiscard]] bool empty() const { return services.empty(); }
};

/// The compact form: versioned, and read back with every length checked, since
/// it travels to the worker and sits in a cache a user can edit.
Bytes encode(const TrustedList& list);
/// Throws std::runtime_error on anything malformed or of another version.
TrustedList decode(const Bytes& blob);

/// Where the LOTL is and who may sign it: the Official Journal publication and
/// the SHA-256 digests of the certificates it names (anchor.cpp). Here rather
/// than in xml.hpp so that the viewer, which never reads the XML, knows where
/// to fetch it.
struct Anchor {
    std::string lotl_url;
    std::string oj_url;
    std::vector<std::string> sha256;  ///< lowercase hex
};

/// The anchor Leht ships: OJ C/2026/1944.
const Anchor& eu_anchor();

/// Where the verified lists are cached, in the compact form: under
/// $XDG_CACHE_HOME (or ~/.cache), leht/trusted-list/trusted-list.bin. The CLI
/// and the viewer share it.
std::string default_cache_path();

/// The Unix time of an xsd:dateTime ("2026-09-24T12:04:06Z"); 0 when it does
/// not parse.
std::int64_t parse_time(const std::string& iso);

}  // namespace leht::trustlist
