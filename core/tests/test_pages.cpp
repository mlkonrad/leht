// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/pages.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::ops::extract;
using leht::ops::PagesResult;
using leht::ops::parse_page_ranges;
using leht::ops::remove_pages;
using leht::ops::rotate;
using leht::ops::split;

namespace {

namespace fs = std::filesystem;

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

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

private:
    fs::path path_;
};

bool throws_on_range(const std::string& spec, int pages) {
    try {
        (void)parse_page_ranges(spec, pages);
        return false;
    } catch (const leht::Error&) {
        return true;
    }
}

void empty_spec_selects_every_page() {
    const std::vector<int> all = parse_page_ranges("", 4);
    CHECK(all == std::vector<int>({0, 1, 2, 3}));
    CHECK(parse_page_ranges("   ", 3) == std::vector<int>({0, 1, 2}));
}

void single_pages_and_lists() {
    CHECK(parse_page_ranges("1", 10) == std::vector<int>({0}));
    CHECK(parse_page_ranges("1,3,5", 10) == std::vector<int>({0, 2, 4}));
    CHECK(parse_page_ranges(" 2 , 4 ", 10) == std::vector<int>({1, 3}));
}

void inclusive_ranges() {
    CHECK(parse_page_ranges("1-3", 10) == std::vector<int>({0, 1, 2}));
    CHECK(parse_page_ranges("2-2", 10) == std::vector<int>({1}));
}

void open_ended_ranges() {
    CHECK(parse_page_ranges("3-", 5) == std::vector<int>({2, 3, 4}));
    CHECK(parse_page_ranges("-3", 5) == std::vector<int>({0, 1, 2}));
    CHECK(parse_page_ranges("-", 3) == std::vector<int>({0, 1, 2}));
}

/// "5-1" reversing is a feature: it is how a caller reverses page order.
void descending_range_reverses() {
    CHECK(parse_page_ranges("5-1", 10) == std::vector<int>({4, 3, 2, 1, 0}));
    CHECK(parse_page_ranges("-", 3) == std::vector<int>({0, 1, 2}));
}

/// Repeating a page is legitimate, so duplicates must survive.
void duplicates_are_preserved() {
    CHECK(parse_page_ranges("1,1,2", 5) == std::vector<int>({0, 0, 1}));
}

void bad_ranges_are_rejected() {
    CHECK(throws_on_range("0", 5));        // 1-based, so 0 is invalid
    CHECK(throws_on_range("6", 5));        // past the end
    CHECK(throws_on_range("1-9", 5));      // range past the end
    CHECK(throws_on_range("abc", 5));      // not a number
    CHECK(throws_on_range("1-x", 5));      // half not a number
    CHECK(throws_on_range("1", 0));        // no pages at all
}

void extract_writes_only_selected_pages() {
    Context ctx;
    TempPdf out{"pages_extract.pdf"};
    const PagesResult result =
        extract(ctx, corpus("text_10p.pdf"), out.str(), "2-4");

    CHECK(result.pages_written == 3);
    CHECK(result.output_bytes > 0);

    Document doc = Document::open(ctx, out.str());
    CHECK(doc.page_count() == 3);
}

void remove_writes_the_complement() {
    Context ctx;
    TempPdf out{"pages_remove.pdf"};
    const PagesResult result =
        remove_pages(ctx, corpus("text_10p.pdf"), out.str(), "1,2");

    CHECK(result.pages_written == 8);
    Document doc = Document::open(ctx, out.str());
    CHECK(doc.page_count() == 8);
}

void removing_every_page_is_refused() {
    Context ctx;
    TempPdf out{"pages_removeall.pdf"};
    bool threw = false;
    try {
        remove_pages(ctx, corpus("text_10p.pdf"), out.str(), "1-10");
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

void rotate_keeps_all_pages_and_writes() {
    Context ctx;
    TempPdf out{"pages_rotate.pdf"};
    const PagesResult result =
        rotate(ctx, corpus("text_10p.pdf"), out.str(), "1-3", 90);

    CHECK(result.pages_written == 10);  // all pages kept, three turned
    Document doc = Document::open(ctx, out.str());
    CHECK(doc.page_count() == 10);
}

void rotate_rejects_non_right_angles() {
    Context ctx;
    TempPdf out{"pages_rot_bad.pdf"};
    bool threw = false;
    try {
        rotate(ctx, corpus("text_10p.pdf"), out.str(), "", 45);
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

void split_writes_one_file_per_page() {
    Context ctx;
    const fs::path dir = fs::temp_directory_path() / "leht_split";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string pattern = (dir / "part-%03d.pdf").string();
    const std::vector<std::string> written =
        split(ctx, corpus("text_10p.pdf"), pattern, 1);

    CHECK(written.size() == 10);
    for (const std::string& path : written) {
        CHECK(fs::exists(path));
        Document doc = Document::open(ctx, path);
        CHECK(doc.page_count() == 1);
    }
    fs::remove_all(dir);
}

void split_honours_chunk_size() {
    Context ctx;
    const fs::path dir = fs::temp_directory_path() / "leht_split_chunk";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string pattern = (dir / "chunk-%d.pdf").string();
    const std::vector<std::string> written =
        split(ctx, corpus("text_10p.pdf"), pattern, 4);

    CHECK(written.size() == 3);  // 4 + 4 + 2
    Document last = Document::open(ctx, written.back());
    CHECK(last.page_count() == 2);
    fs::remove_all(dir);
}

void split_requires_a_format_field() {
    Context ctx;
    bool threw = false;
    try {
        split(ctx, corpus("text_10p.pdf"), "no-field.pdf", 1);
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    RUN(empty_spec_selects_every_page);
    RUN(single_pages_and_lists);
    RUN(inclusive_ranges);
    RUN(open_ended_ranges);
    RUN(descending_range_reverses);
    RUN(duplicates_are_preserved);
    RUN(bad_ranges_are_rejected);
    RUN(extract_writes_only_selected_pages);
    RUN(remove_writes_the_complement);
    RUN(removing_every_page_is_refused);
    RUN(rotate_keeps_all_pages_and_writes);
    RUN(rotate_rejects_non_right_angles);
    RUN(split_writes_one_file_per_page);
    RUN(split_honours_chunk_size);
    RUN(split_requires_a_format_field);
    return 0;
}
