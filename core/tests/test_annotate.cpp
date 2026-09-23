// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/pages.hpp"
#include "leht/ops/sign.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::Point;
using leht::Rect;
using leht::SaveOptions;
using leht::TextPage;
using leht::ops::add_annotation;
using leht::ops::AnnotId;
using leht::ops::AnnotInfo;
using leht::ops::AnnotKind;
using leht::ops::AnnotSpec;
using leht::ops::delete_annotation;
using leht::ops::list_annotations;
using leht::ops::mark_text;
using leht::ops::move_annotation;
using leht::ops::set_annotation_contents;
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

AnnotSpec spec_for(AnnotKind kind) {
    AnnotSpec s;
    s.kind = kind;
    s.author = "Tester";
    s.contents = "comment";
    s.rect = Rect{100, 300, 250, 360};
    s.strokes = {{{100, 400}, {150, 420}, {200, 400}}, {{100, 450}, {200, 450}}};
    return s;
}

const AnnotInfo* find(const std::vector<AnnotInfo>& all, AnnotId id) {
    const auto it = std::find_if(all.begin(), all.end(),
                                 [&](const AnnotInfo& a) { return a.id == id; });
    return it == all.end() ? nullptr : &*it;
}

void every_kind_round_trips() {
    const Context ctx;
    const TempPath out("annotate_kinds.pdf");
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    const auto quads = TextPage(ctx, doc, 0).search("quick brown").front().quads;

    struct Want {
        AnnotKind kind;
        const char* type;
    };
    const Want kinds[] = {
        {AnnotKind::Highlight, "Highlight"}, {AnnotKind::Underline, "Underline"},
        {AnnotKind::StrikeOut, "StrikeOut"}, {AnnotKind::Squiggly, "Squiggly"},
        {AnnotKind::Note, "Text"},           {AnnotKind::FreeText, "FreeText"},
        {AnnotKind::Ink, "Ink"},             {AnnotKind::Square, "Square"},
        {AnnotKind::Circle, "Circle"},       {AnnotKind::Stamp, "Stamp"},
    };
    std::vector<AnnotId> ids;
    for (const Want& w : kinds) {
        AnnotSpec s = spec_for(w.kind);
        s.quads = quads;
        ids.push_back(add_annotation(ctx, doc, 0, s));
    }
    // Ids are distinct, and stable across a save of the open document.
    std::vector<AnnotId> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());
    doc.save(out.str(), SaveOptions{});
    const auto live = list_annotations(ctx, doc);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const AnnotInfo* a = find(live, ids[i]);
        CHECK(a != nullptr);
        CHECK(a->type == kinds[i].type);
    }

    // And everything survives into the file.
    Document again = Document::open(ctx, out.str());
    const auto saved = list_annotations(ctx, again);
    CHECK(saved.size() == std::size(kinds));
    for (std::size_t i = 0; i < saved.size(); ++i) {
        CHECK(saved[i].page == 0);
        CHECK(saved[i].type == kinds[i].type);
        CHECK(saved[i].contents == "comment");
        CHECK(saved[i].author == "Tester");
        CHECK(!saved[i].rect.empty());
    }
    CHECK(qpdf_check(out.str()));

    // An independent parser sees the same annotations.
    if (have_tool("qpdf")) {
        const std::string qdf =
            capture("qpdf --qdf --object-streams=disable '" + out.str() + "' - 2>/dev/null");
        for (const Want& w : kinds) {
            CHECK(qdf.find(std::string("/Subtype /") + w.type) != std::string::npos);
        }
    }
}

void a_highlight_is_drawn() {
    // Every annotation gets an appearance stream, so it renders -- here and
    // in readers that would not synthesise one themselves.
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    const auto hit = TextPage(ctx, doc, 0).search("quick brown").front();
    AnnotSpec s;
    s.quads = hit.quads;
    s.color[0] = 1;
    s.color[1] = 1;
    s.color[2] = 0;  // pure yellow: red and green full, blue gone
    (void)add_annotation(ctx, doc, 0, s);

    leht::Renderer r(ctx, doc);
    const auto bmp = r.render(0, 1.0F);
    const auto& q = hit.quads.front();
    const int x = static_cast<int>((q.min_x() + q.max_x()) / 2);
    const int y = static_cast<int>((q.min_y() + q.max_y()) / 2);
    int yellow = 0;
    for (int dx = -20; dx <= 20; ++dx) {
        const auto at = static_cast<std::size_t>(y * bmp->stride + (x + dx) * bmp->channels);
        if (bmp->pixels[at] > 200 && bmp->pixels[at + 1] > 200 && bmp->pixels[at + 2] < 80) {
            ++yellow;
        }
    }
    CHECK(yellow > 10);
}

