// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/ocr_layer.hpp"

#include "edit_internal.hpp"
#include "glyphless_font.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/text.hpp"
#include "mupdf_c.hpp"
#include "page_content.hpp"

#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace leht::ops {

namespace {

/// Each character is this wide, in thousandths of the font size (/DW).
constexpr int kCharWidth = 500;
/// The resource name the layer's font has inside its form XObject.
constexpr const char* kFontName = "F0";
constexpr const char* kBaseFont = "GlyphLessFont";
constexpr const char* kXObjectPrefix = "LehtOCR";

/// ToUnicode for CIDs that are UTF-16 code units: each CID is its own code.
/// Written as 256 ranges, one per high byte, because a bfrange may only vary
/// in its last byte; at most 100 entries per block.
std::string identity_to_unicode() {
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    for (int block = 0; block < 256; block += 100) {
        const int n = std::min(100, 256 - block);
        char line[64];
        fz_snprintf(line, sizeof(line), "%d beginbfrange\n", n);
        cmap += line;
        for (int hi = block; hi < block + n; ++hi) {
            fz_snprintf(line, sizeof(line), "<%02X00> <%02XFF> <%02X00>\n", hi, hi, hi);
            cmap += line;
        }
        cmap += "endbfrange\n";
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    return cmap;
}

/// UTF-8 to UTF-16 code units, as hex for a PDF string. Returns the number of
/// units, each one CID of kCharWidth; control characters are dropped.
int utf16_hex(const std::string& utf8, std::string& out) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    const auto put = [&](unsigned unit) {
        for (int shift = 12; shift >= 0; shift -= 4) {
            out += kHex[(unit >> shift) & 15];
        }
    };
    int units = 0;
    for (const char* p = utf8.c_str(); *p != '\0';) {
        int rune = 0;
        p += fz_chartorune(&rune, p);
        if (rune < 0x20 || rune == 0x7F || (rune >= 0xD800 && rune <= 0xDFFF)) {
            continue;
        }
        if (rune > 0xFFFF) {
            const unsigned v = static_cast<unsigned>(rune) - 0x10000;
            put(0xD800 + (v >> 10));
            put(0xDC00 + (v & 0x3FF));
            units += 2;
        } else {
            put(static_cast<unsigned>(rune));
            units += 1;
        }
    }
    return units;
}

bool usable(const OcrWord& w) {
    const Rect& b = w.box;
    return !w.text.empty() && std::isfinite(b.x0) && std::isfinite(b.y0) &&
           std::isfinite(b.x1) && std::isfinite(b.y1) && !b.empty();
}

/// The glyphless Type0 font an earlier page's layer already added, or null.
pdf_obj* existing_font(fz_context* g, pdf_document* pdf) {
    const int pages = pdf_count_pages(g, pdf);
    for (int i = 0; i < pages; ++i) {
        pdf_obj* xobjects = pdf_dict_getp(g, pdf_lookup_page_obj(g, pdf, i), "Resources/XObject");
        const int n = pdf_dict_len(g, xobjects);
        for (int k = 0; k < n; ++k) {
            const char* key = pdf_to_name(g, pdf_dict_get_key(g, xobjects, k));
            if (std::strncmp(key, kXObjectPrefix, std::strlen(kXObjectPrefix)) != 0) {
                continue;
            }
            pdf_obj* font = pdf_dict_getp(g, pdf_dict_get_val(g, xobjects, k), "Resources/Font/F0");
            if (pdf_is_dict(g, font) &&
                std::strcmp(pdf_dict_get_name(g, font, PDF_NAME(BaseFont)), kBaseFont) == 0) {
                return font;
            }
        }
    }
    return nullptr;
}

}  // namespace

