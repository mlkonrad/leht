// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal header: adding drawing to a page without disturbing what is
// already there. Shared by the watermark and the OCR text layer. Every
// function here must be called inside a guarded() region.
#pragma once

#include "mupdf_c.hpp"

#include <cstddef>

namespace leht::ops::content {

inline const unsigned char kSave[] = {'q', '\n'};
inline const unsigned char kRestore[] = {'Q', '\n'};

/// A resource name not yet used in `dict`: `prefix`, or `prefix` and a number.
inline void unused_name(fz_context* g, pdf_obj* dict, const char* prefix, char* out,
                        std::size_t size) {
    for (int i = 0;; ++i) {
        if (i == 0) {
            fz_snprintf(out, size, "%s", prefix);
        } else {
            fz_snprintf(out, size, "%s%d", prefix, i);
        }
        if (pdf_dict_gets(g, dict, out) == nullptr) {
            return;
        }
    }
}

/// Wraps the page's existing content between `q_ref` and `end_ref` (streams
/// holding "q" and "Q"), so whatever graphics state it leaves behind cannot
/// affect what is drawn after it, then adds `mark` before or after it.
///
/// A single content stream is first replaced by an array holding it. That
/// array is created into `new_list`, an outer-frame slot, so it is released
/// even when a later call throws -- as one does when resolving /Contents
/// makes MuPDF repair a damaged file mid-edit.
inline void add_content(fz_context* g, pdf_document* pdf, pdf_obj* page_obj, pdf_obj** new_list,
                 pdf_obj* q_ref, pdf_obj* end_ref, pdf_obj* mark, bool under) {
    pdf_obj* contents = pdf_dict_get(g, page_obj, PDF_NAME(Contents));
    pdf_obj* list = contents;
    if (!pdf_is_array(g, contents)) {
        *new_list = pdf_new_array(g, pdf, 4);
        if (contents != nullptr) {
            pdf_array_push(g, *new_list, contents);
        }
        pdf_dict_put(g, page_obj, PDF_NAME(Contents), *new_list);
        list = *new_list;
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
inline pdf_obj* own_resources(fz_context* g, pdf_obj* page_obj) {
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

}  // namespace leht::ops::content
