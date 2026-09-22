// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Signing real PDFs end to end: ops::prepare_signature (the PDF side) and
// crypto::sign_prepared (the key side), then reading and verifying the result
// with our code, and with poppler's pdfsig and qpdf, which are not our code.
#include "leht/context.hpp"
#include "leht/crypto/crypto.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/redact.hpp"
#include "leht/ops/sign.hpp"
#include "leht/ops/watermark.hpp"
#include "leht/renderer.hpp"
#include "edit_harness.hpp"
#include "test_pki.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <string>

using leht::Context;
using leht::Document;
using leht::crypto::CmsReport;
using leht::crypto::Identity;
using leht::crypto::SignOptions;
using leht::crypto::Trust;
using leht::ops::SignatureInfo;
using leht::ops::SignatureRequest;
using leht::test::corpus;
using leht::test::qpdf_check;
using leht::test::TempPath;

namespace {

const leht::test::Pki& pki() {
    static const leht::test::Pki p;
    return p;
}

const Identity& signer() {
    static const Identity id = pki().identity(pki().rsa, pki().rsa_cert);
    return id;
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

/// Both halves, as the CLI does them: prepare into a fresh file, then sign it.
leht::crypto::SignResult sign_into(const Context& ctx, Document& doc, const std::string& out,
                                   SignatureRequest req, const SignOptions& options = {}) {
    req.reserve = leht::crypto::estimate_signature_size(signer(), options);
    const int fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    try {
        const auto prepared = leht::ops::prepare_signature(ctx, doc, req, fd);
        const auto result = leht::crypto::sign_prepared(fd, prepared.range, signer(), options);
        ::close(fd);
        return result;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

leht::crypto::SignResult sign_file(const std::string& in, const std::string& out,
                                   SignatureRequest req = {}, const SignOptions& options = {}) {
    const Context ctx;
    Document doc = Document::open(ctx, in);
    return sign_into(ctx, doc, out, std::move(req), options);
}

struct Checked {
    SignatureInfo info;
    CmsReport report;
};

std::vector<Checked> verify_file(const std::string& path) {
    const Context ctx;
    Document doc = Document::open(ctx, path);
    std::vector<Checked> out;
    for (SignatureInfo& s : leht::ops::list_signatures(ctx, doc)) {
        CmsReport r;
        if (s.range_ok) {
            r = leht::crypto::verify_cms(s.contents, leht::ops::signed_bytes(ctx, doc, s.range),
                                         pki().trust());
        }
        out.push_back({std::move(s), std::move(r)});
    }
    return out;
}

/// poppler's pdfsig, NSS underneath: an independent verifier. It does not know
/// our CA, so it can confirm integrity, not trust.
bool pdfsig_says_valid(const std::string& path, int signatures) {
    if (!leht::test::have_tool("pdfsig")) {
        std::fprintf(stderr, "  SKIP pdfsig (poppler-utils not installed)\n");
        return true;
    }
    const std::string out = leht::test::capture("pdfsig '" + path + "' 2>&1");
    int valid = 0;
    for (std::size_t at = 0; (at = out.find("Signature is Valid", at)) != std::string::npos;
         ++at) {
        ++valid;
    }
    if (valid != signatures) {
        std::fprintf(stderr, "pdfsig said:\n%s\n", out.c_str());
    }
    return valid == signatures;
}

/// Non-white pixels inside `r` (base coordinates) of page `page`, at zoom 1.
int ink_in(const std::string& path, int page, const leht::Rect& r) {
    const Context ctx;
    Document doc = Document::open(ctx, path);
    leht::Renderer renderer(ctx, doc);
    const auto bmp = renderer.render(page, 1.0F);
    CHECK(bmp.has_value());
    int ink = 0;
    for (int y = static_cast<int>(r.y0); y < static_cast<int>(r.y1) && y < bmp->height; ++y) {
        for (int x = static_cast<int>(r.x0); x < static_cast<int>(r.x1) && x < bmp->width; ++x) {
            const std::uint8_t* p = &bmp->pixels[static_cast<std::size_t>(y * bmp->stride +
                                                                           x * bmp->channels)];
            if (p[0] < 200 || p[1] < 200 || p[2] < 200) {
                ++ink;
            }
        }
    }
    return ink;
}

void an_invisible_signature_is_pades_b_b_and_verifies() {
    const TempPath out("sign_invisible.pdf");
    SignatureRequest req;
    req.name = "Mari Maasikas";
    req.reason = "Approved";
    req.location = "Tallinn";
    const auto r = sign_file(corpus("text_10p.pdf"), out.str(), req);
    CHECK(r.der_size > 0);

    // An incremental update: the original bytes are untouched.
    const std::string original = leht::test::read_file(corpus("text_10p.pdf"));
    const std::string signed_ = leht::test::read_file(out.str());
    CHECK(signed_.compare(0, original.size(), original) == 0);

    const auto sigs = verify_file(out.str());
    CHECK(sigs.size() == 1);
    const Checked& s = sigs.front();
    CHECK(s.info.field == "Signature1");
    CHECK(s.info.subfilter == "ETSI.CAdES.detached");
    CHECK(s.info.filter == "Adobe.PPKLite");
    CHECK(s.info.name == "Mari Maasikas");
    CHECK(s.info.reason == "Approved");
    CHECK(s.info.location == "Tallinn");
    CHECK(s.info.claimed_time.rfind("D:", 0) == 0);
    CHECK(s.info.range_ok);
    CHECK(s.info.covers_whole_revision);
    CHECK(!s.info.changed_after_signing);
    CHECK(s.info.range.end() == static_cast<std::int64_t>(signed_.size()));
    CHECK(s.report.intact());
    CHECK(s.report.trust == Trust::Trusted);
    CHECK(s.report.has_signing_certificate_v2);
    CHECK(!s.report.has_signing_time_attribute);
    // No FieldMDP transform was asked for, so none is written.
    CHECK(signed_.find("/TransformMethod") == std::string::npos);
    CHECK(signed_.find("/Lock") == std::string::npos);

    CHECK(qpdf_check(out.str()));
    CHECK(pdfsig_says_valid(out.str(), 1));
}

void a_visible_signature_draws_its_appearance() {
    const TempPath out("sign_visible.pdf");
    SignatureRequest req;
    req.page = 0;
    req.rect = {300, 650, 560, 740};
    req.name = "Jaan Tamm";
    req.appearance.strokes = {{{0, 50}, {20, 10}, {40, 60}, {60, 20}, {80, 55}, {100, 15}}};
    req.appearance.strokes_width = 100;
    req.appearance.strokes_height = 70;
    req.appearance.lines = {"Jaan Tamm", "2026-09-22"};
    (void)sign_file(corpus("text_10p.pdf"), out.str(), req);
    const auto sigs = verify_file(out.str());
    CHECK(sigs.size() == 1 && sigs[0].report.intact());
    CHECK(sigs[0].info.page == 0);
    CHECK(sigs[0].info.rect.x0 > 299 && sigs[0].info.rect.x1 < 561);
    CHECK(ink_in(out.str(), 0, req.rect) > 100);
    CHECK(pdfsig_says_valid(out.str(), 1));
    // poppler draws the appearance too: an independent renderer agrees.
    if (leht::test::have_tool("pdftoppm")) {
        const TempPath png("sign_visible_poppler");
        (void)std::system(("pdftoppm -r 72 -f 1 -l 1 -png -singlefile '" + out.str() + "' '" +
                           png.str() + "'").c_str());
        const Context ctx;
        Document img = Document::open(ctx, png.str() + ".png");
        leht::Renderer r(ctx, img);
        const auto bmp = r.render(0, 1.0F);
        int ink = 0;
        for (int y = 655; y < 735; ++y) {
            for (int x = 305; x < 555; ++x) {
                const std::uint8_t* p = &bmp->pixels[static_cast<std::size_t>(y * bmp->stride +
                                                                               x * bmp->channels)];
                ink += (p[0] < 200) ? 1 : 0;
            }
        }
        std::remove((png.str() + ".png").c_str());
        CHECK(ink > 100);
    }
}

void an_image_appearance_works() {
    const TempPath out("sign_image.pdf");
    SignatureRequest req;
    req.rect = {100, 100, 300, 180};
    const std::string png = leht::test::read_file(corpus("page.png"));
    req.appearance.image.assign(png.begin(), png.end());
    (void)sign_file(corpus("text_10p.pdf"), out.str(), req);
    const auto sigs = verify_file(out.str());
    CHECK(sigs.size() == 1 && sigs[0].report.intact());
    CHECK(ink_in(out.str(), 0, req.rect) > 0);

    // Something that is not an image is refused before anything is written.
    SignatureRequest bad = req;
    bad.appearance.image = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
    const TempPath out2("sign_image_bad.pdf");
    CHECK(throws([&] { (void)sign_file(corpus("text_10p.pdf"), out2.str(), bad); }));
}

void a_second_signature_keeps_the_first_valid() {
    const TempPath one("sign_one.pdf");
    const TempPath two("sign_two.pdf");
    SignatureRequest first;
    first.name = "First";
    (void)sign_file(corpus("text_10p.pdf"), one.str(), first);
    SignatureRequest second;
    second.name = "Second";
    second.rect = {50, 50, 250, 110};
    (void)sign_file(one.str(), two.str(), second);

    const auto sigs = verify_file(two.str());
    CHECK(sigs.size() == 2);
    CHECK(sigs[0].info.field == "Signature1" && sigs[1].info.field == "Signature2");
    CHECK(sigs[0].report.intact() && sigs[1].report.intact());
    // The first has a later revision after it -- the second signature -- and
    // that second signature covers those bytes too.
    CHECK(sigs[0].info.changed_after_signing);
    CHECK(sigs[0].info.later_signature_covers_changes);
    CHECK(!sigs[1].info.changed_after_signing);
    CHECK(pdfsig_says_valid(two.str(), 2));
    CHECK(qpdf_check(two.str()));
}

void edits_after_signing_are_seen() {
    const TempPath signed_("sign_then_edit.pdf");
    (void)sign_file(corpus("text_10p.pdf"), signed_.str());
    {
        // An annotation, saved the default way: Auto sees the signature and
        // appends a revision, so the signature itself stays intact.
        const TempPath annotated("sign_then_annotate.pdf");
        const Context ctx;
        Document doc = Document::open(ctx, signed_.str());
        leht::ops::AnnotSpec note;
        note.kind = leht::ops::AnnotKind::Note;
        note.rect = {30, 30, 50, 50};
        note.contents = "added later";
        (void)leht::ops::add_annotation(ctx, doc, 0, note);
        doc.save(annotated.str(), leht::SaveOptions{});
        const auto sigs = verify_file(annotated.str());
        CHECK(sigs.size() == 1);
        CHECK(sigs[0].report.intact());
        CHECK(sigs[0].info.changed_after_signing);
        CHECK(!sigs[0].info.later_signature_covers_changes);
    }
    {
        // New page content is not a permitted change.
        const TempPath marked("sign_then_watermark.pdf");
        const Context ctx;
        Document doc = Document::open(ctx, signed_.str());
        leht::ops::WatermarkOptions wm;
        wm.text = "COPY";
        (void)leht::ops::watermark(ctx, doc, "", wm);
        doc.save(marked.str(), leht::SaveOptions{});
        const auto sigs = verify_file(marked.str());
        CHECK(sigs.size() == 1);
        CHECK(sigs[0].report.intact());  // the signed bytes are untouched...
        CHECK(sigs[0].info.changed_after_signing);  // ...but the page shows more now
        CHECK(!sigs[0].info.later_signature_covers_changes);
    }
}

void a_changed_byte_breaks_it() {
    const TempPath out("sign_tamper.pdf");
    (void)sign_file(corpus("text_10p.pdf"), out.str());
    std::string bytes = leht::test::read_file(out.str());
    const std::size_t at = bytes.find("Lorem");
    const std::size_t pos = at != std::string::npos ? at : 200;
    bytes[pos] = static_cast<char>(bytes[pos] ^ 0x20);
    leht::test::write_file(out.str(), bytes);
    const auto sigs = verify_file(out.str());
    CHECK(sigs.size() == 1);
    CHECK(sigs[0].info.range_ok);
    CHECK(!sigs[0].report.intact());
}

void an_existing_field_is_signed_once() {
    // A form with an empty signature field, as a form author would leave it.
    leht::test::PdfWriter w;
    w.set(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] >> >>");
    w.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    w.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [4 0 R] >>");
    w.set(4, "<< /FT /Sig /T (Approver) /Type /Annot /Subtype /Widget "
             "/Rect [20 20 220 80] /P 3 0 R /F 4 >>");
    const TempPath form("sign_form.pdf");
    leht::test::write_file(form.str(), w.finish(1));

    const TempPath out("sign_form_signed.pdf");
    SignatureRequest req;
    req.field = "Approver";
    req.name = "Kati Karu";
    (void)sign_file(form.str(), out.str(), req);
    const auto sigs = verify_file(out.str());
    CHECK(sigs.size() == 1);
    CHECK(sigs[0].info.field == "Approver");
    CHECK(sigs[0].report.intact());
    CHECK(pdfsig_says_valid(out.str(), 1));

    const TempPath again("sign_form_again.pdf");
    CHECK(throws([&] { (void)sign_file(out.str(), again.str(), req); }));
    req.field = "Nobody";
    CHECK(throws([&] { (void)sign_file(form.str(), again.str(), req); }));
}

void a_redacted_document_must_be_saved_first() {
    const Context ctx;
    Document doc = Document::open(ctx, corpus("text_10p.pdf"));
    (void)leht::ops::redact(ctx, doc, 0, {leht::Rect{50, 50, 200, 100}});
    const TempPath out("sign_redacted.pdf");
    CHECK(throws([&] { (void)sign_into(ctx, doc, out.str(), SignatureRequest{}); }));
}

void a_lying_byte_range_is_not_believed() {
    // A signature whose ByteRange gap is somewhere other than its /Contents:
    // the bytes it "signs" would not be the ones the blob came from.
    const TempPath good("sign_lie_base.pdf");
    (void)sign_file(corpus("text_10p.pdf"), good.str());
    std::string bytes = leht::test::read_file(good.str());
    const std::size_t br = bytes.rfind("/ByteRange[");
    CHECK(br != std::string::npos);
    // Shift the second span's start by one byte, keeping the length of the text.
    std::size_t p = br + 11;
    for (int field = 0; field < 2; ++field) {
        p = bytes.find(' ', p) + 1;
    }
    char& digit = bytes[p];
    digit = digit == '9' ? '8' : static_cast<char>(digit + 1);
    leht::test::write_file(good.str(), bytes);
    const auto sigs = verify_file(good.str());
    CHECK(sigs.size() == 1);
    CHECK(!sigs[0].info.range_ok);
    CHECK(!sigs[0].info.range_problem.empty());
}

void a_timestamped_signature_is_b_t() {
    const leht::test::LocalTsa tsa(pki().tsa_key, pki().tsa_cert, pki().ca);
    SignOptions o;
    o.tsa_url = tsa.url();
    const TempPath out("sign_b_t.pdf");
    const auto r = sign_file(corpus("text_10p.pdf"), out.str(), {}, o);
    CHECK(r.timestamp.has_value());
    const auto sigs = verify_file(out.str());
    CHECK(sigs.size() == 1 && sigs[0].report.intact());
    CHECK(sigs[0].report.timestamp && sigs[0].report.timestamp->valid);
    CHECK(sigs[0].report.timestamp->trust == Trust::Trusted);
    CHECK(pdfsig_says_valid(out.str(), 1));
}

void a_signature_stamp_is_only_a_picture() {
    const TempPath out("sign_stamp.pdf");
    const Context ctx;
    {
        Document doc = Document::open(ctx, corpus("text_10p.pdf"));
        leht::ops::Appearance a;
        a.strokes = {{{0, 0}, {10, 10}, {20, 0}, {30, 10}}};
        a.strokes_width = 30;
        a.strokes_height = 10;
        a.lines = {"M. Maasikas"};
        const int id = leht::ops::add_signature_stamp(ctx, doc, 1, {100, 600, 300, 660}, a);
        CHECK(id > 0);
        CHECK(throws([&] { (void)leht::ops::add_signature_stamp(ctx, doc, 1, {}, a); }));
        CHECK(throws([&] {
            (void)leht::ops::add_signature_stamp(ctx, doc, 1, {1, 1, 9, 9}, leht::ops::Appearance{});
        }));
        doc.save(out.str(), leht::SaveOptions{});
    }
    Document doc = Document::open(ctx, out.str());
    bool found = false;
    for (const auto& info : leht::ops::list_annotations(ctx, doc)) {
        if (info.type == "Stamp" && info.page == 1) {
            found = info.contents.find("not a digital signature") != std::string::npos;
        }
    }
    CHECK(found);
    CHECK(leht::ops::list_signatures(ctx, doc).empty());
    CHECK(ink_in(out.str(), 1, {100, 600, 300, 660}) > 50);
}

}  // namespace

int main() {
    leht::crypto::init();
    RUN(an_invisible_signature_is_pades_b_b_and_verifies);
    RUN(a_visible_signature_draws_its_appearance);
    RUN(an_image_appearance_works);
    RUN(a_second_signature_keeps_the_first_valid);
    RUN(edits_after_signing_are_seen);
    RUN(a_changed_byte_breaks_it);
    RUN(an_existing_field_is_signed_once);
    RUN(a_redacted_document_must_be_saved_first);
    RUN(a_lying_byte_range_is_not_believed);
    RUN(a_timestamped_signature_is_b_t);
    RUN(a_signature_stamp_is_only_a_picture);
    return 0;
}
