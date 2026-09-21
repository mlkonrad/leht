// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::OutlineItem;

namespace {

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

// outlined.pdf: Chapter One -> p1, Chapter Two -> p2 (child Section 2.1 -> p3),
// Chapter Three -> p3. Pages are 0-based in the API.
void reads_the_outline_tree() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("outlined.pdf"));
    const std::vector<OutlineItem> outline = doc.outline();

    CHECK(outline.size() == 3);
    CHECK(outline[0].title == "Chapter One");
    CHECK(outline[0].page == 0);
    CHECK(outline[1].title == "Chapter Two");
    CHECK(outline[1].page == 1);
    CHECK(outline[2].title == "Chapter Three");
    CHECK(outline[2].page == 2);
}

void nested_children_are_present() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("outlined.pdf"));
    const std::vector<OutlineItem> outline = doc.outline();

    CHECK(outline[0].children.empty());
    CHECK(outline[1].children.size() == 1);
    CHECK(outline[1].children[0].title == "Section 2.1");
    CHECK(outline[1].children[0].page == 2);
    // The child points partway down the page (Dest /XYZ 0 400), so y is set.
    CHECK(outline[1].children[0].y > 0.0F);
}

void document_without_outline_returns_empty() {
    Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    CHECK(doc.outline().empty());
}

}  // namespace

int main() {
    RUN(reads_the_outline_tree);
    RUN(nested_children_are_present);
    RUN(document_without_outline_returns_empty);
    return 0;
}
