// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/forms.hpp"
#include "leht/text.hpp"
#include "edit_harness.hpp"

#include <algorithm>
#include <string>
#include <vector>

using leht::Context;
using leht::Document;
using leht::SaveOptions;
using leht::TextPage;
using leht::ops::FieldInfo;
using leht::ops::FieldType;
using leht::ops::flatten;
using leht::ops::list_fields;
using leht::ops::set_field;
using leht::test::capture;
using leht::test::corpus;
using leht::test::have_tool;
using leht::test::PdfWriter;
using leht::test::qpdf_check;
using leht::test::read_file;
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

/// An appearance stream: a checkbox's or radio's on/off faces.
std::string face(const char* ops) { return PdfWriter::stream("/BBox [0 0 12 12]", ops); }

/// A one-page AcroForm with one field of every kind leht handles, plus the
/// ones it must refuse. `acroform_extra` is spliced into the AcroForm dict.
std::string form_pdf(const std::string& acroform_extra = "") {
    PdfWriter w;
    w.set(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R 6 0 R 8 0 R 9 0 R "
             "12 0 R 13 0 R 14 0 R 15 0 R] /DA (/Helv 0 Tf 0 g) "
             "/DR << /Font << /Helv 20 0 R >> >> " +
                 acroform_extra + " >> >>");
    w.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    w.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
             "/Resources << >> /Annots [5 0 R 7 0 R 8 0 R 10 0 R 11 0 R 12 0 R 13 0 R 14 0 R 15 0 R] >>");
    w.set(4, PdfWriter::stream("", ""));
    // A text field with a length limit (merged field and widget).
    w.set(5, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /MaxLen 10 "
             "/Rect [72 700 272 720] /P 3 0 R >>");
    // A field hierarchy: "address" with a text kid "street".
    w.set(6, "<< /T (address) /Kids [7 0 R] >>");
    w.set(7, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (street) /Parent 6 0 R "
             "/Rect [72 670 272 690] /P 3 0 R >>");
    // A checkbox whose on-state is named "Yes".
    w.set(8, "<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /AS /Off "
             "/Rect [72 640 84 652] /P 3 0 R "
             "/AP << /N << /Yes 16 0 R /Off 17 0 R >> >> >>");
    // A radio group, two widgets.
    w.set(9, "<< /FT /Btn /Ff 49152 /T (color) /V /Off /Kids [10 0 R 11 0 R] >>");
    w.set(10, "<< /Type /Annot /Subtype /Widget /Parent 9 0 R /AS /Off /Rect [72 610 84 622] "
              "/P 3 0 R /AP << /N << /Red 16 0 R /Off 17 0 R >> >> >>");
    w.set(11, "<< /Type /Annot /Subtype /Widget /Parent 9 0 R /AS /Off /Rect [92 610 104 622] "
              "/P 3 0 R /AP << /N << /Blue 16 0 R /Off 17 0 R >> >> >>");
    // A combo box with export values.
    w.set(12, "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (country) "
              "/Opt [[(ee) (Estonia)] [(fi) (Finland)]] /Rect [72 580 272 600] /P 3 0 R >>");
    // Read-only, with a value.
    w.set(13, "<< /Type /Annot /Subtype /Widget /FT /Tx /Ff 1 /T (id) /V (A-1) "
              "/Rect [300 700 400 720] /P 3 0 R >>");
    // A push button.
    w.set(14, "<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (submit) "
              "/Rect [300 670 400 690] /P 3 0 R >>");
    // A field whose keystroke script would rewrite whatever is typed.
    w.set(15, "<< /Type /Annot /Subtype /Widget /FT /Tx /T (scripted) "
              "/AA << /K << /S /JavaScript /JS (event.value = 'HACKED';) >> "
              "/V << /S /JavaScript /JS (event.rc = false;) >> >> "
              "/Rect [300 640 500 660] /P 3 0 R >>");
    w.set(16, face("0 0 12 12 re f"));
    w.set(17, face(""));
    w.set(20, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    return w.finish(1);
}

