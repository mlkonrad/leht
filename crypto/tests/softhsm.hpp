// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A SoftHSM2 token for tests: a smartcard with no card. Created fresh in a
// temporary directory on every run, holding keys and certificates from the
// test PKI, so no key is committed and a real card is only needed for the
// last, manual check.
#pragma once

#include "test_pki.hpp"

#include <p11-kit/p11-kit.h>
#include <p11-kit/pkcs11.h>

#include <openssl/core_names.h>
#include <openssl/objects.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace leht::test {

class SoftHsm {
public:
    static constexpr const char* kPin = "1234";
    static constexpr const char* kSoPin = "87654321";

    /// The SoftHSM2 PKCS#11 module, or empty when SoftHSM is not installed.
    static std::string module() {
        for (const char* p : {"/usr/lib64/pkcs11/libsofthsm2.so",
                              "/usr/lib/softhsm/libsofthsm2.so",
                              "/usr/lib/x86_64-linux-gnu/softhsm/libsofthsm2.so",
                              "/usr/lib/aarch64-linux-gnu/softhsm/libsofthsm2.so"}) {
            if (std::filesystem::exists(p)) {
                return p;
            }
        }
        return {};
    }

    /// Whether SoftHSM2 is here to stand in for a card.
    static bool available() {
        return !module().empty() &&
               std::system("command -v softhsm2-util >/dev/null 2>&1") == 0;
    }

