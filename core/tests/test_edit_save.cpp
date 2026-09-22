// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/redact.hpp"
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

/// One page with a signature field whose value is a (fake) signature: enough
/// structure for signature_count() and the save-mode decision, which never look
/// at whether the signature verifies.
std::string signed_fixture() {
    leht::test::PdfWriter w;
    w.set(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] /SigFlags 3 >> >>");
    w.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    w.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [4 0 R] >>");
    w.set(4, "<< /FT /Sig /T (Sig1) /V 5 0 R /Type /Annot /Subtype /Widget "
             "/Rect [0 0 0 0] /P 3 0 R /F 132 >>");
    w.set(5, "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /ETSI.CAdES.detached "
             "/ByteRange [0 0 0 0] /Contents <00> >>");
    return w.finish(1);
}

void note(const Context& ctx, Document& doc) {
    leht::ops::AnnotSpec spec;
    spec.kind = leht::ops::AnnotKind::Note;
    spec.rect = {20, 20, 40, 40};
    spec.contents = "after";
    (void)leht::ops::add_annotation(ctx, doc, 0, spec);
}

void incremental_save_keeps_the_original_bytes() {
    const Context ctx;
    const TempPath out("edit_save_incremental.pdf");
    const std::string original = leht::test::read_file(corpus("text_10p.pdf"));
    {
        Document doc = Document::open(ctx, corpus("text_10p.pdf"));
        CHECK(doc.signature_count() == 0);
        CHECK(doc.can_save_incrementally());
        note(ctx, doc);
        SaveOptions opts;
        opts.mode = SaveOptions::Mode::Incremental;
        CHECK(doc.saves_incrementally(opts));
        doc.save(out.str(), opts);
    }
    const std::string saved = leht::test::read_file(out.str());
    CHECK(saved.size() > original.size());
    CHECK(saved.compare(0, original.size(), original) == 0);
    CHECK(qpdf_check(out.str()));
    const Document again = Document::open(ctx, out.str());
    CHECK(again.page_count() == 10);
}

void auto_mode_updates_a_signed_document_incrementally() {
    const Context ctx;
    const TempPath in("edit_save_signed_in.pdf");
    const TempPath out("edit_save_signed_out.pdf");
    const std::string original = signed_fixture();
    leht::test::write_file(in.str(), original);

    Document doc = Document::open(ctx, in.str());
    CHECK(doc.signature_count() == 1);
    CHECK(doc.saves_incrementally(SaveOptions{}));
    note(ctx, doc);
    doc.save(out.str(), SaveOptions{});
    const std::string saved = leht::test::read_file(out.str());
    CHECK(saved.compare(0, original.size(), original) == 0);
    CHECK(Document::open(ctx, out.str()).signature_count() == 1);

    // Full is still available on request, and it is a rewrite.
    SaveOptions full;
    full.mode = SaveOptions::Mode::Full;
    CHECK(!doc.saves_incrementally(full));
}

void unsigned_documents_are_still_rewritten() {
    const Context ctx;
    const Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    CHECK(!doc.saves_incrementally(SaveOptions{}));
}

void a_redacted_document_is_never_saved_incrementally() {
    const Context ctx;
    const TempPath in("edit_save_signed_redact.pdf");
    leht::test::write_file(in.str(), signed_fixture());
    Document doc = Document::open(ctx, in.str());
    const auto r = leht::ops::redact(ctx, doc, 0, {leht::Rect{100, 100, 150, 150}});
    CHECK(r.signatures_invalidated == 1);
    CHECK(!doc.can_save_incrementally());
    CHECK(!doc.saves_incrementally(SaveOptions{}));
    SaveOptions inc;
    inc.mode = SaveOptions::Mode::Incremental;
    bool threw = false;
    try {
        (void)doc.saves_incrementally(inc);
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

void a_second_save_after_an_incremental_one_is_refused() {
    // MuPDF treats the open document's file as if it now held the revision it
    // just wrote. A second incremental save chained its /Prev to an xref that
    // is not in the original, and qpdf found an xref loop. Refuse instead.
    const Context ctx;
    const TempPath a("edit_save_inc_a.pdf");
    const TempPath b("edit_save_inc_b.pdf");
    SaveOptions opts;
    opts.mode = SaveOptions::Mode::Incremental;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    note(ctx, doc);
    const int fd = ::open(a.str().c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    doc.save_fd(fd, opts);
    ::close(fd);
    CHECK(qpdf_check(a.str()));
    bool threw = false;
    try {
        doc.save(b.str(), SaveOptions{});
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
    // The saved file itself saves incrementally again without trouble.
    Document reopened = Document::open(ctx, a.str());
    note(ctx, reopened);
    reopened.save(b.str(), opts);
    CHECK(qpdf_check(b.str()));
    CHECK(Document::open(ctx, b.str()).page_count() == 10);
}

}  // namespace

int main() {
    RUN(incremental_save_keeps_the_original_bytes);
    RUN(auto_mode_updates_a_signed_document_incrementally);
    RUN(unsigned_documents_are_still_rewritten);
    RUN(a_redacted_document_is_never_saved_incrementally);
    RUN(a_second_save_after_an_incremental_one_is_refused);
    RUN(save_round_trips);
    RUN(save_fd_writes_into_a_borrowed_descriptor);
    RUN(saving_over_the_open_file_is_safe);
    RUN(a_failed_save_leaves_the_target_alone);
    RUN(non_pdf_documents_cannot_be_saved);
    RUN(page_set_sorts_and_deduplicates);
    return 0;
}
