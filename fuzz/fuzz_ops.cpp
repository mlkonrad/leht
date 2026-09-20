// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fuzz target for the ops layer.
//
// fuzz_open.cpp covers parsing. This covers what happens AFTER parsing, which
// is the more dangerous half: compress walks every object in the document,
// decodes image streams, rescales pixmaps and rewrites the streams in place.
// merge grafts objects between documents. Both do far more with attacker-shaped
// structure than simply opening a file does, and neither had any fuzz coverage.
//
// The ops API takes paths rather than buffers, so each input is written to a
// scratch file first. That makes iterations slower than fuzz_open's, which is
// the price of fuzzing the real entry points rather than a parallel one.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/compress.hpp"
#include "leht/ops/encrypt.hpp"
#include "leht/ops/merge.hpp"
#include "leht/ops/pages.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

leht::Context& shared_context() {
    static leht::Context ctx;
    return ctx;
}

/// One scratch directory per process, cleaned on exit.
const fs::path& scratch() {
    static const fs::path dir = [] {
        fs::path p = fs::temp_directory_path() /
                     ("leht-fuzz-ops-" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return dir;
}

/// Runs one operation, absorbing the failures that are expected on malformed
/// input. Anything it does not absorb -- a crash, a sanitizer report -- is the
/// bug we are looking for.
template <typename F>
void attempt(F&& operation) {
    try {
        operation();
    } catch (const leht::Error&) {
        // Expected: malformed input must be rejected, not crash.
    } catch (const std::bad_alloc&) {
        // Expected: a fuzzed length field can ask for an absurd allocation.
    } catch (const fs::filesystem_error&) {
        // Expected: output paths can fail for ordinary reasons.
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size == 0 || size > (4U << 20)) {
        return 0;
    }

    const fs::path input = scratch() / "in.pdf";
    const fs::path output = scratch() / "out.pdf";
    {
        std::ofstream out(input, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(size));
        if (!out) {
            return 0;
        }
    }

    leht::Context& ctx = shared_context();
    const std::string in = input.string();
    const std::string out = output.string();

    // Cheapest first: if the document will not even open, the ops below will
    // all reject it identically and there is nothing more to learn.
    bool openable = false;
    attempt([&] {
        leht::Document doc = leht::Document::open(ctx, in);
        openable = doc.page_count() > 0 && !doc.needs_password();
    });
    if (!openable) {
        return 0;
    }

    // Compression: the object walk, image decode, rescale and stream rewrite.
    for (const leht::ops::CompressPreset preset :
         {leht::ops::CompressPreset::Lossless,
          leht::ops::CompressPreset::Screen}) {
        attempt([&] {
            leht::ops::CompressOptions options;
            options.preset = preset;
            (void)leht::ops::compress(ctx, in, out, options);
        });
    }

    // Page surgery: grafting objects into a fresh document.
    attempt([&] { (void)leht::ops::extract(ctx, in, out, "1"); });
    attempt([&] { (void)leht::ops::rotate(ctx, in, out, "", 90); });
    attempt([&] { (void)leht::ops::remove_pages(ctx, in, out, "1"); });

    // Merging a document with itself exercises graft collision handling.
    attempt([&] { (void)leht::ops::merge(ctx, {in, in}, out); });

    // Encryption rewrites every stream through the crypt filter.
    attempt([&] {
        leht::ops::EncryptOptions options;
        options.user_password = "fuzz";
        leht::ops::encrypt(ctx, in, out, options);
        leht::ops::decrypt(ctx, out, (scratch() / "dec.pdf").string(), "fuzz");
    });

    return 0;
}
