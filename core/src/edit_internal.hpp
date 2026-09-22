// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal header: the pieces every edit operation needs to reach MuPDF's PDF
// layer from a leht::Document.
#pragma once

#include "guards.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

namespace leht::detail {

using OwnedPage = Owned<fz_page, fz_drop_page>;

/// The PDF behind `doc`, or throws: every edit operation is PDF-only.
inline pdf_document* require_pdf(fz_context* ctx, const Document& doc) {
    if (ctx == nullptr || doc.raw() == nullptr) {
        throw Error(0, "cannot edit with a moved-from Context or Document");
    }
    fz_document* raw = doc.raw();
    pdf_document* pdf = nullptr;
    guarded(ctx, [&](fz_context* g) { pdf = pdf_document_from_fz_document(g, raw); });
    if (pdf == nullptr) {
        throw Error(0, "editing requires a PDF");
    }
    return pdf;
}

/// Loads page `index` (0-based). The caller checks the range.
inline OwnedPage load_pdf_page(fz_context* ctx, const Document& doc, int index) {
    OwnedPage page{ctx};
    fz_document* raw = doc.raw();
    guarded(ctx, [&](fz_context* g) { *page.slot() = fz_load_page(g, raw, index); });
    return page;
}

inline pdf_page* as_pdf_page(fz_context* ctx, const OwnedPage& page) {
    pdf_page* p = nullptr;
    fz_page* raw = page.get();
    guarded(ctx, [&](fz_context* g) { p = pdf_page_from_fz_page(g, raw); });
    if (p == nullptr) {
        throw Error(0, "editing requires a PDF page");
    }
    return p;
}

}  // namespace leht::detail
