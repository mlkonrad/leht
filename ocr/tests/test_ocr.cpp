// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Reading words off a rendered page. The pages are rendered from PDFs whose
// text is known, so what Tesseract reads can be checked -- words and where
// they are.
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ocr/ocr.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::ocr::Recognizer;
using leht::ops::OcrWord;
using leht::test::TempPath;
using leht::test::corpus;

namespace {

constexpr float kZoom = 300.0F / 72.0F;  // 300 dpi

std::string datadir() { return leht::ocr::default_datadir(); }

bool installed(const std::string& lang) {
    const auto all = leht::ocr::installed_languages(datadir());
    return std::find(all.begin(), all.end(), lang) != all.end();
}

std::vector<OcrWord> read_page(const std::string& path, const std::string& langs) {
    const Context ctx;
    Document doc = Document::open(ctx, path);
    const auto bmp = leht::Renderer(ctx, doc).render(0, kZoom);
    CHECK(bmp.has_value());
    Recognizer r(langs, datadir());
    return r.recognize(*bmp, kZoom);
}

const OcrWord* find(const std::vector<OcrWord>& words, const std::string& text) {
    for (const OcrWord& w : words) {
        if (w.text == text) {
            return &w;
        }
    }
    return nullptr;
}

void english_words_are_read_where_they_are() {
    const std::string page = corpus("text_10p.pdf");
    const auto words = read_page(page, "eng");
    CHECK(words.size() > 300);  // 50 lines of 13 words, give or take
    const OcrWord* quick = find(words, "quick");
    CHECK(quick != nullptr);

    // Its box is where the PDF says the first "quick" is, within 3 points.
    const Context ctx;
    Document doc = Document::open(ctx, page);
    const auto hit = leht::TextPage(ctx, doc, 0).search("quick").front().quads.front();
    CHECK(std::abs(quick->box.x0 - hit.min_x()) < 3 && std::abs(quick->box.x1 - hit.max_x()) < 3);
    CHECK(std::abs(quick->box.y0 - hit.min_y()) < 4 && std::abs(quick->box.y1 - hit.max_y()) < 4);
}

void estonian_letters_are_read() {
    if (!installed("est")) {
        std::printf("  SKIP estonian (tesseract-langpack-est not installed)\n");
        return;
    }
    // Helvetica in WinAnsi has o-tilde (365), s-caron (232), z-caron (236)
    // and u-umlaut (374): "Tere õhtust, šokolaad ja žürii".
    leht::test::PdfWriter pdf;
    pdf.set(1, "<< /Type /Catalog /Pages 2 0 R >>");
    pdf.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    pdf.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 200] /Contents 4 0 R "
               "/Resources << /Font << /F1 5 0 R >> >> >>");
    pdf.set(4, leht::test::PdfWriter::stream(
                   "", "BT /F1 28 Tf 40 90 Td (Tere \\365htust, \\232okolaad ja \\236\\374rii) "
                       "Tj ET"));
    pdf.set(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
               "/Encoding /WinAnsiEncoding >>");
    const TempPath path("ocr_estonian.pdf");
    leht::test::write_file(path.str(), pdf.finish(1));

    const auto words = read_page(path.str(), "est");
    std::string all;
    for (const OcrWord& w : words) {
        all += w.text + " ";
    }
    if (all.find("õhtust") == std::string::npos || all.find("žürii") == std::string::npos) {
        std::fprintf(stderr, "read: %s\n", all.c_str());
    }
    CHECK(all.find("õhtust") != std::string::npos);
    // The caron must survive; its case may not. At this size a small š and a
    // capital Š differ only in height, and Tesseract reads either.
    CHECK(all.find("šokolaad") != std::string::npos || all.find("Šokolaad") != std::string::npos);
    CHECK(all.find("žürii") != std::string::npos);
}

void a_blank_page_has_no_words() {
    // No guesses on nothing: the confidence floor keeps noise out.
    leht::Bitmap blank;
    blank.width = 800;
    blank.height = 600;
    blank.channels = 3;
    blank.stride = 800 * 3;
    blank.pixels.assign(static_cast<std::size_t>(blank.stride) * 600, 255);
    Recognizer r("eng", datadir());
    CHECK(r.recognize(blank, kZoom).empty());
}

void unknown_languages_and_bad_images_are_refused() {
    bool threw = false;
    try {
        Recognizer r("klingon", datadir());
    } catch (const leht::Error& e) {
        threw = std::string(e.what()).find("'klingon' is not installed") != std::string::npos;
    }
    CHECK(threw);
    threw = false;
    try {
        Recognizer r("", datadir());
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
    Recognizer r("eng", datadir());
    leht::Bitmap bad;  // no pixels
    threw = false;
    try {
        (void)r.recognize(bad, kZoom);
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    if (datadir().empty() || !installed("eng")) {
        std::printf("SKIP: no Tesseract English data (dnf install tesseract-langpack-eng)\n");
        return 77;
    }
    RUN(english_words_are_read_where_they_are);
    RUN(estonian_letters_are_read);
    RUN(a_blank_page_has_no_words);
    RUN(unknown_languages_and_bad_images_are_refused);
    return 0;
}