const FieldInfo& field(const std::vector<FieldInfo>& all, const std::string& name) {
    const auto it = std::find_if(all.begin(), all.end(),
                                 [&](const FieldInfo& f) { return f.name == name; });
    CHECK(it != all.end());
    return *it;
}

void lists_every_field() {
    const Context ctx;
    const TempPath in("forms_list.pdf");
    write_file(in.str(), form_pdf());
    Document doc = Document::open(ctx, in.str());
    const auto all = list_fields(ctx, doc);
    CHECK(all.size() == 8);

    CHECK(field(all, "name").type == FieldType::Text);
    CHECK(field(all, "name").max_length == 10);
    CHECK(field(all, "address.street").type == FieldType::Text);
    CHECK(field(all, "agree").type == FieldType::Checkbox);
    CHECK(field(all, "agree").options == std::vector<std::string>({"Yes"}));
    CHECK(field(all, "agree").value == "Off");
    CHECK(field(all, "color").type == FieldType::Radio);
    CHECK(field(all, "color").options == std::vector<std::string>({"Red", "Blue"}));
    CHECK(field(all, "country").type == FieldType::Choice);
    CHECK(field(all, "country").options == std::vector<std::string>({"Estonia", "Finland"}));
    CHECK(field(all, "id").read_only);
    CHECK(field(all, "id").value == "A-1");
    CHECK(field(all, "submit").type == FieldType::PushButton);
    CHECK(!field(all, "name").rect.empty());
}

void set_values_persist() {
    const Context ctx;
    const TempPath in("forms_set_in.pdf");
    const TempPath out("forms_set_out.pdf");
    write_file(in.str(), form_pdf());
    {
        Document doc = Document::open(ctx, in.str());
        set_field(ctx, doc, "name", "Marlon");
        set_field(ctx, doc, "address.street", "Tänav 1");
        set_field(ctx, doc, "agree", "yes");
        set_field(ctx, doc, "color", "Blue");
        set_field(ctx, doc, "country", "Finland");
        doc.save(out.str(), SaveOptions{});
    }
    Document doc = Document::open(ctx, out.str());
    const auto all = list_fields(ctx, doc);
    CHECK(field(all, "name").value == "Marlon");
    CHECK(field(all, "address.street").value == "Tänav 1");
    CHECK(field(all, "agree").value == "Yes");
    CHECK(field(all, "color").value == "Blue");
    CHECK(field(all, "country").value == "fi");  // the export value is stored
    CHECK(qpdf_check(out.str()));

    // The appearance was regenerated: the value is drawn on the page.
    CHECK(TextPage(ctx, doc, 0).text().find("Marlon") != std::string::npos);

    // An independent reading of the stored values.
    if (have_tool("qpdf")) {
        const std::string json =
            capture("qpdf --json=2 --json-key=acroform '" + out.str() + "' 2>/dev/null");
        CHECK(json.find("\"value\": \"u:Marlon\"") != std::string::npos);
        CHECK(json.find("\"value\": \"/Yes\"") != std::string::npos);
        CHECK(json.find("\"value\": \"/Blue\"") != std::string::npos);
        CHECK(json.find("\"value\": \"u:fi\"") != std::string::npos);
    }
}

void checkbox_and_radio_switch_off() {
    const Context ctx;
    const TempPath in("forms_off.pdf");
    write_file(in.str(), form_pdf());
    Document doc = Document::open(ctx, in.str());
    set_field(ctx, doc, "agree", "Yes");
    set_field(ctx, doc, "agree", "off");
    set_field(ctx, doc, "color", "Red");
    set_field(ctx, doc, "color", "Blue");
    const auto all = list_fields(ctx, doc);
    CHECK(field(all, "agree").value == "Off");
    CHECK(field(all, "color").value == "Blue");
}

