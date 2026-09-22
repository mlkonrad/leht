// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/watermark.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace leht::ops {

namespace {

using OwnedFont = detail::Owned<fz_font, fz_drop_font>;

const unsigned char kSave[] = {'q', '\n'};
const unsigned char kRestore[] = {'Q', '\n'};

/// `rune` in Windows-1252, the encoding the base-14 font is given, or -1.
int to_windows_1252(int rune) {
    if ((rune >= 0x20 && rune <= 0x7E) || (rune >= 0xA0 && rune <= 0xFF)) {
        return rune;
    }
    // The 0x80-0x9F block, which 1252 fills where Latin-1 has controls.
    static constexpr int kHigh[32] = {
        0x20AC, -1,     0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, -1,     0x017D, -1,
        -1,     0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, -1,     0x017E, 0x0178};
    for (int i = 0; i < 32; ++i) {
        if (kHigh[i] == rune) {
            return 0x80 + i;
        }
    }
    return -1;
}

/// The text as a PDF hex string in Windows-1252: "<48656C6C6F>". Throws on a
/// character the encoding lacks, rather than drawing it as a box.
std::string encode_1252_hex(const std::string& utf8) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = "<";
    for (const char* p = utf8.c_str(); *p != '\0';) {
        int rune = 0;
        p += fz_chartorune(&rune, p);
        const int code = rune == FZ_REPLACEMENT_CHARACTER ? -1 : to_windows_1252(rune);
        if (code < 0) {
            throw Error(0, "watermark text must be Latin (Windows-1252) characters");
        }
        out += kHex[code >> 4];
        out += kHex[code & 15];
    }
    return out + ">";
}

void validate(const WatermarkOptions& o) {
    if (o.text.empty()) {
        throw Error(0, "watermark text is empty");
    }
    if (o.text.size() > 1000) {
        throw Error(0, "watermark text is too long");
    }
    if (!(o.opacity > 0 && o.opacity <= 1)) {
        throw Error(0, "watermark opacity must be above 0 and at most 1");
    }
    if (!(std::isfinite(o.font_size) && o.font_size >= 0 && o.font_size <= 10000)) {
        throw Error(0, "watermark font size must be 0 (fit) or a positive size");
    }
    if (!std::isfinite(o.angle)) {
        throw Error(0, "watermark angle must be a finite number");
    }
    for (const float c : o.color) {
        if (!(c >= 0 && c <= 1)) {
            throw Error(0, "watermark colour components must be 0 to 1");
        }
    }
}

/// A resource name not yet used in `dict`.
void unused_name(fz_context* g, pdf_obj* dict, char* out, std::size_t size) {
    for (int i = 0;; ++i) {
        fz_snprintf(out, size, i == 0 ? "LehtMark" : "LehtMark%d", i);
        if (pdf_dict_gets(g, dict, out) == nullptr) {
            return;
        }
    }
}

/// Wraps the page's existing content between `q_ref` and `end_ref` (streams
/// holding "q" and "Q"), so whatever graphics state it leaves behind cannot
/// affect what is drawn after it, then adds `mark` before or after it.
void add_content(fz_context* g, pdf_obj* page_obj, pdf_obj* q_ref, pdf_obj* end_ref,
                 pdf_obj* mark, bool under) {
    pdf_obj* contents = pdf_dict_get(g, page_obj, PDF_NAME(Contents));
    pdf_obj* list = contents;
    if (!pdf_is_array(g, contents)) {
        pdf_obj* old = pdf_keep_obj(g, contents);
        list = pdf_dict_put_array(g, page_obj, PDF_NAME(Contents), 4);
        if (old != nullptr) {
            pdf_array_push_drop(g, list, old);
        }
    }
    pdf_array_insert(g, list, q_ref, 0);
    pdf_array_push(g, list, end_ref);
    if (under) {
        pdf_array_insert(g, list, mark, 0);
    } else {
        pdf_array_push(g, list, mark);
    }
}

/// The page's own /Resources, created (as a copy, if inherited) when absent.
pdf_obj* own_resources(fz_context* g, pdf_obj* page_obj) {
    pdf_obj* res = pdf_dict_get(g, page_obj, PDF_NAME(Resources));
    if (res != nullptr) {
        return res;
    }
    pdf_obj* inherited = pdf_dict_get_inheritable(g, page_obj, PDF_NAME(Resources));
    if (inherited == nullptr) {
        return pdf_dict_put_dict(g, page_obj, PDF_NAME(Resources), 2);
    }
    pdf_dict_put_drop(g, page_obj, PDF_NAME(Resources), pdf_copy_dict(g, inherited));
    return pdf_dict_get(g, page_obj, PDF_NAME(Resources));
}

}  // namespace

