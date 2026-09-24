// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The invisible text layer: text that is found, selected and copied, but
// never drawn. The recogniser is tested in ocr/; this is the PDF half, fed
// words whose boxes are known.
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/ocr_layer.hpp"
#include "leht/ops/merge.hpp"
#include "leht/ops/pages.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::Rect;
using leht::SaveOptions;
using leht::TextPage;
using leht::ops::add_text_layer;
using leht::ops::OcrWord;
using leht::ops::page_has_text;
using leht::test::capture;
using leht::test::corpus;
using leht::test::have_tool;
using leht::test::qpdf_check;
using leht::test::TempPath;

namespace {

/// A page with no text at all: page 1 of the corpus, redrawn as a picture.
std::string scan(const Context& ctx, const TempPath& out) {
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    leht::Renderer r(ctx, doc);
    const auto bmp = r.render(0, 1.0F);
    CHECK(bmp.has_value());
    const TempPath png("ocr_layer_page.png");
    leht::write_png(ctx, *bmp, png.str());
    (void)leht::ops::merge(ctx, {png.str()}, out.str());
    return out.str();
}

const std::vector<OcrWord> kWords = {
    {"Tere", {72, 100, 120, 118}},
    {"õhtust,", {126, 100, 190, 118}},
    {"šokolaad", {196, 100, 270, 118}},
    {"ja", {276, 100, 292, 118}},
    {"žürii", {298, 100, 340, 118}},
};

bool inside(const leht::TextQuad& q, const Rect& box, float slack = 2) {
    return q.min_x() >= box.x0 - slack && q.max_x() <= box.x1 + slack &&
           q.min_y() >= box.y0 - slack && q.max_y() <= box.y1 + slack;
}

void the_layer_is_invisible_and_found() {
    const Context ctx;
    const TempPath scanned("ocr_layer_scan.pdf");
    const TempPath out("ocr_layer_out.pdf");
    Document doc = Document::open(ctx, scan(ctx, scanned));
    CHECK(!page_has_text(ctx, doc, 0));
    const auto before = leht::Renderer(ctx, doc).render(0, 1.0F);

    CHECK(add_text_layer(ctx, doc, 0, kWords) == static_cast<int>(kWords.size()));
    CHECK(page_has_text(ctx, doc, 0));

    // Not one pixel changed.
    const auto after = leht::Renderer(ctx, doc).render(0, 1.0F);
    CHECK(before->pixels == after->pixels);

    // Every word is found where it was said to be, Estonian letters included.
    const TextPage text(ctx, doc, 0);
    for (const OcrWord& w : kWords) {
        const auto hits = text.search(w.text);
        CHECK(hits.size() == 1);
        for (const auto& q : hits.front().quads) {
            CHECK(inside(q, w.box));
        }
    }
    CHECK(text.text().find("Tere õhtust, šokolaad ja žürii") != std::string::npos);

    doc.save(out.str(), SaveOptions{});
    CHECK(qpdf_check(out.str()));
    // An independent extractor reads the same words back.
    if (have_tool("pdftotext")) {
        const std::string extracted = capture("pdftotext '" + out.str() + "' - 2>/dev/null");
        CHECK(extracted.find("õhtust") != std::string::npos);
        CHECK(extracted.find("žürii") != std::string::npos);
    }
}

void the_font_is_added_once() {
    const Context ctx;
    const TempPath scanned("ocr_layer_scan2.pdf");
    const TempPath two("ocr_layer_two.pdf");
    const TempPath out("ocr_layer_once.pdf");
    // Two picture pages, each given a layer.
    (void)leht::ops::merge(ctx, {scan(ctx, scanned), scanned.str()}, two.str());
    Document doc = Document::open(ctx, two.str());
    CHECK(add_text_layer(ctx, doc, 0, kWords) > 0);
    CHECK(add_text_layer(ctx, doc, 1, kWords) > 0);
    CHECK(add_text_layer(ctx, doc, 0, {{"again", {72, 200, 120, 218}}}) == 1);
    doc.save(out.str(), SaveOptions{});
    if (have_tool("qpdf")) {
        const std::string qdf =
            capture("qpdf --qdf --object-streams=disable '" + out.str() + "' - 2>/dev/null");
        std::size_t fonts = 0;
        for (std::size_t at = 0; (at = qdf.find("/Subtype /Type0", at)) != std::string::npos;
             ++at) {
            ++fonts;
        }
        CHECK(fonts == 1);
        CHECK(qdf.find("/FontFile2") != std::string::npos);
    }
    CHECK(TextPage(ctx, doc, 0).search("again").size() == 1);
    CHECK(TextPage(ctx, doc, 1).search("šokolaad").size() == 1);
}

void a_rotated_page_gets_its_words_where_shown() {
    const Context ctx;
    const TempPath scanned("ocr_layer_scan3.pdf");
    const TempPath rotated("ocr_layer_rotated.pdf");
    (void)leht::ops::rotate(ctx, scan(ctx, scanned), rotated.str(), "1", 90);
    Document doc = Document::open(ctx, rotated.str());
    CHECK(add_text_layer(ctx, doc, 0, kWords) > 0);
    const TextPage text(ctx, doc, 0);
    for (const OcrWord& w : kWords) {
        const auto hits = text.search(w.text);
        CHECK(hits.size() == 1);
        for (const auto& q : hits.front().quads) {
            CHECK(inside(q, w.box));
        }
    }
}

void junk_words_are_skipped_and_bad_pages_refused() {
    const Context ctx;
    const TempPath scanned("ocr_layer_scan4.pdf");
    Document doc = Document::open(ctx, scan(ctx, scanned));
    const std::vector<OcrWord> junk = {
        {"", {1, 1, 20, 20}},
        {"empty", {10, 10, 10, 20}},
        {"inf", {0, 0, std::numeric_limits<float>::infinity(), 5}},
        {"\x01\x02", {1, 1, 20, 20}},
    };
    CHECK(add_text_layer(ctx, doc, 0, junk) == 0);
    CHECK(!page_has_text(ctx, doc, 0));
    bool threw = false;
    try {
        (void)add_text_layer(ctx, doc, 7, kWords);
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
    // A born-digital page already has text.
    Document digital = Document::open(ctx, corpus("text_10p.pdf"));
    CHECK(page_has_text(ctx, digital, 0));
}

}  // namespace

int main() {
    RUN(the_layer_is_invisible_and_found);
    RUN(the_font_is_added_once);
    RUN(a_rotated_page_gets_its_words_where_shown);
    RUN(junk_words_are_skipped_and_bad_pages_refused);
    return 0;
}
