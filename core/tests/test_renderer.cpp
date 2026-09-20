// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/renderer.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <optional>
#include <string>

using leht::Bitmap;
using leht::Cancel;
using leht::Context;
using leht::Document;
using leht::PageSize;
using leht::Renderer;

namespace {

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

/// Counts pixels that are not pure white. A text page must have some.
std::size_t ink_pixels(const Bitmap& bmp) {
    std::size_t ink = 0;
    for (int y = 0; y < bmp.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) *
                                static_cast<std::size_t>(bmp.stride);
        for (int x = 0; x < bmp.width; ++x) {
            const std::size_t i =
                row + static_cast<std::size_t>(x) *
                          static_cast<std::size_t>(bmp.channels);
            if (bmp.pixels[i] != 0xFF || bmp.pixels[i + 1] != 0xFF ||
                bmp.pixels[i + 2] != 0xFF) {
                ++ink;
            }
        }
    }
    return ink;
}

void renders_a_page_with_ink() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    Renderer renderer{ctx, doc};

    std::optional<Bitmap> bmp = renderer.render(0, 1.0F);
    CHECK(bmp.has_value());
    CHECK(bmp->width > 0);
    CHECK(bmp->height > 0);
    CHECK(bmp->channels == 3);
    CHECK(bmp->stride >= bmp->width * bmp->channels);
    // A blank render would be the classic silent failure here.
    CHECK(ink_pixels(*bmp) > 100);
}

void zoom_scales_dimensions() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    Renderer renderer{ctx, doc};

    const std::optional<Bitmap> small = renderer.render(0, 1.0F);
    const std::optional<Bitmap> large = renderer.render(0, 2.0F);
    CHECK(small.has_value() && large.has_value());
    // Allow a pixel of rounding slack on each edge.
    CHECK(large->width >= small->width * 2 - 2);
    CHECK(large->height >= small->height * 2 - 2);
}

/// page_size() must agree with what render() actually produces, or viewer
/// layout will disagree with the pixels and everything will jitter.
void page_size_matches_render() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    Renderer renderer{ctx, doc};

    const PageSize size = renderer.page_size(0, 1.5F);
    const std::optional<Bitmap> bmp = renderer.render(0, 1.5F);
    CHECK(bmp.has_value());
    CHECK(size.width == bmp->width);
    CHECK(size.height == bmp->height);
}

void rotation_swaps_axes() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    Renderer renderer{ctx, doc};

    const PageSize upright = renderer.page_size(0, 1.0F, 0);
    const PageSize turned = renderer.page_size(0, 1.0F, 90);
    CHECK(upright.width == turned.height);
    CHECK(upright.height == turned.width);
}

void cancelled_render_returns_nullopt() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_160p.pdf"));
    Renderer renderer{ctx, doc};

    Cancel cancel;
    cancel.request();  // pre-aborted: MuPDF must bail out
    const std::optional<Bitmap> bmp = renderer.render(0, 1.0F, 0, &cancel);
    CHECK(!bmp.has_value());
    CHECK(cancel.requested());

    // After reset the same token must work normally again.
    cancel.reset();
    CHECK(!cancel.requested());
    const std::optional<Bitmap> ok = renderer.render(0, 1.0F, 0, &cancel);
    CHECK(ok.has_value());
}

void display_list_cache_is_bounded() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_160p.pdf"));
    Renderer renderer{ctx, doc, /*max_cached_lists=*/4};

    for (int page = 0; page < 20; ++page) {
        const std::optional<Bitmap> bmp = renderer.render(page, 0.5F);
        CHECK(bmp.has_value());
    }
    CHECK(renderer.cached_list_count() <= 4);

    renderer.clear_cache();
    CHECK(renderer.cached_list_count() == 0);
}

void damaged_file_does_not_crash() {
    Context ctx;
    bool handled = false;
    try {
        Document doc = Document::open(ctx, corpus("damaged.pdf"));
        Renderer renderer{ctx, doc};
        const std::optional<Bitmap> bmp = renderer.render(0, 1.0F);
        (void)bmp;
        handled = true;  // repaired by MuPDF, also an acceptable outcome
    } catch (const leht::Error&) {
        handled = true;  // rejected cleanly, likewise acceptable
    }
    CHECK(handled);  // the only failure is a crash or a hang
}

}  // namespace

int main() {
    RUN(renders_a_page_with_ink);
    RUN(zoom_scales_dimensions);
    RUN(page_size_matches_render);
    RUN(rotation_swaps_axes);
    RUN(cancelled_render_returns_nullopt);
    RUN(display_list_cache_is_bounded);
    RUN(damaged_file_does_not_crash);
    return 0;
}
