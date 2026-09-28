// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/organize.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <functional>
#include <string>

using leht::Context;
using leht::Document;
using leht::SaveOptions;
using leht::TextPage;
using leht::test::corpus;
using leht::test::qpdf_check;
using leht::test::TempPath;

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

/// Which page of text_10p.pdf this is (1-based), from its text; 0 if none.
int origin(const Context& ctx, Document& doc, int index) {
    const std::string text = TextPage(ctx, doc, index).text();
    for (int n = 10; n >= 1; --n) {  // 10 before 1, which is a prefix of it
        if (text.find("page " + std::to_string(n) + " -") != std::string::npos) {
            return n;
        }
    }
    return 0;
}

std::vector<int> origins(const Context& ctx, Document& doc) {
    std::vector<int> out;
    for (int i = 0; i < doc.page_count(); ++i) {
        out.push_back(origin(ctx, doc, i));
    }
    return out;
}

/// Saves and reopens, so what is checked is what a reader of the file sees.
Document round_trip(const Context& ctx, Document& doc, const TempPath& path) {
    SaveOptions full;
    full.mode = SaveOptions::Mode::Full;
    doc.save(path.str(), full);
    CHECK(qpdf_check(path.str()));
    return Document::open(ctx, path.str());
}

void rotate_turns_the_chosen_pages() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    leht::Renderer before(ctx, doc);
    const leht::PageSize upright = before.page_size(1, 1.0F);
    CHECK(leht::ops::rotate_pages(ctx, doc, "2,4", 90) == 2);
    CHECK(leht::ops::rotate_pages(ctx, doc, "4", -270) == 1);  // 90 - 270 = -180 = 180
    TempPath out("organize_rotate.pdf");
    Document back = round_trip(ctx, doc, out);
    leht::Renderer r(ctx, back);
    const leht::PageSize turned = r.page_size(1, 1.0F);
    CHECK(turned.width == upright.height && turned.height == upright.width);
    const leht::PageSize flipped = r.page_size(3, 1.0F);
    CHECK(flipped.width == upright.width && flipped.height == upright.height);
    CHECK(r.page_size(0, 1.0F).width == upright.width);  // untouched
    CHECK(throws([&] { (void)leht::ops::rotate_pages(ctx, doc, "1", 45); }));
}

void delete_removes_pages_but_never_all() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    CHECK(leht::ops::delete_pages(ctx, doc, "2,4-5") == 3);
    CHECK((origins(ctx, doc) == std::vector<int>{1, 3, 6, 7, 8, 9, 10}));
    CHECK(throws([&] { (void)leht::ops::delete_pages(ctx, doc, ""); }));
    CHECK(doc.page_count() == 7);  // the refusal changed nothing
    TempPath out("organize_delete.pdf");
    Document back = round_trip(ctx, doc, out);
    CHECK((origins(ctx, back) == std::vector<int>{1, 3, 6, 7, 8, 9, 10}));
}