void mark_text_marks_every_hit() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    AnnotSpec base;
    base.kind = AnnotKind::Underline;
    const auto ids = mark_text(ctx, doc, "line 1 ", "2-3", base);
    // "line 1 " occurs once per page ("line 1 - the ...").
    CHECK(ids.size() == 2);
    const auto all = list_annotations(ctx, doc);
    CHECK(all.size() == 2);
    CHECK(all[0].page == 1 && all[1].page == 2);
    CHECK(all[0].type == "Underline");
    AnnotSpec note;
    note.kind = AnnotKind::Note;
    CHECK(throws([&] { (void)mark_text(ctx, doc, "x", "", note); }));
    CHECK(throws([&] { (void)mark_text(ctx, doc, "", "", base); }));
}

void delete_removes_one() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    const AnnotId a = add_annotation(ctx, doc, 0, spec_for(AnnotKind::Note));
    const AnnotId b = add_annotation(ctx, doc, 3, spec_for(AnnotKind::Square));
    CHECK(list_annotations(ctx, doc).size() == 2);
    CHECK(delete_annotation(ctx, doc, b));
    const auto left = list_annotations(ctx, doc);
    CHECK(left.size() == 1 && left.front().id == a);
    CHECK(!delete_annotation(ctx, doc, b));  // already gone
    CHECK(!delete_annotation(ctx, doc, 0));
    CHECK(!delete_annotation(ctx, doc, 999999));
}

void form_fields_are_not_annotations_here() {
    PdfWriter w;
    w.set(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R] >> >>");
    w.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    w.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
             "/Annots [5 0 R 6 0 R] >>");
    w.set(4, PdfWriter::stream("", ""));
    w.set(5, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /Rect [72 700 272 720] >>");
    w.set(6, "<< /Type /Annot /Subtype /Text /Rect [300 700 320 720] /Contents (hi) >>");
    const Context ctx;
    const TempPath in("annotate_widgets.pdf");
    write_file(in.str(), w.finish(1));
    Document doc = Document::open(ctx, in.str());
    const auto all = list_annotations(ctx, doc);
    CHECK(all.size() == 1);
    CHECK(all.front().type == "Text" && all.front().contents == "hi");
    CHECK(!delete_annotation(ctx, doc, 5));  // the widget: forms own it
}

void bad_specs_are_refused() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    AnnotSpec s;  // highlight with no quads
    CHECK(throws([&] { (void)add_annotation(ctx, doc, 0, s); }));
    s = spec_for(AnnotKind::Square);
    s.rect = Rect{10, 10, 5, 5};
    CHECK(throws([&] { (void)add_annotation(ctx, doc, 0, s); }));
    s = spec_for(AnnotKind::Ink);
    s.strokes = {{}};
    CHECK(throws([&] { (void)add_annotation(ctx, doc, 0, s); }));
    s = spec_for(AnnotKind::Stamp);
    s.stamp = "Bogus";
    CHECK(throws([&] { (void)add_annotation(ctx, doc, 0, s); }));
    s = spec_for(AnnotKind::Note);
    s.opacity = 0;
    CHECK(throws([&] { (void)add_annotation(ctx, doc, 0, s); }));
    s.opacity = 1;
    s.color[2] = -0.5F;
    CHECK(throws([&] { (void)add_annotation(ctx, doc, 0, s); }));
    s.color[2] = 0;
    CHECK(throws([&] { (void)add_annotation(ctx, doc, 10, s); }));
    CHECK(throws([&] { (void)add_annotation(ctx, doc, -1, s); }));
    Document png = Document::open(ctx, corpus("page.png"));
    CHECK(throws([&] { (void)add_annotation(ctx, png, 0, s); }));
    CHECK(list_annotations(ctx, doc).empty());  // nothing half-added
}

}  // namespace

/// The listing's entry for `id`, by value (the listing is a temporary).
AnnotInfo info_of(const Context& ctx, Document& doc, AnnotId id) {
    const auto all = list_annotations(ctx, doc);
    const AnnotInfo* a = find(all, id);
    CHECK(a != nullptr);
    return a != nullptr ? *a : AnnotInfo{};
}

