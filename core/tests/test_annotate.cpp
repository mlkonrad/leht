// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <algorithm>
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

int main() {
    RUN(every_kind_round_trips);
    RUN(a_highlight_is_drawn);
    RUN(mark_text_marks_every_hit);
    RUN(delete_removes_one);
    RUN(form_fields_are_not_annotations_here);
    RUN(bad_specs_are_refused);
    return 0;
}
