// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fuzz target for the replies of SK's Smart-ID and Mobile-ID services.
//
// They are parsed in the trusted process -- the one that signs -- so a reply
// that could crash it matters more than most input. Everything SK sends passes
// through parse_smart_id_session(), parse_mobile_id_session() and unbase64();
// the invariant is that they return or throw leht::Error, nothing else.

#include "leht/error.hpp"
#include "sk.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    try {
        (void)leht::crypto::sk::parse_smart_id_session(text);
    } catch (const leht::Error&) {
    }
    try {
        (void)leht::crypto::sk::parse_mobile_id_session(text);
    } catch (const leht::Error&) {
    }
    try {
        (void)leht::crypto::detail::unbase64(text);
    } catch (const leht::Error&) {
    }
    // The verification code reads the first and last bytes of anything.
    (void)leht::crypto::sk::mobile_id_code(leht::crypto::Bytes(data, data + size));
    return 0;
}
