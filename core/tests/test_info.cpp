// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/info.hpp"
#include "edit_harness.hpp"

#include <string>

using leht::Context;
using leht::Document;
using leht::SaveOptions;
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

void properties_are_set_saved_and_removed() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    leht::ops::set_info(ctx, doc, "Title", "Üürileping 2026");  // not in PDFDocEncoding's ASCII
    leht::ops::set_info(ctx, doc, "Author", "Mari Maasikas");
    leht::ops::set_info(ctx, doc, "Keywords", "leping, üür");
    CHECK(doc.metadata("info:Title").value_or("") == "Üürileping 2026");

    TempPath out("info_saved.pdf");
    SaveOptions full;
    full.mode = SaveOptions::Mode::Full;
    doc.save(out.str(), full);
    CHECK(qpdf_check(out.str()));
    Document back = Document::open(ctx, out.str());
    CHECK(back.metadata("info:Title").value_or("") == "Üürileping 2026");
    CHECK(back.metadata("info:Author").value_or("") == "Mari Maasikas");
    CHECK(back.metadata("info:Keywords").value_or("") == "leping, üür");

    leht::ops::set_info(ctx, back, "Author", "");
    CHECK(!back.metadata("info:Author").has_value() || back.metadata("info:Author")->empty());
    CHECK(back.metadata("info:Title").value_or("") == "Üürileping 2026");
}

void only_the_authors_fields_are_editable() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    CHECK(throws([&] { leht::ops::set_info(ctx, doc, "Producer", "me"); }));
    CHECK(throws([&] { leht::ops::set_info(ctx, doc, "CreationDate", "D:2020"); }));
    CHECK(throws([&] { leht::ops::set_info(ctx, doc, "Title", std::string(40000, 'x')); }));
    Document image = Document::open(ctx, corpus("page.png"));
    CHECK(throws([&] { leht::ops::set_info(ctx, image, "Title", "x"); }));
}

}  // namespace

int main() {
    RUN(properties_are_set_saved_and_removed);
    RUN(only_the_authors_fields_are_editable);
    return 0;
}