void move_puts_pages_at_the_drop_position() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    // The last two to the front, as a drag onto the gap before page 1 would.
    CHECK(leht::ops::move_pages(ctx, doc, "9-10", 0) == 2);
    CHECK((origins(ctx, doc) == std::vector<int>{9, 10, 1, 2, 3, 4, 5, 6, 7, 8}));
    // The (new) first page to the end.
    (void)leht::ops::move_pages(ctx, doc, "1", doc.page_count());
    CHECK((origins(ctx, doc) == std::vector<int>{10, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
    // Pages 2 and 5 (origins 1 and 4) before the page now at 7 (origin 7).
    (void)leht::ops::move_pages(ctx, doc, "2,5", 7);
    CHECK((origins(ctx, doc) == std::vector<int>{10, 2, 3, 5, 6, 1, 4, 7, 8, 9}));
    // Dropped where they already are: nothing changes.
    (void)leht::ops::move_pages(ctx, doc, "3", 2);
    CHECK((origins(ctx, doc) == std::vector<int>{10, 2, 3, 5, 6, 1, 4, 7, 8, 9}));
    CHECK(throws([&] { (void)leht::ops::move_pages(ctx, doc, "1", 11); }));
    TempPath out("organize_move.pdf");
    Document back = round_trip(ctx, doc, out);
    CHECK((origins(ctx, back) == std::vector<int>{10, 2, 3, 5, 6, 1, 4, 7, 8, 9}));
}

void insert_copies_pages_from_another_document() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    Document other = Document::open(ctx, corpus("text_10p.pdf"));
    // Pages 3 then 1 of the other file, in that order, before page 2.
    CHECK(leht::ops::insert_pages(ctx, doc, 1, other, "3,1") == 2);
    CHECK((origins(ctx, doc) == std::vector<int>{1, 3, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
    Document outlined = Document::open(ctx, corpus("outlined.pdf"));
    CHECK(leht::ops::insert_pages(ctx, doc, doc.page_count(), outlined, "") == outlined.page_count());
    CHECK(doc.page_count() == 12 + outlined.page_count());
    CHECK(throws([&] { (void)leht::ops::insert_pages(ctx, doc, -1, other, "1"); }));
    CHECK(throws([&] { (void)leht::ops::insert_pages(ctx, doc, 0, other, "11"); }));
    TempPath out("organize_insert.pdf");
    Document back = round_trip(ctx, doc, out);
    CHECK(back.page_count() == 12 + outlined.page_count());
    CHECK(origin(ctx, back, 1) == 3);
}

void blank_pages_have_the_size_asked() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    leht::ops::insert_blank_page(ctx, doc, 0, 200, 300);
    leht::ops::insert_blank_page(ctx, doc, doc.page_count(), 595, 842);
    CHECK(doc.page_count() == 12);
    TempPath out("organize_blank.pdf");
    Document back = round_trip(ctx, doc, out);
    leht::Renderer r(ctx, back);
    CHECK(r.page_size(0, 1.0F).width == 200 && r.page_size(0, 1.0F).height == 300);
    CHECK(TextPage(ctx, back, 0).text().find_first_not_of(" \n") == std::string::npos);
    CHECK(origin(ctx, back, 1) == 1);
    CHECK(throws([&] { leht::ops::insert_blank_page(ctx, doc, 0, 0, 300); }));
    CHECK(throws([&] { leht::ops::insert_blank_page(ctx, doc, 99, 200, 300); }));
}

/// An outline entry must not point at a page that is gone, nor at the wrong
/// page after a move.
void the_outline_follows_the_pages() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("outlined.pdf"));
    const std::vector<leht::OutlineItem> before = doc.outline();
    CHECK(before.size() == 3 && before[2].page == 2);
    (void)leht::ops::move_pages(ctx, doc, "3", 0);  // Chapter Three's page first
    const std::vector<leht::OutlineItem> moved = doc.outline();
    CHECK(moved.size() == 3 && moved[2].page == 0 && moved[0].page == 1);
    (void)leht::ops::delete_pages(ctx, doc, "1");  // and gone
    for (const leht::OutlineItem& item : doc.outline()) {
        CHECK(item.page < doc.page_count());
    }
}

void non_pdf_is_refused() {
    Context ctx;
    Document image = Document::open(ctx, corpus("page.png"));
    CHECK(throws([&] { (void)leht::ops::rotate_pages(ctx, image, "", 90); }));
}

}  // namespace

int main() {
    RUN(rotate_turns_the_chosen_pages);
    RUN(delete_removes_pages_but_never_all);
    RUN(move_puts_pages_at_the_drop_position);
    RUN(insert_copies_pages_from_another_document);
    RUN(blank_pages_have_the_size_asked);
    RUN(the_outline_follows_the_pages);
    RUN(non_pdf_is_refused);
    return 0;
}
