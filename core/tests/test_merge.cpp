// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/merge.hpp"
#include "leht/renderer.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::Renderer;
using leht::ops::merge;
using leht::ops::MergeOptions;
using leht::ops::MergeResult;

namespace {

namespace fs = std::filesystem;

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

/// Self-cleaning scratch path so a failing test cannot leave litter behind.
class TempPdf {
public:
    explicit TempPdf(const char* name)
        : path_(fs::temp_directory_path() / ("leht_test_" + std::string(name))) {
        fs::remove(path_);
    }
    ~TempPdf() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempPdf(const TempPdf&) = delete;
    TempPdf& operator=(const TempPdf&) = delete;

    [[nodiscard]] std::string str() const { return path_.string(); }
    [[nodiscard]] std::uintmax_t size() const { return fs::file_size(path_); }

private:
    fs::path path_;
};

void merges_two_pdfs_and_sums_pages() {
    Context ctx;
    TempPdf out{"merge_two.pdf"};

    const MergeResult result =
        merge(ctx, {corpus("text_10p.pdf"), corpus("text_10p.pdf")}, out.str());

    CHECK(result.inputs_merged == 2);
    CHECK(result.pages_written == 20);
    CHECK(result.output_bytes > 0);

    Document doc = Document::open(ctx, out.str());
    CHECK(doc.page_count() == 20);
}

void merges_images_and_pdfs_together() {
    Context ctx;
    TempPdf out{"merge_mixed.pdf"};

    const MergeResult result = merge(ctx,
                                     {corpus("text_10p.pdf"), corpus("scan.jpg"),
                                      corpus("page.png")},
                                     out.str());

    CHECK(result.inputs_merged == 3);
    CHECK(result.pages_written == 12);  // 10 + 1 + 1

    Document doc = Document::open(ctx, out.str());
    CHECK(doc.page_count() == 12);

    // Every page must actually render, including the image pages.
    Renderer renderer{ctx, doc};
    for (int page = 0; page < 12; ++page) {
        const auto bmp = renderer.render(page, 0.5F);
        CHECK(bmp.has_value());
        CHECK(bmp->width > 0);
    }
}

/// The headline correctness property: a JPEG must be carried into the PDF as
/// its original compressed stream. If it were decoded and re-encoded, the
/// output would be far larger (or visibly degraded), which is exactly the
/// silent damage most merge tools inflict on scans.
void jpeg_is_embedded_losslessly() {
    Context ctx;
    TempPdf out{"merge_jpeg.pdf"};

    const std::uintmax_t source = fs::file_size(corpus("scan.jpg"));
    merge(ctx, {corpus("scan.jpg")}, out.str());
    const std::uintmax_t written = out.size();

    // Allow PDF structural overhead, but nothing like a re-encode would cost.
    CHECK(written >= source);
    CHECK(written < source + (source / 10) + 16384);

    Document doc = Document::open(ctx, out.str());
    CHECK(doc.page_count() == 1);
}

/// A 150 DPI image must produce a page sized in points, not one point per pixel.
void image_page_is_sized_by_resolution() {
    Context ctx;
    TempPdf out{"merge_size.pdf"};
    merge(ctx, {corpus("scan.jpg")}, out.str());

    Document doc = Document::open(ctx, out.str());
    Renderer renderer{ctx, doc};
    const leht::PageSize size = renderer.page_size(0, 1.0F);

    // A 150 DPI render of US Letter/A4 is ~1240x1754 px, which at 72 pt/inch
    // must come back as roughly 595x842 points, not 1240x1754.
    CHECK(size.width > 400 && size.width < 700);
    CHECK(size.height > 700 && size.height < 950);
}

void input_order_is_preserved() {
    Context ctx;
    TempPdf out{"merge_order.pdf"};

    // wide.png is 400x200 -- landscape, unlike any text page in the corpus --
    // so page geometry alone identifies which input landed where.
    merge(ctx, {corpus("wide.png"), corpus("text_10p.pdf")}, out.str());

    Document doc = Document::open(ctx, out.str());
    CHECK(doc.page_count() == 11);

    Renderer renderer{ctx, doc};
    const leht::PageSize first = renderer.page_size(0, 1.0F);
    const leht::PageSize second = renderer.page_size(1, 1.0F);

    CHECK(first.width > first.height);     // page 0 is the landscape image
    CHECK(second.height > second.width);   // page 1 is a portrait text page

    // ...and the reverse order must put them the other way round.
    TempPdf reversed{"merge_order_rev.pdf"};
    merge(ctx, {corpus("text_10p.pdf"), corpus("wide.png")}, reversed.str());
    Document rdoc = Document::open(ctx, reversed.str());
    Renderer rrenderer{ctx, rdoc};
    const leht::PageSize rfirst = rrenderer.page_size(0, 1.0F);
    const leht::PageSize rlast = rrenderer.page_size(10, 1.0F);
    CHECK(rfirst.height > rfirst.width);   // text first now
    CHECK(rlast.width > rlast.height);     // image last
}

void empty_input_list_throws() {
    Context ctx;
    TempPdf out{"merge_empty.pdf"};
    bool threw = false;
    try {
        merge(ctx, {}, out.str());
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

void missing_input_throws() {
    Context ctx;
    TempPdf out{"merge_missing.pdf"};
    bool threw = false;
    try {
        merge(ctx, {corpus("does-not-exist.pdf")}, out.str());
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

void garbage_collection_deduplicates_shared_resources() {
    Context ctx;
    TempPdf plain{"merge_nogc.pdf"};
    TempPdf deduped{"merge_gc.pdf"};

    const std::vector<std::string> inputs(4, corpus("text_10p.pdf"));

    MergeOptions none;
    none.garbage = 0;
    merge(ctx, inputs, plain.str(), none);

    MergeOptions dedupe;
    dedupe.garbage = 3;
    merge(ctx, inputs, deduped.str(), dedupe);

    // Four copies of one file share all their fonts, so de-duplication must
    // win something measurable.
    CHECK(deduped.size() < plain.size());
}

}  // namespace

int main() {
    RUN(merges_two_pdfs_and_sums_pages);
    RUN(merges_images_and_pdfs_together);
    RUN(jpeg_is_embedded_losslessly);
    RUN(image_page_is_sized_by_resolution);
    RUN(input_order_is_preserved);
    RUN(empty_input_list_throws);
    RUN(missing_input_throws);
    RUN(garbage_collection_deduplicates_shared_resources);
    return 0;
}
