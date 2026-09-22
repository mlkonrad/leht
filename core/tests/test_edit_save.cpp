// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "edit_harness.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>

using leht::Context;
using leht::Document;
using leht::SaveOptions;
using leht::test::corpus;
using leht::test::qpdf_check;
using leht::test::TempPath;

namespace {

namespace fs = std::filesystem;

void save_round_trips() {
    const Context ctx;
    const TempPath out("edit_save_round_trip.pdf");
    {
        const Document doc = Document::open(ctx, corpus("text_10p.pdf"));
        CHECK(doc.is_pdf());
        doc.save(out.str(), SaveOptions{});
    }
    const Document again = Document::open(ctx, out.str());
    CHECK(again.page_count() == 10);
    CHECK(qpdf_check(out.str()));
}

void save_fd_writes_into_a_borrowed_descriptor() {
    const Context ctx;
    const TempPath out("edit_save_fd.pdf");
    const int fd = ::open(out.str().c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    {
        const Document doc = Document::open(ctx, corpus("text_10p.pdf"));
        doc.save_fd(fd, SaveOptions{});
    }
    // Still ours: save_fd must not have closed it.
    CHECK(::fcntl(fd, F_GETFD) != -1);
    ::close(fd);
    CHECK(Document::open(ctx, out.str()).page_count() == 10);
    CHECK(qpdf_check(out.str()));
}

void saving_over_the_open_file_is_safe() {
    const Context ctx;
    const TempPath path("edit_save_in_place.pdf");
    fs::copy_file(corpus("text_10p.pdf"), path.str());
    ::chmod(path.str().c_str(), 0640);

    const Document doc = Document::open(ctx, path.str());
    doc.save(path.str(), SaveOptions{});
    // The open document still reads the old inode, so it is unharmed...
    CHECK(doc.page_count() == 10);
    // ...and saving it a second time, from that old inode, still works.
    doc.save(path.str(), SaveOptions{});
    CHECK(Document::open(ctx, path.str()).page_count() == 10);

    struct stat st {};
    CHECK(::stat(path.str().c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0640);

    // No temporary file left behind.
    for (const auto& entry : fs::directory_iterator(fs::path(path.str()).parent_path())) {
        CHECK(entry.path().filename().string().find("leht_test_edit_save_in_place.pdf.leht-") ==
              std::string::npos);
    }
}

void a_failed_save_leaves_the_target_alone() {
    const Context ctx;
    const Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    bool threw = false;
    try {
        doc.save("/nonexistent-dir/out.pdf", SaveOptions{});
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

void non_pdf_documents_cannot_be_saved() {
    const Context ctx;
    const Document img = Document::open(ctx, corpus("page.png"));
    CHECK(!img.is_pdf());
    const TempPath out("edit_save_png.pdf");
    bool threw = false;
    try {
        img.save(out.str(), SaveOptions{});
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!fs::exists(out.str()));
}

void page_set_sorts_and_deduplicates() {
    CHECK(leht::page_set("3,1,1-2", 4) == std::vector<int>({0, 1, 2}));
    CHECK(leht::page_set("", 3) == std::vector<int>({0, 1, 2}));
}

}  // namespace

int main() {
    RUN(save_round_trips);
    RUN(save_fd_writes_into_a_borrowed_descriptor);
    RUN(saving_over_the_open_file_is_safe);
    RUN(a_failed_save_leaves_the_target_alone);
    RUN(non_pdf_documents_cannot_be_saved);
    RUN(page_set_sorts_and_deduplicates);
    return 0;
}