/// Pixels in `r` (base coordinates, zoom 1) of page 0 that satisfy `pred`.
template <typename Pred>
int pixels_in(const Context& ctx, Document& doc, const Rect& r, Pred pred) {
    leht::Renderer renderer(ctx, doc);
    const auto bmp = renderer.render(0, 1.0F);
    CHECK(bmp.has_value());
    int n = 0;
    for (int y = std::max(0, static_cast<int>(r.y0));
         y < static_cast<int>(r.y1) && y < bmp->height; ++y) {
        for (int x = std::max(0, static_cast<int>(r.x0));
             x < static_cast<int>(r.x1) && x < bmp->width; ++x) {
            const std::uint8_t* p = &bmp->pixels[static_cast<std::size_t>(
                y * bmp->stride + x * bmp->channels)];
            if (pred(p[0], p[1], p[2])) {
                ++n;
            }
        }
    }
    return n;
}

bool reddish(int r, int g, int b) { return r > 180 && g < 90 && b < 90; }
bool inky(int r, int g, int b) { return r < 160 || g < 160 || b < 160; }

bool near(const Rect& a, const Rect& b, float tolerance = 0.6F) {
    return std::abs(a.x0 - b.x0) <= tolerance && std::abs(a.y0 - b.y0) <= tolerance &&
           std::abs(a.x1 - b.x1) <= tolerance && std::abs(a.y1 - b.y1) <= tolerance;
}

void ink_moves_and_scales_with_its_strokes() {
    const Context ctx;
    const TempPath out("annotate_move_ink.pdf");
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    AnnotSpec s = spec_for(AnnotKind::Ink);
    s.color[0] = 1;
    s.color[1] = 0;
    s.color[2] = 0;
    s.line_width = 3;
    const AnnotId id = add_annotation(ctx, doc, 0, s);
    const Rect before = info_of(ctx, doc, id).rect;
    CHECK(pixels_in(ctx, doc, before, reddish) > 50);

    // Twice the size, up and to the right, clear of where it was.
    const float w = before.x1 - before.x0;
    const float h = before.y1 - before.y0;
    const Rect to{300, 80, 300 + 2 * w, 80 + 2 * h};
    CHECK(move_annotation(ctx, doc, id, to));
    CHECK(near(info_of(ctx, doc, id).rect, to));
    CHECK(pixels_in(ctx, doc, before, reddish) == 0);
    CHECK(pixels_in(ctx, doc, to, reddish) > 100);

    // The strokes themselves moved, so a reader that redraws from /InkList
    // (instead of using the appearance) draws it in the same place.
    doc.save(out.str(), SaveOptions{});
    CHECK(qpdf_check(out.str()));
    if (have_tool("qpdf")) {
        const std::string qdf =
            capture("qpdf --qdf --object-streams=disable '" + out.str() + "' - 2>/dev/null");
        const std::size_t at = qdf.find("/InkList");
        CHECK(at != std::string::npos);
        // The first point's x, in PDF space (same as base x on this page).
        const std::size_t open = qdf.find_first_of("0123456789", qdf.find('[', at));
        const float x = std::stof(qdf.substr(open));
        CHECK(x >= to.x0 && x <= to.x1);
    }
}

void a_picture_stamp_keeps_its_picture_when_resized() {
    // The signature picture's appearance is custom: regenerating it would
    // replace the drawing with nothing (or MuPDF's red "Draft"). A resize
    // scales the appearance as it is.
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    leht::ops::Appearance a;
    a.strokes = {{{0, 30}, {30, 5}, {60, 35}, {90, 5}}};
    a.strokes_width = 100;
    a.strokes_height = 40;
    a.stroke_width = 4;
    const Rect first{350, 620, 500, 680};
    const int id = leht::ops::add_signature_stamp(ctx, doc, 0, first, a);
    const Rect before = info_of(ctx, doc, id).rect;
    CHECK(pixels_in(ctx, doc, before, inky) > 50);

    const Rect to{60, 60, 60 + 2 * (before.x1 - before.x0), 60 + 1.5F * (before.y1 - before.y0)};
    CHECK(move_annotation(ctx, doc, id, to));
    CHECK(near(info_of(ctx, doc, id).rect, to));
    CHECK(pixels_in(ctx, doc, to, inky) > 100);
    CHECK(pixels_in(ctx, doc, to, reddish) == 0);
    CHECK(info_of(ctx, doc, id).contents ==
          "A picture, not a digital signature.");
}