int add_text_layer(const Context& ctx, Document& doc, int page,
                   const std::vector<OcrWord>& words) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    if (page < 0 || page >= doc.page_count()) {
        throw Error(0, "page " + std::to_string(page + 1) + " is out of range");
    }

    // The words, in base coordinates (the form's /Matrix maps them onto the
    // page): invisible (3 Tr), sized to the box's height and stretched (Tz)
    // to its width. The text matrix flips y, so the glyphs stand upright in
    // the y-down base space. A space after each word, not counted in the
    // stretch, keeps words apart when the text is extracted.
    std::string ops = "BT 3 Tr\n";
    int written = 0;
    for (const OcrWord& w : words) {
        if (!usable(w)) {
            continue;
        }
        std::string hex;
        const int units = utf16_hex(w.text, hex);
        if (units == 0) {
            continue;
        }
        const double h = static_cast<double>(w.box.y1) - static_cast<double>(w.box.y0);
        const double width = static_cast<double>(w.box.x1) - static_cast<double>(w.box.x0);
        const double stretch = 100.0 * width / (units * h * kCharWidth / 1000.0);
        char line[160];
        fz_snprintf(line, sizeof(line), "/%s %g Tf %g Tz 1 0 0 -1 %g %g Tm <", kFontName,
                    h, stretch, static_cast<double>(w.box.x0),
                    static_cast<double>(w.box.y1));
        ops += line;
        ops += hex;
        ops += "0020> Tj\n";
        ++written;
    }
    ops += "ET\n";
    if (written == 0) {
        return 0;
    }

    const std::string cmap = identity_to_unicode();
    const char* ops_ptr = ops.data();
    const std::size_t ops_len = ops.size();
    const char* cmap_ptr = cmap.data();
    const std::size_t cmap_len = cmap.size();

    detail::OwnedPage loaded = detail::load_pdf_page(c, doc, page);
    pdf_page* p = detail::as_pdf_page(c, loaded);
    fz_rect bounds{};
    fz_matrix ctm{};
    guarded(c, [&](fz_context* g) {
        bounds = fz_bound_page(g, loaded.get());
        pdf_page_transform(g, p, nullptr, &ctm);
    });

    // Created into outer-frame slots: a MuPDF throw mid-way releases them.
    detail::OwnedBuffer font_buf{c};
    detail::OwnedBuffer map_buf{c};
    detail::OwnedBuffer cmap_buf{c};
    detail::OwnedBuffer form_buf{c};
    detail::OwnedBuffer call_buf{c};
    detail::OwnedBuffer q_buf{c};
    detail::OwnedBuffer end_buf{c};
    detail::OwnedPdfObj file_dict{c};
    detail::OwnedPdfObj file_ref{c};
    detail::OwnedPdfObj map_ref{c};
    detail::OwnedPdfObj cmap_ref{c};
    detail::OwnedPdfObj descriptor{c};
    detail::OwnedPdfObj cid_font{c};
    detail::OwnedPdfObj type0{c};
    detail::OwnedPdfObj res{c};
    detail::OwnedPdfObj xobj{c};
    detail::OwnedPdfObj call_ref{c};
    detail::OwnedPdfObj q_ref{c};
    detail::OwnedPdfObj end_ref{c};
    detail::OwnedPdfObj new_list{c};
    guarded(c, [&](fz_context* g) {
        pdf_obj* font = existing_font(g, pdf);
        if (font == nullptr) {
            // The embedded glyphless TrueType, as Tesseract's PDF renderer
            // writes it: every CID maps to glyph 1, which draws nothing.
            *font_buf.slot() = fz_new_buffer_from_copied_data(
                g, detail::kGlyphLessFont, sizeof(detail::kGlyphLessFont));
            *file_dict.slot() = pdf_new_dict(g, pdf, 1);
            pdf_dict_put_int(g, file_dict.get(), PDF_NAME(Length1),
                             static_cast<int64_t>(sizeof(detail::kGlyphLessFont)));
            *file_ref.slot() = pdf_add_stream(g, pdf, font_buf.get(), file_dict.get(), 0);

            *map_buf.slot() = fz_new_buffer(g, 2 << 16);
            for (int i = 0; i < (1 << 16); ++i) {
                fz_append_byte(g, map_buf.get(), 0);
                fz_append_byte(g, map_buf.get(), 1);
            }
            *map_ref.slot() = pdf_add_stream(g, pdf, map_buf.get(), nullptr, 0);

            *cmap_buf.slot() = fz_new_buffer_from_copied_data(
                g, reinterpret_cast<const unsigned char*>(cmap_ptr), cmap_len);
            *cmap_ref.slot() = pdf_add_stream(g, pdf, cmap_buf.get(), nullptr, 0);

            *descriptor.slot() = pdf_add_new_dict(g, pdf, 10);
            pdf_obj* d = descriptor.get();
            pdf_dict_put(g, d, PDF_NAME(Type), PDF_NAME(FontDescriptor));
            pdf_dict_put_name(g, d, PDF_NAME(FontName), kBaseFont);
            pdf_dict_put_int(g, d, PDF_NAME(Flags), 5);  // fixed pitch, symbolic
            pdf_dict_put_rect(g, d, PDF_NAME(FontBBox), fz_make_rect(0, 0, kCharWidth, 1000));
            pdf_dict_put_int(g, d, PDF_NAME(ItalicAngle), 0);
            pdf_dict_put_int(g, d, PDF_NAME(Ascent), 1000);
            pdf_dict_put_int(g, d, PDF_NAME(Descent), -1);  // must be negative
            pdf_dict_put_int(g, d, PDF_NAME(CapHeight), 1000);
            pdf_dict_put_int(g, d, PDF_NAME(StemV), 80);
            pdf_dict_put(g, d, PDF_NAME(FontFile2), file_ref.get());

            *cid_font.slot() = pdf_add_new_dict(g, pdf, 8);
            pdf_obj* f = cid_font.get();
            pdf_dict_put(g, f, PDF_NAME(Type), PDF_NAME(Font));
            pdf_dict_put(g, f, PDF_NAME(Subtype), PDF_NAME(CIDFontType2));
            pdf_dict_put_name(g, f, PDF_NAME(BaseFont), kBaseFont);
            pdf_obj* info = pdf_dict_put_dict(g, f, PDF_NAME(CIDSystemInfo), 3);
            pdf_dict_put_text_string(g, info, PDF_NAME(Registry), "Adobe");
            pdf_dict_put_text_string(g, info, PDF_NAME(Ordering), "Identity");
            pdf_dict_put_int(g, info, PDF_NAME(Supplement), 0);
            pdf_dict_put(g, f, PDF_NAME(FontDescriptor), descriptor.get());
            pdf_dict_put_int(g, f, PDF_NAME(DW), kCharWidth);
            pdf_dict_put(g, f, PDF_NAME(CIDToGIDMap), map_ref.get());

            *type0.slot() = pdf_add_new_dict(g, pdf, 6);
            pdf_obj* t = type0.get();
            pdf_dict_put(g, t, PDF_NAME(Type), PDF_NAME(Font));
            pdf_dict_put(g, t, PDF_NAME(Subtype), PDF_NAME(Type0));
            pdf_dict_put_name(g, t, PDF_NAME(BaseFont), kBaseFont);
            pdf_dict_put(g, t, PDF_NAME(Encoding), PDF_NAME(Identity_H));
            pdf_array_push(g, pdf_dict_put_array(g, t, PDF_NAME(DescendantFonts), 1),
                           cid_font.get());
            pdf_dict_put(g, t, PDF_NAME(ToUnicode), cmap_ref.get());
            font = t;
        }

        *res.slot() = pdf_new_dict(g, pdf, 1);
        pdf_dict_puts(g, pdf_dict_put_dict(g, res.get(), PDF_NAME(Font), 1), kFontName, font);
        *form_buf.slot() = fz_new_buffer_from_copied_data(
            g, reinterpret_cast<const unsigned char*>(ops_ptr), ops_len);
        *xobj.slot() =
            pdf_new_xobject(g, pdf, bounds, fz_invert_matrix(ctm), res.get(), form_buf.get());

        pdf_obj* resources = content::own_resources(g, p->obj);
        pdf_obj* xobjects = pdf_dict_get(g, resources, PDF_NAME(XObject));
        if (xobjects == nullptr) {
            xobjects = pdf_dict_put_dict(g, resources, PDF_NAME(XObject), 1);
        }
        char name[32];
        content::unused_name(g, xobjects, kXObjectPrefix, name, sizeof(name));
        pdf_dict_puts(g, xobjects, name, xobj.get());

        *call_buf.slot() = fz_new_buffer(g, 64);
        fz_append_printf(g, call_buf.get(), "q /%s Do Q\n", name);
        *call_ref.slot() = pdf_add_stream(g, pdf, call_buf.get(), nullptr, 0);
        *q_buf.slot() = fz_new_buffer_from_copied_data(g, content::kSave, 2);
        *q_ref.slot() = pdf_add_stream(g, pdf, q_buf.get(), nullptr, 0);
        *end_buf.slot() = fz_new_buffer_from_copied_data(g, content::kRestore, 2);
        *end_ref.slot() = pdf_add_stream(g, pdf, end_buf.get(), nullptr, 0);
        content::add_content(g, pdf, p->obj, new_list.slot(), q_ref.get(), end_ref.get(),
                             call_ref.get(), /*under=*/false);
    });
    return written;
}

bool page_has_text(const Context& ctx, Document& doc, int page, int min_chars) {
    const std::string text = TextPage(ctx, doc, page).text();
    int n = 0;
    for (const char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) == 0 && ++n >= min_chars) {
            return true;
        }
    }
    return false;
}

}  // namespace leht::ops
