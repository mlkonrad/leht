// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/compress.hpp"
#include "leht/ops/merge.hpp"
#include "leht/renderer.hpp"
#include "test_harness.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

using leht::Context;
using leht::Document;
using leht::Renderer;
using leht::ops::compress;
using leht::ops::CompressOptions;
using leht::ops::CompressPreset;
using leht::ops::CompressResult;
using leht::ops::merge;

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
    [[nodiscard]] std::uintmax_t size() const { return fs::file_size(path_); }

private:
    fs::path path_;
};

/// Three copies of a 150 DPI scan: the case compression is actually for.
void build_image_heavy(Context& ctx, const TempPdf& out) {
    merge(ctx, {corpus("scan.jpg"), corpus("scan.jpg"), corpus("scan.jpg")},
          out.str());
}

void screen_preset_shrinks_an_image_heavy_pdf() {
    Context ctx;
    TempPdf source{"comp_src.pdf"};
    TempPdf out{"comp_screen.pdf"};
    build_image_heavy(ctx, source);

    CompressOptions options;
    options.preset = CompressPreset::Screen;
    const CompressResult result = compress(ctx, source.str(), out.str(), options);

    std::printf("      %zu -> %zu bytes (%.1f%% saved, %d/%d images)\n",
                result.input_bytes, result.output_bytes,
                result.saved_fraction() * 100.0, result.images_recompressed,
                result.images_examined);

    CHECK(result.images_examined > 0);
    CHECK(result.images_recompressed > 0);
    CHECK(result.output_bytes < result.input_bytes);
    CHECK(result.saved_fraction() > 0.5);  // 150 DPI -> 72 DPI is a big cut
}

/// Ordering must hold: screen smaller than ebook, ebook smaller than print.
void presets_are_ordered_by_size() {
    Context ctx;
    TempPdf source{"comp_ord_src.pdf"};
    TempPdf screen{"comp_ord_screen.pdf"};
    TempPdf ebook{"comp_ord_ebook.pdf"};
    TempPdf print{"comp_ord_print.pdf"};
    build_image_heavy(ctx, source);

    CompressOptions opt;
    opt.preset = CompressPreset::Screen;
    const auto s = compress(ctx, source.str(), screen.str(), opt);
    opt.preset = CompressPreset::Ebook;
    const auto e = compress(ctx, source.str(), ebook.str(), opt);
    opt.preset = CompressPreset::Print;
    const auto p = compress(ctx, source.str(), print.str(), opt);

    CHECK(s.output_bytes <= e.output_bytes);
    CHECK(e.output_bytes <= p.output_bytes);
}

void lossless_preset_touches_no_images() {
    Context ctx;
    TempPdf source{"comp_ll_src.pdf"};
    TempPdf out{"comp_ll.pdf"};
    build_image_heavy(ctx, source);

    CompressOptions options;
    options.preset = CompressPreset::Lossless;
    const CompressResult result = compress(ctx, source.str(), out.str(), options);

    CHECK(result.images_recompressed == 0);
    CHECK(result.output_bytes > 0);
    // Structural work alone must not inflate an already-tidy file much.
    CHECK(result.output_bytes <= result.input_bytes + 4096);
}

/// A text-only PDF has nothing to downsample; compression must still succeed
/// and must not produce a bigger file.
void text_only_pdf_does_not_inflate() {
    Context ctx;
    TempPdf out{"comp_text.pdf"};
    CompressOptions options;
    options.preset = CompressPreset::Screen;
    const CompressResult result =
        compress(ctx, corpus("text_160p.pdf"), out.str(), options);

    CHECK(result.output_bytes > 0);
    CHECK(result.output_bytes <= result.input_bytes + 4096);
}

void compressed_output_still_renders() {
    Context ctx;
    TempPdf source{"comp_render_src.pdf"};
    TempPdf out{"comp_render.pdf"};
    build_image_heavy(ctx, source);

    CompressOptions options;
    options.preset = CompressPreset::Screen;
    compress(ctx, source.str(), out.str(), options);

    Document doc = Document::open(ctx, out.str());
    CHECK(doc.page_count() == 3);

    Renderer renderer{ctx, doc};
    for (int page = 0; page < 3; ++page) {
        const auto bmp = renderer.render(page, 0.5F);
        CHECK(bmp.has_value());
        CHECK(bmp->width > 0 && bmp->height > 0);
    }
}

/// Refusing to write over the input is what lets a caller show before/after
/// sizes and offer the user a choice.
void refuses_to_overwrite_its_input() {
    Context ctx;
    TempPdf source{"comp_same.pdf"};
    build_image_heavy(ctx, source);

    bool threw = false;
    try {
        compress(ctx, source.str(), source.str());
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

void missing_input_throws() {
    Context ctx;
    TempPdf out{"comp_missing.pdf"};
    bool threw = false;
    try {
        compress(ctx, corpus("nope-not-here.pdf"), out.str());
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}


/// Creates `path` for writing, read-write as Document::save_fd() requires.
int open_write(const std::string& path) {
    return ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

void compresses_an_open_document_into_a_descriptor() {
    Context ctx;
    TempPdf heavy{"compress_fd_in.pdf"};
    build_image_heavy(ctx, heavy);
    TempPdf out{"compress_fd_out.pdf"};

    Document doc = Document::open(ctx, heavy.str());
    leht::ops::CompressOptions options;
    options.preset = leht::ops::CompressPreset::Screen;
    const int fd = open_write(out.str());
    const CompressResult result = leht::ops::compress(ctx, doc, fd, options);
    ::close(fd);

    CHECK(result.input_bytes == 0);  // the caller's to fill in
    CHECK(result.images_recompressed > 0);
    CHECK(result.output_bytes == out.size());
    CHECK(out.size() < heavy.size());

    // Byte-for-byte what the path-based entry point writes, so the viewer and
    // `leht compress` agree.
    TempPdf by_path{"compress_fd_by_path.pdf"};
    (void)leht::ops::compress(ctx, heavy.str(), by_path.str(), options);
    CHECK(by_path.size() == out.size());

    Document reopened = Document::open(ctx, out.str());
    CHECK(reopened.page_count() == 3);
}

}  // namespace

int main() {
    RUN(screen_preset_shrinks_an_image_heavy_pdf);
    RUN(presets_are_ordered_by_size);
    RUN(lossless_preset_touches_no_images);
    RUN(text_only_pdf_does_not_inflate);
    RUN(compressed_output_still_renders);
    RUN(refuses_to_overwrite_its_input);
    RUN(missing_input_throws);
    RUN(compresses_an_open_document_into_a_descriptor);
    return 0;
}
