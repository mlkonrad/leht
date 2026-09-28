// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/info.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <string>

namespace leht::ops {

bool is_editable_info(const std::string& key) {
    return key == "Title" || key == "Author" || key == "Subject" || key == "Keywords";
}

void set_info(const Context& ctx, Document& doc, const std::string& key, const std::string& value) {
    if (!is_editable_info(key)) {
        throw Error(0, "not a document property that can be set: " + key);
    }
    if (value.size() > 32768) {
        throw Error(0, "a document property is at most 32768 bytes");
    }
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    const char* k = key.c_str();
    const char* v = value.c_str();
    const bool remove = value.empty();
    guarded(c, [&](fz_context* g) {
        pdf_obj* trailer = pdf_trailer(g, pdf);
        pdf_obj* info = pdf_dict_get(g, trailer, PDF_NAME(Info));
        if (!pdf_is_dict(g, info)) {
            if (remove) {
                return;  // nothing to take away
            }
            // A new /Info, indirect as the spec prefers; put_drop releases
            // our reference whether or not the put succeeds.
            pdf_dict_put_drop(g, trailer, PDF_NAME(Info), pdf_add_new_dict(g, pdf, 4));
            info = pdf_dict_get(g, trailer, PDF_NAME(Info));
        }
        if (remove) {
            pdf_dict_dels(g, info, k);
        } else {
            // pdf_new_text_string picks PDFDocEncoding or UTF-16 as the text needs.
            pdf_dict_puts_drop(g, info, k, pdf_new_text_string(g, v));
        }
    });
}

}  // namespace leht::ops