void scripts_never_run() {
    const Context ctx;
    const TempPath in("forms_js.pdf");
    write_file(in.str(), form_pdf());
    Document doc = Document::open(ctx, in.str());
    set_field(ctx, doc, "scripted", "hello");
    CHECK(field(list_fields(ctx, doc), "scripted").value == "hello");
}

void refuses_what_it_should() {
    const Context ctx;
    const TempPath in("forms_refuse.pdf");
    write_file(in.str(), form_pdf());
    Document doc = Document::open(ctx, in.str());
    CHECK(throws([&] { set_field(ctx, doc, "nope", "x"); }));
    CHECK(throws([&] { set_field(ctx, doc, "id", "B-2"); }));             // read-only
    CHECK(throws([&] { set_field(ctx, doc, "submit", "x"); }));           // push button
    CHECK(throws([&] { set_field(ctx, doc, "name", "far too long a name"); }));
    CHECK(throws([&] { set_field(ctx, doc, "agree", "maybe"); }));
    CHECK(throws([&] { set_field(ctx, doc, "color", "Green"); }));
    CHECK(throws([&] { set_field(ctx, doc, "country", "Sweden"); }));    // not editable
    CHECK(field(list_fields(ctx, doc), "id").value == "A-1");
    CHECK(field(list_fields(ctx, doc), "name").value.empty());

    Document png = Document::open(ctx, corpus("page.png"));
    CHECK(throws([&] { (void)list_fields(ctx, png); }));
}

void xfa_only_is_refused_and_hybrid_is_converted() {
    const Context ctx;
    // XFA-only: the AcroForm lists no fields at all.
    PdfWriter w;
    w.set(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [] /XFA 5 0 R >> >>");
    w.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    w.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>");
    w.set(4, PdfWriter::stream("", ""));
    w.set(5, PdfWriter::stream("", "<xdp:xdp/>"));
    const TempPath xfa("forms_xfa.pdf");
    write_file(xfa.str(), w.finish(1));
    Document only = Document::open(ctx, xfa.str());
    CHECK(throws([&] { (void)list_fields(ctx, only); }));

    // Hybrid: fill the AcroForm, and the stale XFA copy goes.
    const TempPath in("forms_hybrid_in.pdf");
    const TempPath out("forms_hybrid_out.pdf");
    std::string hybrid = form_pdf("/XFA 21 0 R");
    write_file(in.str(), hybrid);
    Document doc = Document::open(ctx, in.str());
    CHECK(list_fields(ctx, doc).size() == 8);
    set_field(ctx, doc, "name", "Marlon");
    doc.save(out.str(), SaveOptions{});
    CHECK(read_file(out.str()).find("/XFA") == std::string::npos);
}

void flatten_bakes_the_values_in() {
    const Context ctx;
    const TempPath in("forms_flat_in.pdf");
    const TempPath out("forms_flat_out.pdf");
    write_file(in.str(), form_pdf());
    {
        Document doc = Document::open(ctx, in.str());
        set_field(ctx, doc, "name", "Marlon");
        CHECK(flatten(ctx, doc) == 8);
        doc.save(out.str(), SaveOptions{});
    }
    Document doc = Document::open(ctx, out.str());
    CHECK(list_fields(ctx, doc).empty());
    CHECK(TextPage(ctx, doc, 0).text().find("Marlon") != std::string::npos);
    CHECK(qpdf_check(out.str()));
    if (have_tool("pdftotext")) {
        CHECK(capture("pdftotext '" + out.str() + "' - 2>/dev/null").find("Marlon") !=
              std::string::npos);
    }
}

}  // namespace

int main() {
    RUN(lists_every_field);
    RUN(set_values_persist);
    RUN(checkbox_and_radio_switch_off);
    RUN(scripts_never_run);
    RUN(refuses_what_it_should);
    RUN(xfa_only_is_refused_and_hybrid_is_converted);
    RUN(flatten_bakes_the_values_in);
    return 0;
}
