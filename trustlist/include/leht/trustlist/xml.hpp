// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// leht::trustlist_xml -- reading and verifying the EU trusted lists
// (ETSI TS 119 612 XML, signed with XMLDSig/XAdES), with libxml2 and xmlsec1.
//
// Everything here parses bytes from the network, so the viewer runs it in the
// sandboxed worker (leht-worker --trusted-list). The CLI runs it in process,
// as it parses documents.
//
// An update is a loop: fetch the LOTL, call advance(), fetch what it asks for,
// call it again with those, until it is done. The fetching is the caller's --
// the only step with network -- and the logic is all here, in one place.

#include "leht/trustlist/model.hpp"

#include <functional>
#include <string>
#include <vector>

namespace leht::trustlist {

/// Something fetched for advance(): its body, or why there is none.
struct Fetched {
    std::string url;
    Bytes body;
    std::string error;
};

struct Step {
    /// Fetch these next (http:// or https://) and call advance() again with
    /// them added. Empty when done.
    std::vector<std::string> need;
    bool done = false;
    /// When done: the verified services, and each list's status. A LOTL that
    /// could not be trusted leaves `result.services` empty and says why in
    /// `result.lotl.problem`.
    TrustedList result;
};

/// One step of an update. `lotl` is what was fetched from anchor.lotl_url;
/// `fetched` holds everything fetched since, keyed by URL. `now` stamps the
/// result. Never throws on the lists' content: a list that fails is recorded
/// in the result, and the others still count.
Step advance(const Anchor& anchor, const Bytes& lotl, const std::vector<Fetched>& fetched,
             std::int64_t now);

/// Verifies `xml`'s enveloped signature and returns the signer's certificate
/// (DER). The signature must be the root's only ds:Signature, cover the whole
/// document (a Reference with URI="" and the enveloped transform), reference
/// nothing else but its own XAdES SignedProperties, and be made by a
/// certificate `allowed` accepts. Throws leht::Error saying why not.
Bytes verify_signature(const Bytes& xml, const std::function<bool(const Bytes& der)>& allowed);

/// Loads what verification can need before a sandbox: libxml2's and xmlsec1's
/// initialisation, and OpenSSL's algorithms.
void init();

namespace detail {
/// The services a list describes, WITHOUT verifying its signature: for the
/// fuzzer and tests only. Nothing that decides trust may call it.
std::vector<Service> parse_services_unverified(const Bytes& xml);
}  // namespace detail

}  // namespace leht::trustlist
