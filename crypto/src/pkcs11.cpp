// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Keys on PKCS#11 tokens: an ID card through OpenSC, or any smartcard or HSM.
// The private key never leaves the token; the token is handed a hash and
// returns a signature value. Modules come from p11-kit, which knows the ones
// the system has registered (OpenSC's opensc.module), so no path is needed.
#include "ossl.hpp"

#include "leht/error.hpp"

#include <p11-kit/p11-kit.h>
#include <p11-kit/pkcs11.h>
#include <p11-kit/uri.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>

namespace leht::crypto {

namespace detail {

namespace {

std::string rv_text(CK_RV rv) {
    const char* s = p11_kit_strerror(rv);
    return s != nullptr ? s : "PKCS#11 error " + std::to_string(rv);
}

std::string last_message() {
    const char* s = p11_kit_message();
    return s != nullptr ? s : "unknown reason";
}

/// The PKCS#11 modules in use: every module p11-kit has registered, or the
/// one at an explicit path. Finalized when the last token session is gone.
class Modules {
public:
    explicit Modules(const std::string& path) {
        if (path.empty()) {
            list_ = p11_kit_modules_load_and_initialize(0);
            if (list_ == nullptr) {
                throw Error(0, "cannot load the system's PKCS#11 modules: " + last_message());
            }
            for (CK_FUNCTION_LIST** m = list_; *m != nullptr; ++m) {
                char* name = p11_kit_module_get_name(*m);
                // The trust module holds CA certificates, never keys.
                const bool trust = name != nullptr && std::string(name) == "p11-kit-trust";
                std::free(name);
                if (!trust) {
                    mods_.push_back(*m);
                }
            }
            return;
        }
        CK_FUNCTION_LIST* m = p11_kit_module_load(path.c_str(), 0);
        if (m == nullptr) {
            throw Error(0, "cannot load the PKCS#11 module " + path + ": " + last_message());
        }
        const CK_RV rv = p11_kit_module_initialize(m);
        if (rv != CKR_OK) {
            p11_kit_module_release(m);
            throw Error(0, "cannot initialise the PKCS#11 module " + path + ": " + rv_text(rv));
        }
        mods_.push_back(m);
    }
    Modules(const Modules&) = delete;
    Modules& operator=(const Modules&) = delete;
    ~Modules() {
        if (list_ != nullptr) {
            p11_kit_modules_finalize_and_release(list_);
            return;
        }
        for (CK_FUNCTION_LIST* m : mods_) {
            p11_kit_module_finalize(m);
            p11_kit_module_release(m);
        }
    }

