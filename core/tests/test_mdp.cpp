// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Certification (DocMDP) and field locks (FieldMDP): what they write, and how
// the changes made after a signature are judged against them. No
// cryptography: prepare_signature leaves a real signature revision with an
// empty hole, and judging only reads revisions -- whether the signature
// itself verifies is test_pdf_sign's business.
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/forms.hpp"
#include "leht/ops/sign.hpp"
#include "leht/ops/watermark.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::SaveOptions;
using leht::ops::SignatureInfo;
using leht::ops::SignatureRequest;
using leht::test::TempPath;
using leht::test::corpus;

namespace {

/// Signs `in` into `out` (a prepared signature: a real revision, empty hole).
void sign(const std::string& in, const std::string& out, SignatureRequest req = {}) {
    const Context ctx;
    Document doc = Document::open(ctx, in);
    const int fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    try {
        (void)leht::ops::prepare_signature(ctx, doc, req, fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
}

/// Opens `in`, runs `change`, saves to `out` -- incrementally, as any save
/// of a signed document is.
void edit(const std::string& in, const std::string& out,
          const std::function<void(const Context&, Document&)>& change) {
    const Context ctx;
    Document doc = Document::open(ctx, in);
    change(ctx, doc);
    doc.save(out, SaveOptions{});
}

std::vector<SignatureInfo> signatures(const std::string& path) {
    const Context ctx;
    Document doc = Document::open(ctx, path);
    return leht::ops::list_signatures(ctx, doc);
}

template <typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const leht::Error&) {
        return true;
    }
    return false;
}

std::string problems(const SignatureInfo& s) {
    std::string out;
    for (const std::string& p : s.change_problems) {
        out += "        " + p + "\n";
    }
    return out;
}

// The four kinds of later change.
const auto fill_name = [](const Context& ctx, Document& doc) {
    leht::ops::set_field(ctx, doc, "name", "Mari");
};
const auto second_signature = [](const std::string& in, const std::string& out) {
    SignatureRequest req;
    req.override_certification = true;  // at level 1 it must be forced
    sign(in, out, req);
};
const auto annotate = [](const Context& ctx, Document& doc) {
    leht::ops::AnnotSpec note;
    note.kind = leht::ops::AnnotKind::Note;
    note.rect = {100, 100, 100, 100};
    note.contents = "later";
    (void)leht::ops::add_annotation(ctx, doc, 0, note);
};
const auto watermark = [](const Context& ctx, Document& doc) {
    leht::ops::WatermarkOptions mark;
    mark.text = "LATER";
    (void)leht::ops::watermark(ctx, doc, "1", mark);
};

/// Certifies form.pdf at `level`, makes change `kind`, and returns what the
/// certifying signature says about it.
SignatureInfo after(int level, int kind) {
    const std::string tag = std::to_string(level) + "_" + std::to_string(kind);
    const TempPath certified("mdp_cert_" + tag + ".pdf");
    const TempPath changed("mdp_changed_" + tag + ".pdf");
    SignatureRequest req;
    req.certify = level;
    sign(corpus("form.pdf"), certified.str(), req);
    switch (kind) {
        case 0: edit(certified.str(), changed.str(), fill_name); break;
        case 1: second_signature(certified.str(), changed.str()); break;
        case 2: edit(certified.str(), changed.str(), annotate); break;
        default: edit(certified.str(), changed.str(), watermark); break;
    }
    const auto sigs = signatures(changed.str());
    CHECK(!sigs.empty());
    return sigs.front();
}

void the_permission_matrix_holds() {
    if (!std::filesystem::exists(corpus("form.pdf"))) {
        std::printf("      SKIP tests/corpus/form.pdf not generated\n");
        return;
    }
    const char* kinds[] = {"a form field is filled", "a second signature",
                           "an annotation", "a watermark on the page"};
    //                      fill   sign   annot  page
    const bool allowed[4][4] = {{false, false, false, false},  // level 1
                                {true, true, false, false},    // level 2
                                {true, true, true, false},     // level 3
                                {}};
    for (int level = 1; level <= 3; ++level) {
        for (int kind = 0; kind < 4; ++kind) {
            const SignatureInfo s = after(level, kind);
            const bool want = allowed[level - 1][kind];
            if (s.certification != level || !s.changes_judged || s.changes_permitted != want) {
                std::fprintf(stderr, "level %d, %s: certification %d judged %d permitted %d\n%s",
                             level, kinds[kind], s.certification, s.changes_judged,
                             s.changes_permitted, problems(s).c_str());
            }
            CHECK(s.certification == level);
            CHECK(s.changed_after_signing);
            CHECK(s.changes_judged);
            CHECK(s.changes_permitted == want);
            CHECK(s.changes_permitted == s.change_problems.empty());
        }
    }
}

/// MuPDF's own verdict on a file's change history: true when every revision
/// checks out (pdf_validate_change_history returns 0).
bool mupdf_accepts_history(const std::string& path) {
    fz_context* g = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    CHECK(g != nullptr);
    int last_ok = -1;
    fz_try(g) {
        pdf_document* doc = pdf_open_document(g, path.c_str());
        fz_try(g) { last_ok = pdf_validate_change_history(g, doc); }
        fz_always(g) { pdf_drop_document(g, doc); }
        fz_catch(g) { fz_rethrow(g); }
    }
    fz_catch(g) { last_ok = -1; }
    fz_drop_context(g);
    CHECK(last_ok >= 0);
    return last_ok == 0;
}

void level_one_agrees_with_mupdf() {
    // MuPDF enforces DocMDP level 1 itself, so there the two validators must
    // agree: the certified file checks out, and every later change does not.
    // (At levels 2 and 3 MuPDF rejects permitted changes; see mdp.cpp.)
    if (!std::filesystem::exists(corpus("form.pdf"))) {
        std::printf("      SKIP tests/corpus/form.pdf not generated\n");
        return;
    }
    const TempPath certified("mdp_mupdf_cert.pdf");
    SignatureRequest req;
    req.certify = 1;
    sign(corpus("form.pdf"), certified.str(), req);
    CHECK(mupdf_accepts_history(certified.str()));
    for (int kind = 0; kind < 4; ++kind) {
        const TempPath changed("mdp_mupdf_" + std::to_string(kind) + ".pdf");
        switch (kind) {
            case 0: edit(certified.str(), changed.str(), fill_name); break;
            case 1: second_signature(certified.str(), changed.str()); break;
            case 2: edit(certified.str(), changed.str(), annotate); break;
            default: edit(certified.str(), changed.str(), watermark); break;
        }
        const SignatureInfo s = signatures(changed.str()).front();
        CHECK(s.changes_judged && !s.changes_permitted);
        CHECK(!mupdf_accepts_history(changed.str()));
    }
}

void an_approval_signature_passes_no_judgement() {
    // Without a certification or a lock, Leht says what changed and no more.
    const TempPath signed_("mdp_approval.pdf");
    const TempPath changed("mdp_approval_changed.pdf");
    sign(corpus("text_10p.pdf"), signed_.str());
    edit(signed_.str(), changed.str(), watermark);
    const auto s = signatures(changed.str()).front();
    CHECK(s.certification == 0 && s.locks.empty());
    CHECK(s.changed_after_signing && !s.changes_judged && s.changes_permitted);
}

void a_certification_comes_first_and_one_forbids_more() {
    const TempPath signed_("mdp_first.pdf");
    const TempPath again("mdp_again.pdf");
    sign(corpus("text_10p.pdf"), signed_.str());
    SignatureRequest certify;
    certify.certify = 2;
    CHECK(throws([&] { sign(signed_.str(), again.str(), certify); }));

    const TempPath locked("mdp_level1.pdf");
    SignatureRequest none;
    none.certify = 1;
    sign(corpus("text_10p.pdf"), locked.str(), none);
    CHECK(throws([&] { sign(locked.str(), again.str()); }));
    {
        const Context ctx;
        Document doc = Document::open(ctx, locked.str());
        CHECK(leht::ops::certification_level(ctx, doc) == 1);
    }
    CHECK(throws([&] {
        SignatureRequest bad;
        bad.certify = 4;
        sign(corpus("text_10p.pdf"), again.str(), bad);
    }));
}

/// A form with two text fields and an empty signature field whose /Lock names
/// one of them: what a form author does to say "signing locks this".
std::string locked_form(const TempPath& path) {
    leht::test::PdfWriter pdf;
    pdf.set(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R 6 0 R 7 0 R] "
               "/DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv 8 0 R >> >> >> >>");
    pdf.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    pdf.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
               "/Annots [5 0 R 6 0 R 7 0 R] >>");
    pdf.set(4, leht::test::PdfWriter::stream("", "BT /F1 12 Tf 72 720 Td (Form) Tj ET"));
    pdf.set(5, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (amount) /Rect [72 600 272 620] "
               "/P 3 0 R /DA (/Helv 12 Tf 0 g) /V () >>");
    pdf.set(6, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (comment) /Rect [72 560 272 580] "
               "/P 3 0 R /DA (/Helv 12 Tf 0 g) /V () >>");
    pdf.set(7, "<< /Type /Annot /Subtype /Widget /FT /Sig /T (Approval) /Rect [72 400 272 460] "
               "/P 3 0 R /Lock << /Type /SigFieldLock /Action /Include /Fields [(amount)] >> >>");
    pdf.set(8, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    leht::test::write_file(path.str(), pdf.finish(1));
    return path.str();
}