    /// An empty token labelled `label`, with user PIN kPin. Points
    /// SOFTHSM2_CONF at it, so only one SoftHsm may live at a time.
    explicit SoftHsm(const std::string& label = "Leht Test Card")
        : dir_(std::filesystem::temp_directory_path() /
               ("leht_test_softhsm_" + std::to_string(::getpid()))),
          label_(label) {
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "tokens");
        conf_ = (dir_ / "softhsm2.conf").string();
        std::ofstream(conf_) << "directories.tokendir = " << (dir_ / "tokens").string()
                             << "\nobjectstore.backend = file\nlog.level = ERROR\n";
        ::setenv("SOFTHSM2_CONF", conf_.c_str(), 1);
        const std::string cmd = "softhsm2-util --init-token --free --label '" + label +
                                "' --so-pin " + kSoPin + " --pin " + kPin + " >/dev/null";
        if (std::system(cmd.c_str()) != 0) {
            die("softhsm2-util --init-token");
        }
    }
    ~SoftHsm() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    SoftHsm(const SoftHsm&) = delete;
    SoftHsm& operator=(const SoftHsm&) = delete;

    [[nodiscard]] const std::string& label() const { return label_; }
    [[nodiscard]] const std::string& conf() const { return conf_; }

    /// Stores `key` (RSA or EC) and `cert` on the token under CKA_ID `id`. The
    /// certificate may belong to a different key: that is how a mismatch is
    /// tested. `always_authenticate` makes the key demand the PIN again for
    /// every signature, as an ID card's signing key does.
    void add(const Key& key, const Cert& cert, const std::string& id, const std::string& name,
             bool always_authenticate = false) {
        CK_FUNCTION_LIST* m = p11_kit_module_load(module().c_str(), 0);
        if (m == nullptr || p11_kit_module_initialize(m) != CKR_OK) {
            die("load SoftHSM");
        }
        CK_SESSION_HANDLE s = open(m);

        CK_BBOOL yes = CK_TRUE;
        CK_BBOOL no = CK_FALSE;
        CK_BBOOL always = always_authenticate ? CK_TRUE : CK_FALSE;
        std::vector<unsigned char> idb(id.begin(), id.end());
        std::vector<unsigned char> der = crypto_der(cert);
        std::vector<unsigned char> subject = name_der(X509_get_subject_name(cert.p));
        CK_OBJECT_CLASS cert_class = CKO_CERTIFICATE;
        CK_CERTIFICATE_TYPE x509 = CKC_X_509;
        std::vector<CK_ATTRIBUTE> ct{
            {CKA_CLASS, &cert_class, sizeof cert_class},
            {CKA_CERTIFICATE_TYPE, &x509, sizeof x509},
            {CKA_TOKEN, &yes, sizeof yes},
            {CKA_PRIVATE, &no, sizeof no},
            {CKA_ID, idb.data(), idb.size()},
            {CKA_LABEL, const_cast<char*>(name.data()), name.size()},
            {CKA_SUBJECT, subject.data(), subject.size()},
            {CKA_VALUE, der.data(), der.size()},
        };
        create(m, s, ct);

        CK_OBJECT_CLASS key_class = CKO_PRIVATE_KEY;
        std::vector<CK_ATTRIBUTE> kt{
            {CKA_CLASS, &key_class, sizeof key_class},
            {CKA_TOKEN, &yes, sizeof yes},
            {CKA_PRIVATE, &yes, sizeof yes},
            {CKA_SENSITIVE, &yes, sizeof yes},
            {CKA_SIGN, &yes, sizeof yes},
            {CKA_ALWAYS_AUTHENTICATE, &always, sizeof always},
            {CKA_ID, idb.data(), idb.size()},
            {CKA_LABEL, const_cast<char*>(name.data()), name.size()},
        };
        std::vector<std::vector<unsigned char>> keep;
        const auto bn = [&](const char* param) {
            BIGNUM* b = nullptr;
            if (EVP_PKEY_get_bn_param(key.p, param, &b) != 1) {
                die("key parameter");
            }
            keep.emplace_back(static_cast<std::size_t>(BN_num_bytes(b)));
            BN_bn2bin(b, keep.back().data());
            BN_free(b);
            return &keep.back();
        };
        keep.reserve(16);
        CK_KEY_TYPE type = 0;
        if (EVP_PKEY_get_base_id(key.p) == EVP_PKEY_EC) {
            type = CKK_EC;
            char group[64];
            if (EVP_PKEY_get_utf8_string_param(key.p, OSSL_PKEY_PARAM_GROUP_NAME, group,
                                               sizeof group, nullptr) != 1) {
                die("EC group");
            }
            ASN1_OBJECT* oid = OBJ_nid2obj(EC_curve_nist2nid(group) != NID_undef
                                               ? EC_curve_nist2nid(group)
                                               : OBJ_sn2nid(group));
            unsigned char* p = nullptr;
            const int n = i2d_ASN1_OBJECT(oid, &p);
            keep.emplace_back(p, p + n);
            OPENSSL_free(p);
            const auto& params = keep.back();
            kt.push_back({CKA_EC_PARAMS, const_cast<unsigned char*>(params.data()),
                          params.size()});
            const auto* v = bn(OSSL_PKEY_PARAM_PRIV_KEY);
            kt.push_back({CKA_VALUE, const_cast<unsigned char*>(v->data()), v->size()});
        } else {
            type = CKK_RSA;
            const std::pair<CK_ATTRIBUTE_TYPE, const char*> parts[] = {
                {CKA_MODULUS, OSSL_PKEY_PARAM_RSA_N},
                {CKA_PUBLIC_EXPONENT, OSSL_PKEY_PARAM_RSA_E},
                {CKA_PRIVATE_EXPONENT, OSSL_PKEY_PARAM_RSA_D},
                {CKA_PRIME_1, OSSL_PKEY_PARAM_RSA_FACTOR1},
                {CKA_PRIME_2, OSSL_PKEY_PARAM_RSA_FACTOR2},
                {CKA_EXPONENT_1, OSSL_PKEY_PARAM_RSA_EXPONENT1},
                {CKA_EXPONENT_2, OSSL_PKEY_PARAM_RSA_EXPONENT2},
                {CKA_COEFFICIENT, OSSL_PKEY_PARAM_RSA_COEFFICIENT1},
            };
            for (const auto& [attr, param] : parts) {
                const auto* v = bn(param);
                kt.push_back({attr, const_cast<unsigned char*>(v->data()), v->size()});
            }
        }
        kt.push_back({CKA_KEY_TYPE, &type, sizeof type});
        create(m, s, kt);

        m->C_Logout(s);
        m->C_CloseSession(s);
        p11_kit_module_finalize(m);
        p11_kit_module_release(m);
    }

    /// A read-write session on this token through `m`, logged in as the user.
    CK_SESSION_HANDLE open(CK_FUNCTION_LIST* m) const {
        CK_SLOT_ID found[8];
        CK_ULONG n = 8;
        if (m->C_GetSlotList(CK_TRUE, found, &n) != CKR_OK) {
            die("slot list");
        }
        for (CK_ULONG i = 0; i < n; ++i) {
            CK_TOKEN_INFO info{};
            m->C_GetTokenInfo(found[i], &info);
            std::string l(reinterpret_cast<const char*>(info.label), sizeof info.label);
            l.erase(l.find_last_not_of(' ') + 1);
            if (l != label_) {
                continue;
            }
            CK_SESSION_HANDLE s = 0;
            if (m->C_OpenSession(found[i], CKF_SERIAL_SESSION | CKF_RW_SESSION, nullptr, nullptr,
                                 &s) != CKR_OK) {
                die("open session");
            }
            std::string pin = kPin;
            const CK_RV rv = m->C_Login(s, CKU_USER, reinterpret_cast<CK_UTF8CHAR*>(pin.data()),
                                        pin.size());
            if (rv != CKR_OK && rv != CKR_USER_ALREADY_LOGGED_IN) {
                die("login");
            }
            return s;
        }
        die("no SoftHSM token with that label");
        return 0;
    }

private:
    static std::vector<unsigned char> crypto_der(const Cert& c) {
        unsigned char* p = nullptr;
        const int n = i2d_X509(c.p, &p);
        std::vector<unsigned char> out(p, p + n);
        OPENSSL_free(p);
        return out;
    }
    static std::vector<unsigned char> name_der(X509_NAME* name) {
        unsigned char* p = nullptr;
        const int n = i2d_X509_NAME(name, &p);
        std::vector<unsigned char> out(p, p + n);
        OPENSSL_free(p);
        return out;
    }

    static void create(CK_FUNCTION_LIST* m, CK_SESSION_HANDLE s, std::vector<CK_ATTRIBUTE>& t) {
        CK_OBJECT_HANDLE h = 0;
        const CK_RV rv = m->C_CreateObject(s, t.data(), static_cast<CK_ULONG>(t.size()), &h);
        if (rv != CKR_OK) {
            std::fprintf(stderr, "C_CreateObject: %s\n", p11_kit_strerror(rv));
            die("C_CreateObject");
        }
    }

    std::filesystem::path dir_;
    std::string label_;
    std::string conf_;
};

}  // namespace leht::test