void free_text_reflows_and_takes_new_words() {
    const Context ctx;
    const TempPath out("annotate_freetext.pdf");
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    AnnotSpec s = spec_for(AnnotKind::FreeText);
    s.contents = "Leht";
    s.font_size = 18;
    s.color[0] = 1;
    s.color[1] = 0;
    s.color[2] = 0;
    s.rect = Rect{100, 60, 220, 100};
    const AnnotId id = add_annotation(ctx, doc, 0, s);
    const AnnotInfo info = info_of(ctx, doc, id);
    CHECK(info.movable && info.resizable);
    CHECK(info.font_size == 18);
    CHECK(info.color[0] == 1 && info.color[1] == 0 && info.color[2] == 0);
    // Inside the box only: its red border would otherwise outweigh the words.
    const auto inside = [](const Rect& r) { return Rect{r.x0 + 3, r.y0 + 3, r.x1 - 3, r.y1 - 3}; };
    const int red_before = pixels_in(ctx, doc, inside(info.rect), reddish);
    CHECK(red_before > 20);

    // Resized, the text is drawn again at its size, not stretched: about as
    // much red ink as before.
    const Rect to{300, 60, 540, 140};
    CHECK(move_annotation(ctx, doc, id, to));
    const int red_after = pixels_in(ctx, doc, inside(to), reddish);
    CHECK(red_after > red_before / 2 && red_after < red_before * 2);

    // New words: more text, more ink; and the listing says what it now reads.
    CHECK(set_annotation_contents(ctx, doc, id, "Leht, the page, in Estonian"));
    CHECK(info_of(ctx, doc, id).contents == "Leht, the page, in Estonian");
    CHECK(pixels_in(ctx, doc, inside(to), reddish) > red_after * 2);
    doc.save(out.str(), SaveOptions{});
    CHECK(qpdf_check(out.str()));
}

void a_move_on_a_rotated_page_lands_where_asked() {
    // Base coordinates are the page as displayed; the dictionary is in the
    // unrotated PDF space. The move must go through the page's transform.
    const Context ctx;
    const TempPath rotated("annotate_rotated.pdf");
    (void)leht::ops::rotate(ctx, corpus("text_10p.pdf"), rotated.str(), "1", 90);
    Document doc = Document::open(ctx, rotated.str());
    const AnnotId id = add_annotation(ctx, doc, 0, spec_for(AnnotKind::Square));
    const Rect to{50, 40, 130, 90};
    CHECK(move_annotation(ctx, doc, id, to));
    CHECK(near(info_of(ctx, doc, id).rect, to, 1.0F));
}

void what_cannot_move_says_so() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    AnnotSpec hl;
    hl.quads = TextPage(ctx, doc, 0).search("quick brown").front().quads;
    const AnnotId highlight = add_annotation(ctx, doc, 0, hl);
    CHECK(!info_of(ctx, doc, highlight).movable);
    CHECK(throws([&] { move_annotation(ctx, doc, highlight, Rect{10, 10, 60, 30}); }));

    // A note moves, but its icon keeps its size.
    const AnnotId note = add_annotation(ctx, doc, 0, spec_for(AnnotKind::Note));
    const AnnotInfo n = info_of(ctx, doc, note);
    CHECK(n.movable && !n.resizable);
    const Rect r = n.rect;
    CHECK(move_annotation(ctx, doc, note, Rect{r.x0 + 50, r.y0 + 50, r.x1 + 50, r.y1 + 50}));
    CHECK(throws([&] { move_annotation(ctx, doc, note, Rect{0, 0, 100, 100}); }));
    CHECK(set_annotation_contents(ctx, doc, note, "rewritten"));
    CHECK(info_of(ctx, doc, note).contents == "rewritten");

    const AnnotId ink = add_annotation(ctx, doc, 0, spec_for(AnnotKind::Ink));
    CHECK(throws([&] { set_annotation_contents(ctx, doc, ink, "no"); }));
    CHECK(throws([&] { move_annotation(ctx, doc, ink, Rect{10, 10, 10, 30}); }));
    CHECK(throws([&] {
        move_annotation(ctx, doc, ink, Rect{0, 0, std::numeric_limits<float>::infinity(), 5});
    }));
    CHECK(!move_annotation(ctx, doc, 999999, Rect{10, 10, 60, 30}));
    CHECK(!set_annotation_contents(ctx, doc, 999999, "x"));
}

int main() {
    RUN(every_kind_round_trips);
    RUN(a_highlight_is_drawn);
    RUN(mark_text_marks_every_hit);
    RUN(delete_removes_one);
    RUN(form_fields_are_not_annotations_here);
    RUN(bad_specs_are_refused);
    RUN(ink_moves_and_scales_with_its_strokes);
    RUN(a_picture_stamp_keeps_its_picture_when_resized);
    RUN(free_text_reflows_and_takes_new_words);
    RUN(a_move_on_a_rotated_page_lands_where_asked);
    RUN(what_cannot_move_says_so);
    return 0;
}
