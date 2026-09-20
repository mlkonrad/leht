// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/encrypt.hpp"

#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <cstring>

namespace leht::ops {

namespace {

using detail::OwnedPdfDoc;

int method_code(Encryption method) {
    switch (method) {
        case Encryption::Rc4_128: return PDF_ENCRYPT_RC4_128;
        case Encryption::Aes128:  return PDF_ENCRYPT_AES_128;
        case Encryption::Aes256:  return PDF_ENCRYPT_AES_256;
    }
    return PDF_ENCRYPT_AES_256;
}

/// PDF permission bits are "1 means allowed", and the reserved bits are
/// required to be 1, so start from all-ones and clear what is disallowed.
int permission_bits(const Permissions& p) {
    int bits = -1;
    if (!p.print)              { bits &= ~PDF_PERM_PRINT; }
    if (!p.modify)             { bits &= ~PDF_PERM_MODIFY; }
    if (!p.copy)               { bits &= ~PDF_PERM_COPY; }
    if (!p.annotate)           { bits &= ~PDF_PERM_ANNOTATE; }
    if (!p.fill_forms)         { bits &= ~PDF_PERM_FORM; }
    if (!p.assemble)           { bits &= ~PDF_PERM_ASSEMBLE; }
    if (!p.print_high_quality) { bits &= ~PDF_PERM_PRINT_HQ; }
    return bits;
}

void copy_password(char (&dest)[128], const std::string& source,
                   const char* which) {
    if (source.size() >= sizeof(dest)) {
        throw Error(0, std::string(which) + " password is too long (max " +
                           std::to_string(sizeof(dest) - 1) + " bytes)");
    }
    std::memset(dest, 0, sizeof(dest));
    std::memcpy(dest, source.data(), source.size());
}

OwnedPdfDoc open_and_unlock(fz_context* ctx, const std::string& path,
                            const std::string& password) {
    OwnedPdfDoc doc{ctx};
    const char* cpath = path.c_str();
    guarded(ctx, [&](fz_context* g) { *doc.slot() = pdf_open_document(g, cpath); });
    if (!doc) {
        throw Error(0, "could not open as PDF: " + path);
    }

    pdf_document* pdf = doc.get();
    int needs = 0;
    guarded(ctx, [&](fz_context* g) { needs = pdf_needs_password(g, pdf); });
    if (needs == 0) {
        return doc;
    }

    const char* pw = password.c_str();
    int ok = 0;
    guarded(ctx, [&](fz_context* g) {
        ok = pdf_authenticate_password(g, pdf, pw);
    });
    if (ok == 0) {
        throw Error(0, password.empty()
                           ? "document is encrypted and needs a password: " + path
                           : "wrong password for: " + path);
    }
    return doc;
}

pdf_write_options base_options() {
    pdf_write_options opts = pdf_default_write_options;
    opts.do_garbage = 3;
    opts.do_compress = 1;
    opts.do_compress_images = 1;
    opts.do_compress_fonts = 1;
    return opts;
}

}  // namespace

void encrypt(const Context& ctx, const std::string& input,
             const std::string& output, const EncryptOptions& options) {
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot encrypt with a moved-from Context");
    }
    if (input == output) {
        throw Error(0, "encrypt will not write over its input: " + input);
    }

    OwnedPdfDoc doc = open_and_unlock(c, input, options.user_password);

    pdf_write_options opts = base_options();
    opts.do_encrypt = method_code(options.method);
    opts.permissions = permission_bits(options.permissions);
    copy_password(opts.upwd_utf8, options.user_password, "user");
    // An empty owner password would leave the document trivially unlockable,
    // so fall back to the user password rather than to nothing.
    copy_password(opts.opwd_utf8,
                  options.owner_password.empty() ? options.user_password
                                                 : options.owner_password,
                  "owner");

    pdf_document* pdf = doc.get();
    const char* out = output.c_str();
    guarded(c, [&](fz_context* g) { pdf_save_document(g, pdf, out, &opts); });
}

void decrypt(const Context& ctx, const std::string& input,
             const std::string& output, const std::string& password) {
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot decrypt with a moved-from Context");
    }
    if (input == output) {
        throw Error(0, "decrypt will not write over its input: " + input);
    }

    OwnedPdfDoc doc = open_and_unlock(c, input, password);

    pdf_write_options opts = base_options();
    opts.do_encrypt = PDF_ENCRYPT_NONE;

    pdf_document* pdf = doc.get();
    const char* out = output.c_str();
    guarded(c, [&](fz_context* g) { pdf_save_document(g, pdf, out, &opts); });
}

}  // namespace leht::ops
