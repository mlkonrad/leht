// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/crop.hpp"
#include "leht/ops/pages.hpp"
#include "leht/ops/watermark.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using leht::Context;
using leht::Document;
using leht::Rect;
using leht::SaveOptions;
using leht::TextPage;
using leht::ops::crop;
using leht::ops::crop_margins;
using leht::ops::Margins;
using leht::ops::watermark;
using leht::ops::WatermarkOptions;
using leht::test::capture;
using leht::test::corpus;
using leht::test::have_tool;
using leht::test::PdfWriter;
using leht::test::qpdf_check;
using leht::test::TempPath;
using leht::test::write_file;

namespace {

template <typename F>
bool throws(F&& fn) {
    try {
        fn();
        return false;
    } catch (const leht::Error&) {
        return true;
    }
}

leht::PageSize size_of(const Context& ctx, Document& doc, int page) {
    leht::Renderer r(ctx, doc);
    return r.page_size(page, 1.0F);
}

// --- crop --------------------------------------------------------------------

void crop_sets_the_visible_area() {
    const Context ctx;
    const TempPath out("crop_box.pdf");
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    const auto before = size_of(ctx, doc, 0);
    CHECK(crop(ctx, doc, "1-2", Rect{50, 60, 350, 460}) == 2);
    doc.save(out.str(), SaveOptions{});

    Document again = Document::open(ctx, out.str());
    const auto cropped = size_of(ctx, again, 0);
    CHECK(cropped.width == 300 && cropped.height == 400);
    const auto untouched = size_of(ctx, again, 2);
    CHECK(untouched.width == before.width && untouched.height == before.height);
    CHECK(qpdf_check(out.str()));

    // poppler agrees on the new size.
    if (have_tool("pdfinfo")) {
        const std::string info = capture("pdfinfo -f 1 -l 1 '" + out.str() + "' 2>/dev/null");
        CHECK(info.find("300 x 400 pts") != std::string::npos);
    }
}

void crop_hides_but_does_not_remove() {
    const Context ctx;
    const TempPath out("crop_hides.pdf");
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    // A box below the first line of text.
    (void)crop(ctx, doc, "1", Rect{0, 200, 400, 400});
    doc.save(out.str(), SaveOptions{});
    // The first line's text is still in the file: this is not redaction.
    CHECK(capture("qpdf --qdf --object-streams=disable '" + out.str() + "' - 2>/dev/null")
                  .find("line 0") != std::string::npos ||
          !have_tool("qpdf"));
}

void crop_margins_trims_each_page() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    const auto before = size_of(ctx, doc, 0);
    CHECK(crop_margins(ctx, doc, "", Margins{36, 10, 36, 20}) == 10);
    const auto after = size_of(ctx, doc, 0);
    CHECK(after.width == before.width - 72);
    CHECK(after.height == before.height - 30);
}

void crop_follows_the_page_as_displayed() {
    // On a page rotated 90 degrees, a wide box must stay wide on screen.
    const Context ctx;
    const TempPath rotated("crop_rotated_in.pdf");
    (void)leht::ops::rotate(ctx, corpus("text_10p.pdf"), rotated.str(), "1", 90);
    Document doc = Document::open(ctx, rotated.str());
    const auto before = size_of(ctx, doc, 0);
    CHECK(before.width > before.height);  // landscape as displayed
    (void)crop(ctx, doc, "1", Rect{10, 20, 310, 120});
    const auto after = size_of(ctx, doc, 0);
    CHECK(after.width == 300 && after.height == 100);
}

void crop_refuses_nonsense() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    CHECK(throws([&] { (void)crop(ctx, doc, "", Rect{5000, 5000, 6000, 6000}); }));
    CHECK(throws([&] { (void)crop(ctx, doc, "", Rect{100, 100, 50, 50}); }));
    CHECK(throws([&] { (void)crop(ctx, doc, "", Rect{0, 0, NAN, 10}); }));
    CHECK(throws([&] { (void)crop_margins(ctx, doc, "", Margins{-1, 0, 0, 0}); }));
    CHECK(throws([&] { (void)crop_margins(ctx, doc, "", Margins{400, 0, 400, 0}); }));
    CHECK(throws([&] { (void)crop(ctx, doc, "99", Rect{0, 0, 10, 10}); }));
    Document png = Document::open(ctx, corpus("page.png"));
    CHECK(throws([&] { (void)crop(ctx, png, "", Rect{0, 0, 10, 10}); }));
}

// --- watermark ---------------------------------------------------------------

/// Pixels that differ between two renders of the same page, and where their
/// centre of mass lies.
struct Diff {
    long count = 0;
    double cx = 0, cy = 0;
};

Diff diff(const leht::Bitmap& a, const leht::Bitmap& b) {
    Diff d;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const auto at = static_cast<std::size_t>(y * a.stride + x * a.channels);
            if (a.pixels[at] != b.pixels[at] || a.pixels[at + 1] != b.pixels[at + 1]) {
                ++d.count;
                d.cx += x;
                d.cy += y;
            }
        }
    }
    if (d.count > 0) {
        d.cx /= static_cast<double>(d.count);
        d.cy /= static_cast<double>(d.count);
    }
    return d;
}