int watermark(const Context& ctx, Document& doc, const std::string& pages,
              const WatermarkOptions& options) {
    validate(options);
    const std::string hex = encode_1252_hex(options.text);
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    const std::vector<int> selected = page_set(pages, doc.page_count());

    OwnedFont font{c};
    fz_matrix end{};  // the text matrix after laying the string out at size 1
    const char* text = options.text.c_str();
    guarded(c, [&](fz_context* g) {
        *font.slot() = fz_new_base14_font(g, "Helvetica");
        end = fz_measure_string(g, font.get(), fz_identity, text, 0, 0, FZ_BIDI_LTR,
                                FZ_LANG_UNSET);
    });
    const float unit_width = end.e;
    if (!(unit_width > 0)) {
        throw Error(0, "watermark text has no visible width");
    }
    const float radians = options.angle * 3.14159265358979F / 180.0F;
    const float cos_a = std::cos(radians);
    const float sin_a = std::sin(radians);

    int marked = 0;
    for (const int index : selected) {
        detail::OwnedPage page = detail::load_pdf_page(c, doc, index);
        pdf_page* p = detail::as_pdf_page(c, page);

        fz_rect bounds{};
        fz_matrix ctm{};
        guarded(c, [&](fz_context* g) {
            bounds = fz_bound_page(g, page.get());
            pdf_page_transform(g, p, nullptr, &ctm);
        });
        const float w = bounds.x1 - bounds.x0;
        const float h = bounds.y1 - bounds.y0;

        float size = options.font_size;
        if (size == 0) {
            // The longest line through the centre at this angle, 80% of it.
            const float half_w = std::abs(cos_a) > 1e-4F ? w / 2 / std::abs(cos_a) : 1e9F;
            const float half_h = std::abs(sin_a) > 1e-4F ? h / 2 / std::abs(sin_a) : 1e9F;
            size = 0.8F * 2 * std::min(half_w, half_h) / unit_width;
            size = std::min(size, 0.5F * std::min(w, h));
        }
        const float text_w = unit_width * size;
        const float cx = (bounds.x0 + bounds.x1) / 2;
        const float cy = (bounds.y0 + bounds.y1) / 2;
        // The text matrix. Base coordinates run y-down, so the glyphs are
        // flipped upright, and the rotation is negated to turn
        // counter-clockwise as displayed.
        fz_matrix trm = fz_scale(size, -size);
        trm = fz_concat(trm, fz_translate(-text_w / 2, 0.35F * size));
        trm = fz_concat(trm, fz_rotate(-options.angle));
        trm = fz_concat(trm, fz_translate(cx, cy));

        // The mark's own content: text in base coordinates (the Form's /Matrix
        // maps them to the page), in a non-embedded Helvetica, with opacity
        // from an ExtGState. Written by hand rather than through MuPDF's PDF
        // device, which would embed a 30 KB font for a word.
        char ops[512];
        fz_snprintf(ops, sizeof(ops),
                    "/GS0 gs %g %g %g rg BT /F0 1 Tf %g %g %g %g %g %g Tm ",
                    static_cast<double>(options.color[0]),
                    static_cast<double>(options.color[1]),
                    static_cast<double>(options.color[2]), static_cast<double>(trm.a),
                    static_cast<double>(trm.b), static_cast<double>(trm.c),
                    static_cast<double>(trm.d), static_cast<double>(trm.e),
                    static_cast<double>(trm.f));
        const std::string content = ops + hex + " Tj ET\n";
        const char* content_ptr = content.c_str();
        const std::size_t content_len = content.size();

        detail::OwnedPdfObj res{c};
        detail::OwnedPdfObj xobj{c};
        detail::OwnedPdfObj mark{c};
        detail::OwnedBuffer form{c};
        detail::OwnedBuffer call{c};
        detail::OwnedBuffer q_buf{c};
        detail::OwnedBuffer end_buf{c};
        detail::OwnedPdfObj q_ref{c};
        detail::OwnedPdfObj end_ref{c};
        const float alpha = options.opacity;
        const bool under = options.under;
        guarded(c, [&](fz_context* g) {
            *res.slot() = pdf_new_dict(g, pdf, 2);
            pdf_obj* font_dict =
                pdf_dict_puts_dict(g, pdf_dict_put_dict(g, res.get(), PDF_NAME(Font), 1), "F0", 4);
            pdf_dict_put(g, font_dict, PDF_NAME(Type), PDF_NAME(Font));
            pdf_dict_put(g, font_dict, PDF_NAME(Subtype), PDF_NAME(Type1));
            pdf_dict_put_name(g, font_dict, PDF_NAME(BaseFont), "Helvetica");
            pdf_dict_put(g, font_dict, PDF_NAME(Encoding), PDF_NAME(WinAnsiEncoding));
            pdf_obj* gs = pdf_dict_puts_dict(
                g, pdf_dict_put_dict(g, res.get(), PDF_NAME(ExtGState), 1), "GS0", 2);
            pdf_dict_put_real(g, gs, PDF_NAME(ca), static_cast<double>(alpha));
            pdf_dict_put_real(g, gs, PDF_NAME(CA), static_cast<double>(alpha));

            *form.slot() = fz_new_buffer_from_copied_data(
                g, reinterpret_cast<const unsigned char*>(content_ptr), content_len);
            *xobj.slot() = pdf_new_xobject(g, pdf, bounds, fz_invert_matrix(ctm), res.get(),
                                           form.get());

            pdf_obj* xobjects = pdf_dict_get(g, own_resources(g, p->obj), PDF_NAME(XObject));
            if (xobjects == nullptr) {
                xobjects = pdf_dict_put_dict(g, own_resources(g, p->obj), PDF_NAME(XObject), 1);
            }
            char name[32];
            unused_name(g, xobjects, name, sizeof(name));
            pdf_dict_puts(g, xobjects, name, xobj.get());

            *call.slot() = fz_new_buffer(g, 64);
            fz_append_printf(g, call.get(), "q /%s Do Q\n", name);
            *mark.slot() = pdf_add_stream(g, pdf, call.get(), nullptr, 0);
            *q_buf.slot() = fz_new_buffer_from_copied_data(g, kSave, 2);
            *q_ref.slot() = pdf_add_stream(g, pdf, q_buf.get(), nullptr, 0);
            *end_buf.slot() = fz_new_buffer_from_copied_data(g, kRestore, 2);
            *end_ref.slot() = pdf_add_stream(g, pdf, end_buf.get(), nullptr, 0);
            add_content(g, p->obj, q_ref.get(), end_ref.get(), mark.get(), under);
        });
        ++marked;
    }
    return marked;
}

}  // namespace leht::ops
