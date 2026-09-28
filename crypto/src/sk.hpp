// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal header: the parts of the Smart-ID and Mobile-ID clients (sk.cpp)
// that are pure functions, so the tests can hold them against SK's published
// examples and the fuzzer can feed them hostile replies.
#pragma once

#include "leht/crypto/crypto.hpp"

#include <string>

namespace leht::crypto::detail {
/// Base64 with padding, and back (throws leht::Error on anything else).
/// Also declared in ossl.hpp, which names OpenSSL types; this one does not.
std::string base64(const Bytes& data);
Bytes unbase64(const std::string& text);
}  // namespace leht::crypto::detail

namespace leht::crypto::sk {

/// Smart-ID's authCode: HMAC-SHA256 of `payload` keyed with the session
/// secret, Base64URL without padding.
std::string auth_code(const Bytes& session_secret, const std::string& payload);

/// What a Smart-ID QR link is made of. `protocol`, `digest` and
/// `interactions` stay empty for a certificate-choice session.
struct LinkParts {
    std::string device_link_base;  ///< from SK's reply
    std::string session_token;
    std::string session_type;  ///< "cert", "sign" or "auth"
    std::string lang = "eng";
    long elapsed_seconds = 0;
    std::string scheme_name;
    std::string relying_party_name;
    std::string brokered_rp_name;  ///< empty for Leht
    std::string protocol;
    std::string digest;        ///< Base64, as sent
    std::string interactions;  ///< Base64, as sent
};

/// The dynamic QR code's link: the device link with elapsedSeconds, and its
/// authCode over the fields in SK's order (none of them URL-encoded).
std::string qr_link(const LinkParts& parts, const Bytes& session_secret);

/// Mobile-ID's verification code: the first 6 bits of the hash and its last
/// 7, as a number of four digits.
std::string mobile_id_code(const Bytes& hash);

/// What a finished (or running) session says, parsed from SK's JSON. Throws
/// leht::Error on JSON that is not what SK sends. Used for every reply, so
/// that one place parses hostile text.
struct SmartIdSession {
    bool complete = false;
    std::string end_result;       ///< "OK", "USER_REFUSED", ...
    std::string document_number;
    Bytes cert;                   ///< DER, when the session returned one
    std::string cert_level;
    Bytes signature;
    std::string signature_algorithm;  ///< "rsassa-pss", ...
    std::string hash_algorithm;       ///< signatureAlgorithmParameters.hashAlgorithm
    std::string mgf_hash_algorithm;
    int salt_length = -1;
    std::string flow_type;            ///< "QR", "Notification", ...
    std::string interaction_used;
};
SmartIdSession parse_smart_id_session(const std::string& json);

struct MobileIdSession {
    bool complete = false;
    std::string result;  ///< "OK", "USER_CANCELLED", ...
    Bytes signature;
    std::string algorithm;  ///< "SHA256WithECEncryption", ...
    Bytes cert;             ///< DER, when present
};
MobileIdSession parse_mobile_id_session(const std::string& json);

}  // namespace leht::crypto::sk
