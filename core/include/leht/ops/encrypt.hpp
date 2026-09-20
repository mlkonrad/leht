// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <string>

namespace leht {
class Context;
}

namespace leht::ops {

enum class Encryption {
    Rc4_128,  ///< Legacy. Only for readers that cannot handle AES.
    Aes128,
    Aes256,   ///< The default, and the only one worth choosing today.
};

/// What a reader may do without the owner password. PDF permissions are
/// advisory -- a reader chooses whether to honour them -- so treat these as a
/// statement of intent, not as enforcement. Real confidentiality comes from
/// the user password, which is what actually encrypts the content.
struct Permissions {
    bool print = true;
    bool modify = false;
    bool copy = false;
    bool annotate = false;
    bool fill_forms = true;
    bool assemble = false;
    bool print_high_quality = true;
};

struct EncryptOptions {
    /// Required to open the document. Empty means anyone can open it, and only
    /// the permission flags apply.
    std::string user_password;
    /// Grants full rights. Empty reuses the user password.
    std::string owner_password;

    Encryption method = Encryption::Aes256;
    Permissions permissions{};
};

/// Writes an encrypted copy of `input` to `output`. Never modifies the input.
void encrypt(const Context& ctx, const std::string& input,
             const std::string& output, const EncryptOptions& options);

/// Writes a decrypted copy. `password` must open the document; it may be
/// either the user or the owner password.
void decrypt(const Context& ctx, const std::string& input,
             const std::string& output, const std::string& password = "");

}  // namespace leht::ops