void a_field_lock_is_enacted_and_judged() {
    const TempPath form("mdp_lock_form.pdf");
    const TempPath signed_("mdp_lock_signed.pdf");
    const TempPath other("mdp_lock_other.pdf");
    SignatureRequest req;
    req.field = "Approval";
    sign(locked_form(form), signed_.str(), req);

    // The lock is written as the standard shapes it, and enacted: the locked
    // field is read-only now, and Leht itself will not fill it.
    const std::string bytes = leht::test::read_file(signed_.str());
    CHECK(bytes.find("/TransformMethod/FieldMDP") != std::string::npos ||
          bytes.find("/TransformMethod /FieldMDP") != std::string::npos);
    {
        const Context ctx;
        Document doc = Document::open(ctx, signed_.str());
        for (const auto& f : leht::ops::list_fields(ctx, doc)) {
            if (f.name == "amount") {
                CHECK(f.read_only);
            } else if (f.name == "comment") {
                CHECK(!f.read_only);
            }
        }
        CHECK(throws([&] { leht::ops::set_field(ctx, doc, "amount", "1000"); }));
        CHECK(leht::ops::list_signatures(ctx, doc).front().locks == "field amount");
    }

    // Filling a field the lock leaves open is permitted...
    edit(signed_.str(), other.str(), [](const Context& ctx, Document& doc) {
        leht::ops::set_field(ctx, doc, "comment", "fine");
    });
    auto s = signatures(other.str()).front();
    CHECK(s.changes_judged && s.changes_permitted);

    // ...and changing the locked one is not. Leht will not do it, so the
    // change is appended by hand, as another program might.
    const TempPath forced("mdp_lock_forced.pdf");
    std::string file = leht::test::read_file(signed_.str());
    const std::size_t sx = file.rfind("startxref");
    const std::string prev = file.substr(sx + 10, file.find('\n', sx + 10) - (sx + 10));
    const std::size_t offset = file.size();
    file += "5 0 obj\n<< /Type /Annot /Subtype /Widget /FT /Tx /T (amount) "
            "/Rect [72 600 272 620] /P 3 0 R /DA (/Helv 12 Tf 0 g) /Ff 1 /V (1000000) >>\n"
            "endobj\n";
    const std::size_t xref = file.size();
    char entry[32];
    std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
    // The catalog is object 1 in every revision (locked_form writes it so).
    file += "xref\n5 1\n" + std::string(entry) + "trailer\n<< /Size 40 /Root 1 0 R /Prev " +
            prev + " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    leht::test::write_file(forced.str(), file);
    s = signatures(forced.str()).front();
    if (s.changes_permitted) {
        std::fprintf(stderr, "forced change was permitted (judged %d)\n", s.changes_judged);
    }
    CHECK(s.changes_judged && !s.changes_permitted);
    bool named = false;
    for (const std::string& p : s.change_problems) {
        named = named || p.find("'amount'") != std::string::npos;
    }
    CHECK(named);

    // A later revision whose trailer is broken: MuPDF repairs the file, which
    // folds its revisions into one. What changed can no longer be told, and
    // that must never read as "permitted".
    const TempPath broken("mdp_lock_broken.pdf");
    // (A wrong startxref: the repair rebuilds the xref by scanning the file,
    // which finds every object -- the form included -- but no revisions.)
    std::string damaged = leht::test::read_file(forced.str());
    const std::size_t sx2 = damaged.rfind("startxref\n");
    CHECK(sx2 != std::string::npos);
    damaged.replace(sx2 + 10, damaged.find('\n', sx2 + 10) - (sx2 + 10), "17");
    leht::test::write_file(broken.str(), damaged);
    const auto repaired = signatures(broken.str());
    CHECK(!repaired.empty());
    CHECK(repaired.front().changes_judged && !repaired.front().changes_permitted);
}

}  // namespace

int main() {
    RUN(the_permission_matrix_holds);
    RUN(level_one_agrees_with_mupdf);
    RUN(an_approval_signature_passes_no_judgement);
    RUN(a_certification_comes_first_and_one_forbids_more);
    RUN(a_field_lock_is_enacted_and_judged);
    return 0;
}