void watermark_marks_the_chosen_pages() {
    const Context ctx;
    const TempPath out("watermark.pdf");
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    leht::Renderer before_r(ctx, doc);
    const auto before = before_r.render(0, 1.0F);
    const auto before_p2 = before_r.render(1, 1.0F);

    WatermarkOptions o;
    o.text = "CONFIDENTIAL";
    CHECK(watermark(ctx, doc, "1", o) == 1);
    doc.save(out.str(), SaveOptions{});

    Document again = Document::open(ctx, out.str());
    CHECK(TextPage(ctx, again, 0).text().find("CONFIDENTIAL") != std::string::npos);
    CHECK(TextPage(ctx, again, 1).text().find("CONFIDENTIAL") == std::string::npos);

    leht::Renderer after_r(ctx, again);
    const auto after = after_r.render(0, 1.0F);
    const Diff d = diff(*before, *after);
    CHECK(d.count > 2000);  // a large, visible mark
    // Centred: its ink's centre of mass is near the page centre.
    CHECK(std::abs(d.cx - before->width / 2.0) < before->width * 0.1);
    CHECK(std::abs(d.cy - before->height / 2.0) < before->height * 0.1);
    CHECK(diff(*before_p2, *after_r.render(1, 1.0F)).count == 0);

    // No font embedded: the file barely grows.
    CHECK(std::filesystem::file_size(out.str()) <
          std::filesystem::file_size(corpus("text_10p.pdf")) + 16 * 1024);
    CHECK(qpdf_check(out.str()));
    if (have_tool("pdftotext")) {
        // poppler breaks diagonal text into short runs; read it in content
        // order and ignore the whitespace it puts between them.
        const std::string raw = capture("pdftotext -raw -f 1 -l 1 '" + out.str() +
                                        "' - 2>/dev/null | tr -d ' \\n'");
        CHECK(raw.find("CONFIDENTIAL") != std::string::npos);
    }
}

/// A page whose content leaves the graphics state scaled and shifted, and
/// already has an XObject named LehtMark.
std::string hostile_state_pdf() {
    PdfWriter w;
    w.set(1, "<< /Type /Catalog /Pages 2 0 R >>");
    w.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    w.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] "
             "/Resources << /XObject << /LehtMark 5 0 R >> >> /Contents 4 0 R >>");
    w.set(4, PdfWriter::stream("", "0.2 0 0 0.2 400 20 cm 1 0 0 rg /LehtMark Do"));
    w.set(5, PdfWriter::stream("/Type /XObject /Subtype /Form /BBox [0 0 100 100]",
                               "0 0 100 100 re f"));
    return w.finish(1);
}

void watermark_is_immune_to_the_page_state_and_names() {
    const Context ctx;
    const TempPath in("watermark_state_in.pdf");
    const TempPath out("watermark_state_out.pdf");
    write_file(in.str(), hostile_state_pdf());

    Document doc = Document::open(ctx, in.str());
    WatermarkOptions o;
    o.text = "DRAFT";
    o.angle = 0;
    (void)watermark(ctx, doc, "", o);
    doc.save(out.str(), SaveOptions{});

    Document again = Document::open(ctx, out.str());
    const auto hits = TextPage(ctx, again, 0).search("DRAFT");
    CHECK(hits.size() == 1);
    const auto& q = hits.front().quads.front();
    // Centred on the 600x800 page despite the cm the content left behind.
    CHECK(std::abs((q.min_x() + q.max_x()) / 2 - 300) < 5);
    CHECK(std::abs((q.min_y() + q.max_y()) / 2 - 400) < 20);
    CHECK(q.max_x() - q.min_x() > 300);  // fitted: most of the width
    CHECK(qpdf_check(out.str()));
}

void watermark_follows_the_page_as_displayed() {
    const Context ctx;
    const TempPath rotated("watermark_rotated.pdf");
    (void)leht::ops::rotate(ctx, corpus("text_10p.pdf"), rotated.str(), "1", 90);
    Document doc = Document::open(ctx, rotated.str());
    WatermarkOptions o;
    o.text = "LEVEL";
    o.angle = 0;
    o.font_size = 40;
    (void)watermark(ctx, doc, "1", o);
    const auto size = size_of(ctx, doc, 0);
    const auto hits = TextPage(ctx, doc, 0).search("LEVEL");
    CHECK(hits.size() == 1);
    const auto& q = hits.front().quads.front();
    // Horizontal on screen, and centred on the landscape page.
    CHECK(q.max_x() - q.min_x() > 2 * (q.max_y() - q.min_y()));
    CHECK(std::abs((q.min_x() + q.max_x()) / 2 - static_cast<float>(size.width) / 2) < 5);
    CHECK(std::abs((q.min_y() + q.max_y()) / 2 - static_cast<float>(size.height) / 2) < 30);
}

void watermark_refuses_bad_options() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    WatermarkOptions o;
    CHECK(throws([&] { (void)watermark(ctx, doc, "", o); }));  // empty
    o.text = "OK";
    o.opacity = 0;
    CHECK(throws([&] { (void)watermark(ctx, doc, "", o); }));
    o.opacity = 0.5F;
    o.angle = NAN;
    CHECK(throws([&] { (void)watermark(ctx, doc, "", o); }));
    o.angle = 30;
    o.color[1] = 2;
    CHECK(throws([&] { (void)watermark(ctx, doc, "", o); }));
    o.color[1] = 0;
    o.text = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";  // Cyrillic
    CHECK(throws([&] { (void)watermark(ctx, doc, "", o); }));
    o.text = "Caf\xC3\xA9 \xE2\x82\xAC";  // Latin-1 and the euro sign are fine
    CHECK(watermark(ctx, doc, "1", o) == 1);
}

}  // namespace

int main() {
    RUN(crop_sets_the_visible_area);
    RUN(crop_hides_but_does_not_remove);
    RUN(crop_margins_trims_each_page);
    RUN(crop_follows_the_page_as_displayed);
    RUN(crop_refuses_nonsense);
    RUN(watermark_marks_the_chosen_pages);
    RUN(watermark_is_immune_to_the_page_state_and_names);
    RUN(watermark_follows_the_page_as_displayed);
    RUN(watermark_refuses_bad_options);
    return 0;
}
