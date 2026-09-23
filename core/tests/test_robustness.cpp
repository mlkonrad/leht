// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Error-path hardening. The happy paths are covered elsewhere; this file is
// about what happens when things go wrong, because that is where the untested
// code lives. Every case here must produce a thrown leht::Error -- never a
// crash, never a silent success, never a corrupt output file left behind.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/compress.hpp"
#include "leht/ops/encrypt.hpp"
#include "leht/ops/merge.hpp"
#include "leht/ops/pages.hpp"
#include "leht/renderer.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <functional>
#include <limits>
#include <fstream>
#include <string>
#include <utility>

using leht::Context;
using leht::Document;
using leht::Renderer;

namespace {

namespace fs = std::filesystem;

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

/// True when `operation` throws leht::Error. Anything else -- another exception
/// type, or no exception at all -- is a failure.
template <typename F>
bool throws_leht_error(F&& operation) {
    try {
        operation();
        return false;
    } catch (const leht::Error&) {
        return true;
    } catch (...) {
        return false;  // wrong exception type is also a bug
    }
}

const std::string kUnwritable = "/proc/leht-cannot-write-here/out.pdf";

// -- output paths that cannot be written ------------------------------------

/// Every op must fail cleanly when its output cannot be created. A half-written
/// or zero-byte file left behind would be worse than an error.
void unwritable_output_fails_cleanly() {
    Context ctx;
    const std::string in = corpus("text_10p.pdf");

    CHECK(throws_leht_error([&] { leht::ops::merge(ctx, {in}, kUnwritable); }));
    CHECK(throws_leht_error([&] { leht::ops::compress(ctx, in, kUnwritable); }));
    CHECK(throws_leht_error([&] { leht::ops::extract(ctx, in, kUnwritable, "1"); }));
    CHECK(throws_leht_error([&] { leht::ops::remove_pages(ctx, in, kUnwritable, "1"); }));
    CHECK(throws_leht_error([&] { leht::ops::rotate(ctx, in, kUnwritable, "", 90); }));
    CHECK(throws_leht_error([&] {
        leht::ops::EncryptOptions options;
        options.user_password = "x";
        leht::ops::encrypt(ctx, in, kUnwritable, options);
    }));
}

/// A directory where a file is expected must not be mistaken for a file.
void directory_as_output_fails_cleanly() {
    Context ctx;
    const std::string dir = fs::temp_directory_path().string();
    CHECK(throws_leht_error([&] {
        leht::ops::merge(ctx, {corpus("text_10p.pdf")}, dir);
    }));
}

/// An EMPTY directory is the dangerous case: MuPDF remove()s its output path
/// before writing, and remove() deletes an empty directory. Every writer must
/// refuse it and leave it standing.
void empty_directory_as_output_survives() {
    Context ctx;
    const fs::path dir = fs::temp_directory_path() / "leht_test_empty_output_dir";
    fs::remove_all(dir);
    fs::create_directory(dir);
    const std::string out = dir.string();
    const std::string in = corpus("text_10p.pdf");
    CHECK(throws_leht_error([&] { leht::ops::merge(ctx, {in}, out); }));
    CHECK(throws_leht_error([&] { leht::ops::compress(ctx, in, out); }));
    CHECK(throws_leht_error([&] { leht::ops::extract(ctx, in, out, "1"); }));
    CHECK(throws_leht_error([&] { leht::ops::remove_pages(ctx, in, out, "1"); }));
    CHECK(throws_leht_error([&] { leht::ops::rotate(ctx, in, out, "", 90); }));
    CHECK(throws_leht_error([&] {
        leht::ops::EncryptOptions options;
        options.user_password = "x";
        leht::ops::encrypt(ctx, in, out, options);
    }));
    CHECK(throws_leht_error([&] {
        Document doc = Document::open(ctx, in);
        leht::Renderer renderer(ctx, doc);
        const auto bmp = renderer.render(0, 0.25F);
        CHECK(bmp.has_value());
        leht::write_png(ctx, *bmp, out);
    }));
    CHECK(fs::is_directory(dir));
    fs::remove_all(dir);
}

void directory_as_input_fails_cleanly() {
    Context ctx;
    const std::string dir = fs::temp_directory_path().string();
    CHECK(throws_leht_error([&] { Document::open(ctx, dir); }));
}

// -- malformed and degenerate inputs ----------------------------------------

/// The damaged corpus file through every op. MuPDF may repair it or reject it;
/// either is fine. Crashing is not.
void damaged_input_through_every_op() {
    Context ctx;
    const std::string in = corpus("damaged.pdf");
    const std::string out =
        (fs::temp_directory_path() / "leht_robust_damaged.pdf").string();

    // No CHECK on the outcome: repair and rejection are both acceptable. The
    // assertion is that control returns here at all.
    for (auto&& op : {
             std::function<void()>{[&] { leht::ops::merge(ctx, {in}, out); }},
             std::function<void()>{[&] { leht::ops::compress(ctx, in, out); }},
             std::function<void()>{[&] { leht::ops::extract(ctx, in, out, "1"); }},
             std::function<void()>{[&] { leht::ops::rotate(ctx, in, out, "", 90); }},
         }) {
        try {
            op();
        } catch (const leht::Error&) {
        }
    }
    std::error_code ec;
    fs::remove(out, ec);
    CHECK(true);  // reaching here without crashing is the test
}

void empty_file_is_rejected() {
    Context ctx;
    const fs::path empty = fs::temp_directory_path() / "leht_robust_empty.pdf";
    { std::ofstream create(empty, std::ios::binary | std::ios::trunc); }

    CHECK(throws_leht_error([&] { Document::open(ctx, empty.string()); }));
    std::error_code ec;
    fs::remove(empty, ec);
}

void empty_buffer_is_rejected() {
    Context ctx;
    CHECK(throws_leht_error([&] {
        Document::open_memory(ctx, nullptr, 0);
    }));
    const unsigned char byte = 'x';
    CHECK(throws_leht_error([&] {
        Document::open_memory(ctx, &byte, 0);
    }));
}

void garbage_buffer_is_rejected() {
    Context ctx;
    const std::string junk(4096, '\xAB');
    CHECK(throws_leht_error([&] {
        Document::open_memory(ctx, junk.data(), junk.size());
    }));
}

// -- degenerate render parameters -------------------------------------------

/// Invalid zoom must be rejected at the API boundary, before MuPDF sees it.
///
/// This used to accept either a throw or a result. It now REQUIRES a throw:
/// NaN in particular slips past MuPDF's coordinate clamp (every comparison with
/// NaN is false) into a float-to-int conversion, which is undefined behaviour,
/// and NaN also used to make render() "succeed" with an empty bitmap.
void invalid_zoom_is_rejected() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    Renderer renderer{ctx, doc};

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (const float zoom : {0.0F, -0.0F, -1.0F, -0.0001F, nan, inf, -inf,
                             64.0001F, 1000.0F, 1e30F}) {
        CHECK(throws_leht_error([&] { (void)renderer.render(0, zoom); }));
        CHECK(throws_leht_error([&] { (void)renderer.page_size(0, zoom); }));
    }

