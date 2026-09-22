// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Redaction is only worth anything if the removed text is gone from EVERY
// byte of the output, not just from what a viewer draws. So these tests hide
// one secret in each place a PDF can repeat text -- page content, an earlier
// revision, an annotation, a reply to it, a form field, the structure tree,
// marked-content /ActualText, a page thumbnail -- then check the result three
// independent ways: our own text search, poppler's pdftotext, and a raw byte
// scan of the file (uncompressed, and again after qpdf decompresses it).
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/merge.hpp"
#include "leht/ops/redact.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::Rect;
using leht::SaveOptions;
using leht::TextPage;
using leht::ops::redact;
using leht::ops::redact_text;
using leht::ops::RedactResult;
using leht::test::capture;
using leht::test::corpus;
using leht::test::have_tool;
using leht::test::PdfWriter;
using leht::test::qpdf_check;
using leht::test::read_file;
using leht::test::TempPath;
using leht::test::write_file;

namespace {

const std::string kSecret = "SECRET42";

/// One page, one line of text containing the secret, and the secret repeated
/// in every other place a PDF can hold it. The page content's first revision
/// mentions it too; an incremental update then replaces that content, so the
/// old stream survives only as an earlier revision.
std::string leaky_pdf(const std::string& extra_catalog = "", const std::string& info = "") {
    PdfWriter w;
    w.set(1, "<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 9 0 R "
             "/MarkInfo << /Marked true >> /AcroForm << /Fields [8 0 R] >> " +
                 extra_catalog + " >>");
    w.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    w.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R "
             "/Annots [6 0 R 8 0 R 12 0 R] /Thumb 10 0 R /StructParents 0 >>");
    w.set(4, PdfWriter::stream("", "BT /F1 12 Tf 72 700 Td (Old draft: SECRET42) Tj ET"));
    w.set(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    // A note right on the secret's line: overlaps the redaction.
    w.set(6, "<< /Type /Annot /Subtype /Text /Rect [150 640 170 660] "
             "/Contents (note about SECRET42) /P 3 0 R >>");
    // A form field whose widget sits on the secret.
    w.set(8, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (code) /V (SECRET42) "
             "/Rect [135 646 178 656] /DA (/Helv 0 Tf 0 g) /P 3 0 R >>");
    w.set(9, "<< /Type /StructTreeRoot /K 11 0 R >>");
    // The thumbnail's pixels spell the secret: 8 gray pixels, one per byte.
    w.set(10, PdfWriter::stream("/Width 8 /Height 1 /ColorSpace /DeviceGray "
                                "/BitsPerComponent 8",
                                kSecret));
    w.set(11, "<< /Type /StructElem /S /P /P 9 0 R /Pg 3 0 R /K 0 "
              "/Alt (SECRET42 alt) /ActualText (SECRET42) >>");
    // A reply to the note, far from the secret: must go with its parent.
    w.set(12, "<< /Type /Annot /Subtype /Text /Rect [500 700 520 720] "
              "/Contents (reply: SECRET42) /IRT 6 0 R /P 3 0 R >>");
    std::string trailer;
    if (!info.empty()) {
        w.set(13, info);
        trailer = "/Info 13 0 R";
    }
    const std::string base = w.finish(1, trailer);

    const std::string current =
        "BT /F1 12 Tf 72 700 Td (Public heading) Tj ET\n"
        "BT /F1 12 Tf 72 650 Td (The code is SECRET42 ok) Tj ET\n"
        // Glyphs that do not spell the secret, whose /ActualText does: how a
        // PDF carries the real text behind a ligature, logo or drawn lettering.
        "/Span << /ActualText (SECRET42) >> BDC\n"
        "BT /F1 12 Tf 72 620 Td (xxxxxxxx) Tj ET\n"
        "EMC\n"
        "BT /F1 12 Tf 72 600 Td (Public footer) Tj ET";
    return w.update(base, {{4, PdfWriter::stream("", current)}}, 1, trailer);
}

std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool bytes_contain_secret(const std::string& bytes) {
    return upper(bytes).find(kSecret) != std::string::npos;
}

/// Our own text search, across every page.
int own_hits(const Context& ctx, Document& doc, const std::string& needle) {
    int hits = 0;
    for (int i = 0; i < doc.page_count(); ++i) {
        hits += static_cast<int>(TextPage(ctx, doc, i).search(needle).size());
    }
    return hits;
}

void the_fixture_really_leaks() {
    // Guard against a test that passes because the fixture is broken: before
    // redaction, the secret must be findable on the page.
    const Context ctx;
    const TempPath in("redact_fixture_check.pdf");
    write_file(in.str(), leaky_pdf());
    Document doc = Document::open(ctx, in.str());
    CHECK(doc.page_count() == 1);
    CHECK(own_hits(ctx, doc, kSecret) >= 1);
    CHECK(bytes_contain_secret(read_file(in.str())));
}

void redact_text_removes_the_secret_from_every_byte() {
    const Context ctx;
    const TempPath in("redact_leaky_in.pdf");
    const TempPath out("redact_leaky_out.pdf");
    write_file(in.str(), leaky_pdf());

    Document doc = Document::open(ctx, in.str());
    const RedactResult r = redact_text(ctx, doc, kSecret);
    CHECK(r.pages == std::vector<int>({0}));
    CHECK(r.areas >= 1);
    CHECK(r.annotations_removed >= 3);  // the note, its reply, the widget
    CHECK(r.structure_dropped);
    for (const std::string& where : r.remaining) {
        std::fprintf(stderr, "  remaining: %s\n", where.c_str());
    }
    CHECK(r.remaining.empty());
    CHECK(doc.redacted());

    // Uncompressed, so a raw byte scan sees every string in the file. Asking
    // for no garbage collection is overridden, because the doc is redacted.
    SaveOptions plain;
    plain.compress_streams = false;
    plain.garbage = 0;
    doc.save(out.str(), plain);

    const std::string bytes = read_file(out.str());
    if (bytes_contain_secret(bytes)) {
        const std::size_t at = upper(bytes).find(kSecret);
        std::fprintf(stderr, "  secret at byte %zu: ...%s...\n", at,
                     bytes.substr(at > 80 ? at - 80 : 0, 160).c_str());
    }
    CHECK(!bytes_contain_secret(bytes));

    // 1. Our own extraction.
    Document again = Document::open(ctx, out.str());
    CHECK(own_hits(ctx, again, kSecret) == 0);
    // The rest of the page is still there: redaction is not deletion.
    const std::string text = TextPage(ctx, again, 0).text();
    CHECK(text.find("Public heading") != std::string::npos);
    CHECK(text.find("Public footer") != std::string::npos);
    CHECK(text.find("The code is") != std::string::npos);
    CHECK(text.find(" ok") != std::string::npos);

    // 2. An independent engine.
    if (have_tool("pdftotext")) {
        const std::string poppler = capture("pdftotext '" + out.str() + "' - 2>/dev/null");
        CHECK(poppler.find("Public heading") != std::string::npos);
        CHECK(!bytes_contain_secret(poppler));
    } else {
        std::fprintf(stderr, "  SKIP pdftotext cross-check (poppler-utils not installed)\n");
    }

    // 3. Every byte, after an independent tool has decompressed everything.
    if (have_tool("qpdf")) {
        const std::string qdf = capture("qpdf --qdf --object-streams=disable '" +
                                        out.str() + "' - 2>/dev/null");
        CHECK(!qdf.empty());
        CHECK(!bytes_contain_secret(qdf));
    }
    CHECK(qpdf_check(out.str()));
}

void compressed_saves_are_clean_too() {
    // The default save compresses; scan it after qpdf inflates every stream.
    if (!have_tool("qpdf")) {
        std::fprintf(stderr, "  SKIP (qpdf not installed)\n");
        return;
    }
    const Context ctx;
    const TempPath in("redact_leaky_in2.pdf");
    const TempPath out("redact_leaky_out2.pdf");
    write_file(in.str(), leaky_pdf());
    Document doc = Document::open(ctx, in.str());
    (void)redact_text(ctx, doc, kSecret);
    doc.save(out.str(), SaveOptions{});
    const std::string qdf = capture("qpdf --qdf --object-streams=disable '" + out.str() +
                                    "' - 2>/dev/null");
    CHECK(!qdf.empty());
    CHECK(!bytes_contain_secret(qdf));
}

void what_it_cannot_remove_it_reports() {
    const Context ctx;
    const TempPath in("redact_report.pdf");
    write_file(in.str(), leaky_pdf("", "<< /Title (Report) /Subject (About SECRET42) >>"));
    Document doc = Document::open(ctx, in.str());
    const RedactResult r = redact_text(ctx, doc, kSecret);
    bool subject = false;
    for (const std::string& where : r.remaining) {
        subject = subject || where == "document metadata (Subject)";
    }
    CHECK(subject);
}

void redact_by_area_removes_only_what_is_under_it() {
    const Context ctx;
    const TempPath out("redact_area.pdf");
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));

    // Find a phrase and redact exactly its box.
    const auto hits = TextPage(ctx, doc, 0).search("quick brown");
    CHECK(!hits.empty());
    const auto& q = hits.front().quads.front();
    const Rect box{q.min_x(), q.min_y(), q.max_x(), q.max_y()};

    leht::Renderer before_r(ctx, doc);
    const auto before = before_r.render(0, 1.0F);
    CHECK(before.has_value());

    const RedactResult r = redact(ctx, doc, 0, {box});
    CHECK(r.areas == 1);
    CHECK(r.pages == std::vector<int>({0}));
    doc.save(out.str(), SaveOptions{});

    Document again = Document::open(ctx, out.str());
    const auto after_hits = TextPage(ctx, again, 0).search("quick brown");
    // Every line on the page says "quick brown"; exactly one is gone.
    CHECK(after_hits.size() + 1 == hits.size());

    // Outside the box (with a pixel of antialiasing margin) nothing changed.
    leht::Renderer after_r(ctx, again);
    const auto after = after_r.render(0, 1.0F);
    CHECK(after.has_value());
    CHECK(after->width == before->width && after->height == before->height);
    int differing_outside = 0;
    int black_inside = 0;
    for (int y = 0; y < before->height; ++y) {
        for (int x = 0; x < before->width; ++x) {
            const auto at = static_cast<std::size_t>(y * before->stride + x * before->channels);
            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);
            const bool inside = fx >= box.x0 - 2 && fx <= box.x1 + 2 && fy >= box.y0 - 2 &&
                                fy <= box.y1 + 2;
            const bool same = std::equal(before->pixels.begin() + static_cast<long>(at),
                                         before->pixels.begin() + static_cast<long>(at) + 3,
                                         after->pixels.begin() + static_cast<long>(at));
            if (!inside && !same) {
                ++differing_outside;
            }
            if (inside && after->pixels[at] == 0) {
                ++black_inside;
            }
        }
    }
    CHECK(differing_outside == 0);
    CHECK(black_inside > 0);  // the black box is drawn
    CHECK(qpdf_check(out.str()));
}

