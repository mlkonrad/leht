// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fuzz target for the EU trusted lists (queue M5): XML from ~30 servers on the
// network, parsed by libxml2 and xmlsec1 inside the sandboxed worker, and the
// compact form Leht caches and hands to the document worker.
//
// The invariant: whatever the bytes, each call returns or throws leht::Error
// (or std::runtime_error from the compact form) -- nothing else, and no
// sanitizer report. Seeded with the real lists (trustlist/tests/fixtures.tar.xz).

#include "leht/error.hpp"
#include "leht/trustlist/model.hpp"
#include "leht/trustlist/xml.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const bool ready = [] {
        leht::trustlist::init();
        return true;
    }();
    (void)ready;
    const leht::trustlist::Bytes bytes(data, data + size);

    // 1. As a signed list: parsing, the anti-wrapping checks, xmlsec1.
    try {
        (void)leht::trustlist::verify_signature(bytes, [](const auto&) { return true; });
    } catch (const leht::Error&) {
    }
    // 2. As a LOTL, all the way through advance(): the scheme, the pivots and
    //    the pointers are read before any signature decides anything.
    try {
        leht::trustlist::Anchor anchor{"https://lotl.test/l.xml", "https://oj.test/1", {"00"}};
        const auto step = leht::trustlist::advance(anchor, bytes, {}, 0);
        std::vector<leht::trustlist::Fetched> fetched;
        for (const auto& url : step.need) {
            fetched.push_back({url, bytes, ""});
        }
        (void)leht::trustlist::advance(anchor, bytes, fetched, 0);
    } catch (const leht::Error&) {
    }
    // 3. The service extraction, which a real update reaches only after a
    //    signature verifies: fuzzed directly.
    try {
        (void)leht::trustlist::detail::parse_services_unverified(bytes);
    } catch (const leht::Error&) {
    }
    // 4. The compact form, as read back from the cache or by the worker.
    try {
        (void)leht::trustlist::decode(bytes);
    } catch (const std::runtime_error&) {
    }
    return 0;
}