    // The boundary itself, and ordinary values, must still work.
    for (const float zoom : {0.01F, 1.0F, 2.5F}) {
        const auto bitmap = renderer.render(0, zoom);
        CHECK(bitmap.has_value());
        CHECK(bitmap->width > 0);
    }
    CHECK(renderer.page_size(0, 64.0F).width > 0);
}

/// Rotations that are not multiples of 90 are rejected by the ops layer, but
/// the renderer accepts arbitrary angles; neither may crash.
void odd_rotations_do_not_crash() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    Renderer renderer{ctx, doc};

    for (const int degrees : {0, 90, 180, 270, 360, -90, 720, 45}) {
        const leht::PageSize size = renderer.page_size(0, 0.25F, degrees);
        CHECK(size.width > 0);
        CHECK(size.height > 0);
    }
}

void out_of_range_page_is_rejected() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    Renderer renderer{ctx, doc};

    CHECK(throws_leht_error([&] { (void)renderer.render(-1, 1.0F); }));
    CHECK(throws_leht_error([&] { (void)renderer.render(999, 1.0F); }));
    CHECK(throws_leht_error([&] { (void)renderer.page_size(999, 1.0F); }));
}

// -- moved-from objects ------------------------------------------------------

/// Using a moved-from Context must throw, not dereference a null fz_context.
void moved_from_context_is_rejected() {
    Context ctx;
    Context taken{std::move(ctx)};
    (void)taken;

    // NOLINTBEGIN(bugprone-use-after-move) -- deliberately testing this
    CHECK(throws_leht_error([&] { Document::open(ctx, corpus("text_10p.pdf")); }));
    CHECK(throws_leht_error([&] {
        Document::open_memory(ctx, "x", 1);
    }));
    CHECK(throws_leht_error([&] { (void)ctx.clone(); }));
    CHECK(throws_leht_error([&] {
        leht::ops::merge(ctx, {corpus("text_10p.pdf")}, "/tmp/leht_never.pdf");
    }));
    // NOLINTEND(bugprone-use-after-move)
}

// -- self-overwrite refusal --------------------------------------------------

/// compress and encrypt refuse to write over their input. That is a deliberate
/// guarantee the CLI relies on, so it gets a test of its own rather than being
/// left to the ops-specific suites.
void ops_refuse_to_overwrite_input() {
    Context ctx;
    const std::string in = corpus("text_10p.pdf");

    CHECK(throws_leht_error([&] { leht::ops::compress(ctx, in, in); }));
    CHECK(throws_leht_error([&] {
        leht::ops::EncryptOptions options;
        options.user_password = "x";
        leht::ops::encrypt(ctx, in, in, options);
    }));
    CHECK(throws_leht_error([&] { leht::ops::decrypt(ctx, in, in, ""); }));

    // The input must be untouched afterwards.
    CHECK(fs::exists(in));
    CHECK(fs::file_size(in) > 0);
}

}  // namespace

int main() {
    RUN(unwritable_output_fails_cleanly);
    RUN(directory_as_output_fails_cleanly);
    RUN(empty_directory_as_output_survives);
    RUN(directory_as_input_fails_cleanly);
    RUN(damaged_input_through_every_op);
    RUN(empty_file_is_rejected);
    RUN(empty_buffer_is_rejected);
    RUN(garbage_buffer_is_rejected);
    RUN(invalid_zoom_is_rejected);
    RUN(odd_rotations_do_not_crash);
    RUN(out_of_range_page_is_rejected);
    RUN(moved_from_context_is_rejected);
    RUN(ops_refuse_to_overwrite_input);
    return 0;
}
