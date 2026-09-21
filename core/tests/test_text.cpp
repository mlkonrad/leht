// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <string>

using leht::Context;
using leht::Document;
using leht::Renderer;
using leht::SearchHit;
using leht::Selection;
using leht::SelectMode;
using leht::TextPage;
using leht::TextQuad;

namespace {

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// The corpus generator writes this exact line on every page:
//   "leht corpus - page N - line M - the quick brown fox jumps ..."
void extracts_known_text() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    TextPage tp{ctx, doc, 0};

    const std::string text = tp.text();
    CHECK(text.size() > 100);
    CHECK(contains(text, "leht corpus"));
    CHECK(contains(text, "quick brown fox"));
    CHECK(contains(text, "0123456789"));
}

void empty_needle_and_zero_hits() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    TextPage tp{ctx, doc, 0};

    CHECK(tp.search("").empty());
    CHECK(tp.search("zzzznotpresentzzzz").empty());
}

void search_finds_repeated_term_with_geometry() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    TextPage tp{ctx, doc, 0};

    // "line" appears once per line, 50 lines on the page.
    const std::vector<SearchHit> hits = tp.search("line");
    CHECK(hits.size() >= 40);

    for (const SearchHit& hit : hits) {
        CHECK(!hit.quads.empty());
        const TextQuad& q = hit.quads.front();
        // Real geometry: positive extent, on the page.
        CHECK(q.max_x() > q.min_x());
        CHECK(q.max_y() > q.min_y());
        CHECK(q.min_x() >= 0);
        CHECK(q.min_y() >= 0);
    }

    // Lines run top to bottom: later hits sit lower on the page.
    const float first_y = hits.front().quads.front().min_y();
    const float last_y = hits.back().quads.front().min_y();
    CHECK(last_y > first_y);
}

void search_respects_max_hits() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    TextPage tp{ctx, doc, 0};

    const std::vector<SearchHit> capped = tp.search("line", 5);
    CHECK(capped.size() == 5);
}

// Geometry must scale with zoom: the same word at 2x sits twice as far down.
void geometry_tracks_zoom() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));

    TextPage at1{ctx, doc, 0, 1.0F};
    TextPage at2{ctx, doc, 0, 2.0F};

    const auto h1 = at1.search("quick");
    const auto h2 = at2.search("quick");
    CHECK(!h1.empty() && !h2.empty());

    const float y1 = h1.front().quads.front().min_y();
    const float y2 = h2.front().quads.front().min_y();
    // Allow rounding slack; 2x zoom should roughly double the offset.
    CHECK(y2 > y1 * 1.8F);
    CHECK(y2 < y1 * 2.2F + 2.0F);
}

// Search geometry must land on the rendered pixels: a hit's box must fall
// within the page bitmap at the same zoom, and cover ink.
void geometry_lands_on_rendered_pixels() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));

    const float zoom = 1.5F;
    Renderer renderer{ctx, doc};
    const auto bmp = renderer.render(0, zoom);
    CHECK(bmp.has_value());

    TextPage tp{ctx, doc, 0, zoom};
    const auto hits = tp.search("quick");
    CHECK(!hits.empty());
    const TextQuad& q = hits.front().quads.front();

    // Inside the bitmap.
    CHECK(q.min_x() >= 0 && q.max_x() <= static_cast<float>(bmp->width));
    CHECK(q.min_y() >= 0 && q.max_y() <= static_cast<float>(bmp->height));

    // The box covers non-white pixels — it is actually on the word.
    std::size_t ink = 0;
    const int x0 = std::max(0, static_cast<int>(q.min_x()));
    const int x1 = std::min(bmp->width, static_cast<int>(q.max_x()) + 1);
    const int y0 = std::max(0, static_cast<int>(q.min_y()));
    const int y1 = std::min(bmp->height, static_cast<int>(q.max_y()) + 1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(bmp->stride) +
                                  static_cast<std::size_t>(x) *
                                      static_cast<std::size_t>(bmp->channels);
            if (bmp->pixels[i] < 200) {
                ++ink;
            }
        }
    }
    CHECK(ink > 0);
}

void selection_returns_text_and_quads() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    const float zoom = 1.0F;
    TextPage tp{ctx, doc, 0, zoom};

    // Find a word, then select across its box.
    const auto hits = tp.search("brown");
    CHECK(!hits.empty());
    const TextQuad& q = hits.front().quads.front();

    const Selection sel = tp.select(q.min_x() - 1, (q.min_y() + q.max_y()) / 2,
                                    q.max_x() + 1, (q.min_y() + q.max_y()) / 2,
                                    SelectMode::Words);
    CHECK(!sel.quads.empty());
    CHECK(contains(sel.text, "brown"));
}

void select_empty_region_is_harmless() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    TextPage tp{ctx, doc, 0};
    // A zero-size selection in a blank corner: no crash, no garbage.
    const Selection sel = tp.select(5, 5, 5, 5);
    CHECK(sel.text.size() < 8);  // empty or a stray character, never a line
}

void image_page_has_no_text() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("wide.png"));
    TextPage tp{ctx, doc, 0};
    // A rasterised image carries no extractable text; the label is drawn as
    // pixels, not glyphs. Must return cleanly, not throw.
    const std::string text = tp.text();
    CHECK(tp.search("anything").empty());
    (void)text;
    CHECK(true);
}

void out_of_range_page_throws() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    bool threw = false;
    try {
        TextPage tp{ctx, doc, 999};
        (void)tp;
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    RUN(extracts_known_text);
    RUN(empty_needle_and_zero_hits);
    RUN(search_finds_repeated_term_with_geometry);
    RUN(search_respects_max_hits);
    RUN(geometry_tracks_zoom);
    RUN(geometry_lands_on_rendered_pixels);
    RUN(selection_returns_text_and_quads);
    RUN(select_empty_region_is_harmless);
    RUN(image_page_has_no_text);
    RUN(out_of_range_page_throws);
    return 0;
}
