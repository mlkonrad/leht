// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/encrypt.hpp"
#include "leht/renderer.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <string>

using leht::Context;
using leht::Document;
using leht::Renderer;
using leht::ops::decrypt;
using leht::ops::encrypt;
using leht::ops::EncryptOptions;
using leht::ops::Encryption;

namespace {

namespace fs = std::filesystem;

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

class TempPdf {
public:
    explicit TempPdf(const char* name)
        : path_(fs::temp_directory_path() / ("leht_test_" + std::string(name))) {
        fs::remove(path_);
    }
    ~TempPdf() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempPdf(const TempPdf&) = delete;
    TempPdf& operator=(const TempPdf&) = delete;
    [[nodiscard]] std::string str() const { return path_.string(); }

private:
    fs::path path_;
};

void encrypted_document_demands_a_password() {
    Context ctx;
    TempPdf out{"enc_basic.pdf"};

    EncryptOptions options;
    options.user_password = "correct horse";
    encrypt(ctx, corpus("text_10p.pdf"), out.str(), options);

    Document doc = Document::open(ctx, out.str());
    CHECK(doc.needs_password());
    CHECK(!doc.authenticate("wrong password"));
    CHECK(doc.authenticate("correct horse"));
    CHECK(doc.page_count() == 10);
}

void unlocked_document_renders() {
    Context ctx;
    TempPdf out{"enc_render.pdf"};

    EncryptOptions options;
    options.user_password = "hunter2";
    encrypt(ctx, corpus("text_10p.pdf"), out.str(), options);

    Document doc = Document::open(ctx, out.str());
    CHECK(doc.authenticate("hunter2"));

    Renderer renderer{ctx, doc};
    const auto bmp = renderer.render(0, 0.5F);
    CHECK(bmp.has_value());
    CHECK(bmp->width > 0);
}

void decrypt_round_trips() {
    Context ctx;
    TempPdf locked{"enc_rt_locked.pdf"};
    TempPdf opened{"enc_rt_open.pdf"};

    EncryptOptions options;
    options.user_password = "s3cret";
    encrypt(ctx, corpus("text_10p.pdf"), locked.str(), options);
    decrypt(ctx, locked.str(), opened.str(), "s3cret");

    Document doc = Document::open(ctx, opened.str());
    CHECK(!doc.needs_password());
    CHECK(doc.page_count() == 10);
}

void decrypt_with_wrong_password_fails() {
    Context ctx;
    TempPdf locked{"enc_wrong_locked.pdf"};
    TempPdf opened{"enc_wrong_open.pdf"};

    EncryptOptions options;
    options.user_password = "right";
    encrypt(ctx, corpus("text_10p.pdf"), locked.str(), options);

    bool threw = false;
    try {
        decrypt(ctx, locked.str(), opened.str(), "wrong");
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

/// Permission-only encryption: openable by anyone, but flags are recorded.
void empty_user_password_still_opens() {
    Context ctx;
    TempPdf out{"enc_permonly.pdf"};

    EncryptOptions options;
    options.owner_password = "owner-only";
    options.permissions.print = false;
    options.permissions.copy = false;
    encrypt(ctx, corpus("text_10p.pdf"), out.str(), options);

    Document doc = Document::open(ctx, out.str());
    CHECK(!doc.needs_password());
    CHECK(doc.page_count() == 10);
}

void all_methods_produce_readable_files() {
    Context ctx;
    for (const Encryption method :
         {Encryption::Rc4_128, Encryption::Aes128, Encryption::Aes256}) {
        TempPdf out{"enc_method.pdf"};
        EncryptOptions options;
        options.method = method;
        options.user_password = "pw";
        encrypt(ctx, corpus("text_10p.pdf"), out.str(), options);

        Document doc = Document::open(ctx, out.str());
        CHECK(doc.needs_password());
        CHECK(doc.authenticate("pw"));
        CHECK(doc.page_count() == 10);
    }
}

void refuses_to_overwrite_its_input() {
    Context ctx;
    bool threw = false;
    try {
        EncryptOptions options;
        options.user_password = "x";
        encrypt(ctx, corpus("text_10p.pdf"), corpus("text_10p.pdf"), options);
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

void overlong_password_is_rejected() {
    Context ctx;
    TempPdf out{"enc_long.pdf"};
    bool threw = false;
    try {
        EncryptOptions options;
        options.user_password = std::string(200, 'x');
        encrypt(ctx, corpus("text_10p.pdf"), out.str(), options);
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    RUN(encrypted_document_demands_a_password);
    RUN(unlocked_document_renders);
    RUN(decrypt_round_trips);
    RUN(decrypt_with_wrong_password_fails);
    RUN(empty_user_password_still_opens);
    RUN(all_methods_produce_readable_files);
    RUN(refuses_to_overwrite_its_input);
    RUN(overlong_password_is_rejected);
    return 0;
}