void images_are_blacked_out_under_the_box() {
    const Context ctx;
    const TempPath img_pdf("redact_image_in.pdf");
    const TempPath out("redact_image_out.pdf");
    (void)leht::ops::merge(ctx, {corpus("scan.jpg")}, img_pdf.str());

    Document doc = Document::open(ctx, img_pdf.str());
    leht::Renderer before_r(ctx, doc);
    const auto before = before_r.render(0, 1.0F);
    CHECK(before.has_value());
    const auto w = static_cast<float>(before->width);
    const auto h = static_cast<float>(before->height);
    const Rect box{w * 0.25F, h * 0.25F, w * 0.5F, h * 0.5F};

    const RedactResult r = redact(ctx, doc, 0, {box});
    CHECK(r.areas == 1);
    doc.save(out.str(), SaveOptions{});

    Document again = Document::open(ctx, out.str());
    leht::Renderer after_r(ctx, again);
    const auto after = after_r.render(0, 1.0F);
    CHECK(after.has_value());

    // Inside (well clear of the edge): black. Outside: the same picture, up
    // to the image being re-encoded -- a small mean difference, not zero.
    double inside_sum = 0;
    double outside_diff = 0;
    long inside_n = 0;
    long outside_n = 0;
    for (int y = 0; y < after->height; ++y) {
        for (int x = 0; x < after->width; ++x) {
            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);
            const auto at = static_cast<std::size_t>(y * after->stride + x * after->channels);
            if (fx > box.x0 + 3 && fx < box.x1 - 3 && fy > box.y0 + 3 && fy < box.y1 - 3) {
                inside_sum += after->pixels[at];
                ++inside_n;
            } else if (fx < box.x0 - 3 || fx > box.x1 + 3 || fy < box.y0 - 3 || fy > box.y1 + 3) {
                outside_diff += std::abs(int{after->pixels[at]} - int{before->pixels[at]});
                ++outside_n;
            }
        }
    }
    CHECK(inside_n > 0 && outside_n > 0);
    const double inside_mean = inside_sum / static_cast<double>(inside_n);
    const double outside_mean = outside_diff / static_cast<double>(outside_n);
    std::printf("      inside mean %.1f, outside mean diff %.2f\n", inside_mean, outside_mean);
    CHECK(inside_mean < 8.0);
    CHECK(outside_mean < 3.0);
    CHECK(qpdf_check(out.str()));
}

void bad_input_is_refused() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    auto throws = [](auto&& fn) {
        try {
            fn();
            return false;
        } catch (const leht::Error&) {
            return true;
        }
    };
    CHECK(throws([&] { (void)redact(ctx, doc, 10, {{0, 0, 10, 10}}); }));
    CHECK(throws([&] { (void)redact(ctx, doc, -1, {{0, 0, 10, 10}}); }));
    CHECK(throws([&] { (void)redact(ctx, doc, 0, {{10, 10, 0, 0}}); }));
    CHECK(throws([&] { (void)redact_text(ctx, doc, ""); }));

    Document png = Document::open(ctx, corpus("page.png"));
    CHECK(throws([&] { (void)redact_text(ctx, png, "x"); }));
}

}  // namespace

int main() {
    RUN(the_fixture_really_leaks);
    RUN(redact_text_removes_the_secret_from_every_byte);
    RUN(compressed_saves_are_clean_too);
    RUN(what_it_cannot_remove_it_reports);
    RUN(redact_by_area_removes_only_what_is_under_it);
    RUN(images_are_blacked_out_under_the_box);
    RUN(bad_input_is_refused);
    return 0;
}