    [[nodiscard]] const std::vector<CK_FUNCTION_LIST*>& all() const { return mods_; }

private:
    CK_FUNCTION_LIST** list_ = nullptr;
    std::vector<CK_FUNCTION_LIST*> mods_;
};

/// PKCS#11's fixed-width, space-padded strings.
std::string padded(const CK_UTF8CHAR* p, std::size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

std::vector<CK_SLOT_ID> slots_with_token(CK_FUNCTION_LIST* m) {
    CK_ULONG n = 0;
    if (m->C_GetSlotList(CK_TRUE, nullptr, &n) != CKR_OK || n == 0) {
        return {};
    }
    std::vector<CK_SLOT_ID> slots(n);
    if (m->C_GetSlotList(CK_TRUE, slots.data(), &n) != CKR_OK) {
        return {};
    }
    slots.resize(n);
    return slots;
}

class Session {
public:
    Session(CK_FUNCTION_LIST* m, CK_SLOT_ID slot) : m_(m) {
        const CK_RV rv = m->C_OpenSession(slot, CKF_SERIAL_SESSION, nullptr, nullptr, &h_);
        if (rv != CKR_OK) {
            h_ = CK_INVALID_HANDLE;
            throw Error(0, "cannot open a session with the token: " + rv_text(rv));
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() {
        if (h_ != CK_INVALID_HANDLE) {
            m_->C_CloseSession(h_);
        }
    }
    [[nodiscard]] CK_SESSION_HANDLE get() const { return h_; }
    /// Hands the session over to a Token, which closes it.
    CK_SESSION_HANDLE release() { return std::exchange(h_, CK_INVALID_HANDLE); }

private:
    CK_FUNCTION_LIST* m_;
    CK_SESSION_HANDLE h_ = CK_INVALID_HANDLE;
};

std::vector<CK_OBJECT_HANDLE> find(CK_FUNCTION_LIST* m, CK_SESSION_HANDLE s,
                                   std::vector<CK_ATTRIBUTE> tmpl) {
    std::vector<CK_OBJECT_HANDLE> out;
    if (m->C_FindObjectsInit(s, tmpl.data(), static_cast<CK_ULONG>(tmpl.size())) != CKR_OK) {
        return out;
    }
    CK_OBJECT_HANDLE batch[16];
    CK_ULONG got = 0;
    while (m->C_FindObjects(s, batch, 16, &got) == CKR_OK && got > 0) {
        out.insert(out.end(), batch, batch + got);
    }
    m->C_FindObjectsFinal(s);
    return out;
}

/// One attribute's bytes; empty when the token does not have or show it.
Bytes attribute(CK_FUNCTION_LIST* m, CK_SESSION_HANDLE s, CK_OBJECT_HANDLE o,
                CK_ATTRIBUTE_TYPE type) {
    CK_ATTRIBUTE a{type, nullptr, 0};
    if (m->C_GetAttributeValue(s, o, &a, 1) != CKR_OK ||
        a.ulValueLen == CK_UNAVAILABLE_INFORMATION || a.ulValueLen == 0) {
        return {};
    }
    Bytes v(a.ulValueLen);
    a.pValue = v.data();
    if (m->C_GetAttributeValue(s, o, &a, 1) != CKR_OK) {
        return {};
    }
    v.resize(a.ulValueLen);
    return v;
}

template <typename T>
T scalar(CK_FUNCTION_LIST* m, CK_SESSION_HANDLE s, CK_OBJECT_HANDLE o, CK_ATTRIBUTE_TYPE type,
         T fallback) {
    T v = fallback;
    CK_ATTRIBUTE a{type, &v, sizeof v};
    if (m->C_GetAttributeValue(s, o, &a, 1) != CKR_OK || a.ulValueLen != sizeof v) {
        return fallback;
    }
    return v;
}

/// X.509 certificates on the token; only those with `id` when it is not empty.
std::vector<CK_OBJECT_HANDLE> certificates_with_id(CK_FUNCTION_LIST* m, CK_SESSION_HANDLE s,
                                                   Bytes id) {
    CK_OBJECT_CLASS cls = CKO_CERTIFICATE;
    CK_CERTIFICATE_TYPE type = CKC_X_509;
    std::vector<CK_ATTRIBUTE> t{{CKA_CLASS, &cls, sizeof cls},
                                {CKA_CERTIFICATE_TYPE, &type, sizeof type}};
    if (!id.empty()) {
        t.push_back({CKA_ID, id.data(), id.size()});
    }
    return find(m, s, t);
}

using UriPtr = Ptr<P11KitUri, p11_kit_uri_free>;

std::string make_uri(const CK_TOKEN_INFO& token, Bytes id) {
    const UriPtr uri{p11_kit_uri_new()};
    *p11_kit_uri_get_token_info(uri.get()) = token;
    CK_OBJECT_CLASS cls = CKO_PRIVATE_KEY;
    CK_ATTRIBUTE a{CKA_CLASS, &cls, sizeof cls};
    CK_ATTRIBUTE b{CKA_ID, id.data(), id.size()};
    p11_kit_uri_set_attribute(uri.get(), &a);
    p11_kit_uri_set_attribute(uri.get(), &b);
    char* text = nullptr;
    if (p11_kit_uri_format(uri.get(), P11_KIT_URI_FOR_OBJECT_ON_TOKEN, &text) != P11_KIT_URI_OK) {
        throw Error(0, "cannot write a PKCS#11 URI");
    }
    std::string out(text);
    std::free(text);
    return out;
}

void set_pin_state(TokenKey& k, CK_FLAGS flags) {
    k.pinpad = (flags & CKF_PROTECTED_AUTHENTICATION_PATH) != 0;
    k.pin_count_low = (flags & CKF_USER_PIN_COUNT_LOW) != 0;
    k.pin_final_try = (flags & CKF_USER_PIN_FINAL_TRY) != 0;
    k.pin_locked = (flags & CKF_USER_PIN_LOCKED) != 0;
}

/// What a failed login or signature means for the person at the reader.
[[noreturn]] void token_fail(CK_RV rv, CK_FUNCTION_LIST* m, CK_SLOT_ID slot) {
    CK_TOKEN_INFO info{};
    const bool have = m->C_GetTokenInfo(slot, &info) == CKR_OK;
    switch (rv) {
        case CKR_PIN_INCORRECT: {
            std::string msg = "The PIN is wrong.";
            if (have && (info.flags & CKF_USER_PIN_LOCKED) != 0) {
                msg += " It is now blocked; unblock it with the PUK code (for an Estonian ID "
                       "card, in DigiDoc4).";
            } else if (have && (info.flags & CKF_USER_PIN_FINAL_TRY) != 0) {
                msg += " One more wrong PIN will block it.";
            } else if (have && (info.flags & CKF_USER_PIN_COUNT_LOW) != 0) {
                msg += " A few more wrong tries will block it.";
            }
            throw Error(0, msg);
        }
        case CKR_PIN_LOCKED:
            throw Error(0, "The PIN is blocked after too many wrong tries. Unblock it with the "
                           "PUK code (for an Estonian ID card, in DigiDoc4).");
        case CKR_PIN_LEN_RANGE:
            throw Error(0, "The PIN has the wrong number of digits.");
        case CKR_FUNCTION_CANCELED:
            throw Error(0, "PIN entry was cancelled on the reader.");
        case CKR_TOKEN_NOT_PRESENT:
        case CKR_DEVICE_REMOVED:
        case CKR_SESSION_CLOSED:
        case CKR_SESSION_HANDLE_INVALID:
            throw Error(0, "The card was removed from the reader.");
        default:
            throw Error(0, "the token refused: " + rv_text(rv));
    }
}

}  // namespace

struct Token {
    std::shared_ptr<Modules> modules;
    CK_FUNCTION_LIST* m = nullptr;
    CK_SLOT_ID slot = 0;
    CK_SESSION_HANDLE session = CK_INVALID_HANDLE;
    CK_OBJECT_HANDLE key = CK_INVALID_HANDLE;
    CK_KEY_TYPE type = CKK_RSA;
    bool always_authenticate = false;
    bool pinpad = false;
    Secret pin;

    Token() = default;
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    ~Token() {
        if (session != CK_INVALID_HANDLE) {
            m->C_Logout(session);
            m->C_CloseSession(session);
        }
    }

    void login(CK_USER_TYPE who) {
        const auto* p = reinterpret_cast<const CK_UTF8CHAR*>(pin.str().data());
        const CK_RV rv =
            pinpad ? m->C_Login(session, who, nullptr, 0)
                   : m->C_Login(session, who, const_cast<CK_UTF8CHAR*>(p),
                                static_cast<CK_ULONG>(pin.str().size()));
        if (rv != CKR_OK && !(who == CKU_USER && rv == CKR_USER_ALREADY_LOGGED_IN)) {
            token_fail(rv, m, slot);
        }
    }
};

Bytes token_sign(Token& t, const Bytes& input) {
    CK_MECHANISM mech{t.type == CKK_EC ? CKM_ECDSA : CKM_RSA_PKCS, nullptr, 0};
    CK_RV rv = t.m->C_SignInit(t.session, &mech, t.key);
    if (rv != CKR_OK) {
        token_fail(rv, t.m, t.slot);
    }
    if (t.always_authenticate) {
        t.login(CKU_CONTEXT_SPECIFIC);
    }
    auto* in = const_cast<CK_BYTE*>(input.data());
    CK_ULONG n = 0;
    rv = t.m->C_Sign(t.session, in, static_cast<CK_ULONG>(input.size()), nullptr, &n);
    if (rv != CKR_OK) {
        token_fail(rv, t.m, t.slot);
    }
    Bytes out(n);
    rv = t.m->C_Sign(t.session, in, static_cast<CK_ULONG>(input.size()), out.data(), &n);
    if (rv != CKR_OK) {
        token_fail(rv, t.m, t.slot);
    }
    out.resize(n);
    return out;
}

}  // namespace detail

std::vector<TokenKey> list_token_keys(const std::string& module_path) {
    init();
    const detail::Modules modules(module_path);
    std::vector<TokenKey> out;
    for (CK_FUNCTION_LIST* m : modules.all()) {
        for (const CK_SLOT_ID slot : detail::slots_with_token(m)) {
            // One unreadable token must not hide the others.
            try {
                CK_TOKEN_INFO info{};
                if (m->C_GetTokenInfo(slot, &info) != CKR_OK) {
                    continue;
                }
                const detail::Session s(m, slot);
                for (const CK_OBJECT_HANDLE o : detail::certificates_with_id(m, s.get(), {})) {
                    const Bytes id = detail::attribute(m, s.get(), o, CKA_ID);
                    const Bytes der = detail::attribute(m, s.get(), o, CKA_VALUE);
                    if (id.empty() || der.empty()) {
                        continue;
                    }
                    const unsigned char* p = der.data();
                    const detail::X509Ptr cert{
                        d2i_X509(nullptr, &p, static_cast<long>(der.size()))};
                    if (!cert) {
                        ERR_clear_error();
                        continue;
                    }
                    TokenKey k;
                    k.cert = detail::info(cert.get());
                    if (k.cert.is_ca || !k.cert.can_sign) {
                        continue;
                    }
                    const std::uint32_t usage = X509_get_key_usage(cert.get());
                    k.non_repudiation = usage != UINT32_MAX && (usage & KU_NON_REPUDIATION) != 0;
                    k.uri = detail::make_uri(info, id);
                    k.token_label = detail::padded(info.label, sizeof info.label);
                    const Bytes label = detail::attribute(m, s.get(), o, CKA_LABEL);
                    k.key_label.assign(label.begin(), label.end());
                    detail::set_pin_state(k, info.flags);
                    out.push_back(std::move(k));
                }
            } catch (const Error&) {
                continue;
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const TokenKey& a, const TokenKey& b) {
        return a.non_repudiation && !b.non_repudiation;
    });
    return out;
}

Identity Identity::from_pkcs11(const std::string& uri_text, const PinSource& pin,
                               const std::string& module_path) {
    init();
    const detail::UriPtr uri{p11_kit_uri_new()};
    const int parsed = p11_kit_uri_parse(uri_text.c_str(), P11_KIT_URI_FOR_ANY, uri.get());
    if (parsed != P11_KIT_URI_OK) {
        throw Error(0, "not a PKCS#11 URI: " + uri_text + " (" + p11_kit_uri_message(parsed) + ")");
    }
    if (p11_kit_uri_get_pin_value(uri.get()) != nullptr ||
        p11_kit_uri_get_pin_source(uri.get()) != nullptr) {
        throw Error(0, "a PKCS#11 URI must not carry the PIN; leht asks for it");
    }

    auto modules = std::make_shared<detail::Modules>(module_path);
    CK_FUNCTION_LIST* m = nullptr;
    CK_SLOT_ID slot = 0;
    CK_TOKEN_INFO info{};
    for (CK_FUNCTION_LIST* candidate : modules->all()) {
        CK_INFO module_info{};
        if (candidate->C_GetInfo(&module_info) != CKR_OK ||
            p11_kit_uri_match_module_info(uri.get(), &module_info) != 1) {
            continue;
        }
        for (const CK_SLOT_ID s : detail::slots_with_token(candidate)) {
            if (candidate->C_GetTokenInfo(s, &info) == CKR_OK &&
                p11_kit_uri_match_token_info(uri.get(), &info) == 1) {
                m = candidate;
                slot = s;
                break;
            }
        }
        if (m != nullptr) {
            break;
        }
    }
    if (m == nullptr) {
        throw Error(0, "No card or token matches " + uri_text + ". Is the card in the reader?");
    }

    // The URI's object attributes, always narrowed to a private key.
    CK_ULONG n_attrs = 0;
    CK_ATTRIBUTE* attrs = p11_kit_uri_get_attributes(uri.get(), &n_attrs);
    std::vector<CK_ATTRIBUTE> key_tmpl;
    Bytes wanted_id;
    for (CK_ULONG i = 0; i < n_attrs; ++i) {
        if (attrs[i].type == CKA_CLASS) {
            continue;
        }
        if (attrs[i].type == CKA_ID) {
            const auto* p = static_cast<const std::uint8_t*>(attrs[i].pValue);
            wanted_id.assign(p, p + attrs[i].ulValueLen);
        }
        key_tmpl.push_back(attrs[i]);
    }
    CK_OBJECT_CLASS key_class = CKO_PRIVATE_KEY;
    key_tmpl.push_back({CKA_CLASS, &key_class, sizeof key_class});

    detail::Session session(m, slot);
    auto token = std::make_shared<detail::Token>();
    token->modules = modules;
    token->m = m;
    token->slot = slot;

    // What the PIN prompt can say: the token, and the certificate when it is
    // readable before logging in (it is public on ID cards).
    TokenKey about;
    about.uri = uri_text;
    about.token_label = detail::padded(info.label, sizeof info.label);
    detail::set_pin_state(about, info.flags);
    if (!wanted_id.empty()) {
        const auto certs = detail::certificates_with_id(m, session.get(), wanted_id);
        if (!certs.empty()) {
            const Bytes der = detail::attribute(m, session.get(), certs.front(), CKA_VALUE);
            const unsigned char* p = der.data();
            const detail::X509Ptr c{d2i_X509(nullptr, &p, static_cast<long>(der.size()))};
            if (c) {
                about.cert = detail::info(c.get());
            }
            ERR_clear_error();
        }
    }
    if (about.pin_locked) {
        throw Error(0, "The PIN is blocked after too many wrong tries. Unblock it with the PUK "
                       "code (for an Estonian ID card, in DigiDoc4).");
    }
    token->pinpad = about.pinpad;
    token->pin = pin(about);
    token->session = session.release();
    token->login(CKU_USER);

    const auto keys = detail::find(m, token->session, key_tmpl);
    if (keys.empty()) {
        throw Error(0, "The token " + about.token_label + " has no private key matching " +
                           uri_text);
    }
    token->key = keys.front();
    token->type = detail::scalar<CK_KEY_TYPE>(m, token->session, token->key, CKA_KEY_TYPE,
                                              CKK_VENDOR_DEFINED);
    if (token->type != CKK_RSA && token->type != CKK_EC) {
        throw Error(0, "the token's key is neither RSA nor EC, which leht cannot sign with");
    }
    token->always_authenticate = detail::scalar<CK_BBOOL>(
                                     m, token->session, token->key, CKA_ALWAYS_AUTHENTICATE,
                                     CK_FALSE) == CK_TRUE;

    const Bytes key_id = detail::attribute(m, token->session, token->key, CKA_ID);
    const auto certs = detail::certificates_with_id(m, token->session, key_id);
    if (key_id.empty() || certs.empty()) {
        throw Error(0, "The token " + about.token_label +
                           " has no certificate for this key, so there is nothing to sign as");
    }
    const Bytes der = detail::attribute(m, token->session, certs.front(), CKA_VALUE);
    const unsigned char* p = der.data();
    auto impl = std::make_unique<Impl>();
    impl->cert.reset(d2i_X509(nullptr, &p, static_cast<long>(der.size())));
    if (!impl->cert) {
        detail::fail("the token's certificate cannot be read");
    }
    impl->key.reset(X509_get_pubkey(impl->cert.get()));
    if (!impl->key) {
        detail::fail("the token's certificate holds no usable public key");
    }
    impl->token = std::move(token);
    return Identity{std::move(impl)};
}

bool Identity::on_token() const { return impl_->token != nullptr; }

}  // namespace leht::crypto
