// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/annotate.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace leht::ops {

namespace {

using OwnedAnnot = detail::Owned<pdf_annot, pdf_drop_annot>;

constexpr const char* kStamps[] = {
    "Approved",     "AsIs",    "Confidential", "Departmental",        "Draft",
    "Experimental", "Expired", "Final",        "ForComment",          "ForPublicRelease",
    "NotApproved",  "NotForPublicRelease",     "Sold",                "TopSecret"};

/// Longest text an annotation may carry. Far above any real note; it only
/// stops one call from building a gigabyte string into the document.
constexpr std::size_t kMaxText = std::size_t{1} << 20;

bool is_markup(AnnotKind k) {
    return k == AnnotKind::Highlight || k == AnnotKind::Underline ||
           k == AnnotKind::StrikeOut || k == AnnotKind::Squiggly;
}

bool uses_rect(AnnotKind k) {
    return k == AnnotKind::FreeText || k == AnnotKind::Square || k == AnnotKind::Circle ||
           k == AnnotKind::Stamp;
}

enum pdf_annot_type to_mupdf(AnnotKind k) {
    switch (k) {
        case AnnotKind::Highlight: return PDF_ANNOT_HIGHLIGHT;
        case AnnotKind::Underline: return PDF_ANNOT_UNDERLINE;
        case AnnotKind::StrikeOut: return PDF_ANNOT_STRIKE_OUT;
        case AnnotKind::Squiggly:  return PDF_ANNOT_SQUIGGLY;
        case AnnotKind::Note:      return PDF_ANNOT_TEXT;
        case AnnotKind::FreeText:  return PDF_ANNOT_FREE_TEXT;
        case AnnotKind::Ink:       return PDF_ANNOT_INK;
        case AnnotKind::Square:    return PDF_ANNOT_SQUARE;
        case AnnotKind::Circle:    return PDF_ANNOT_CIRCLE;
        case AnnotKind::Stamp:     return PDF_ANNOT_STAMP;
    }
    throw Error(0, "unknown annotation kind");
}

bool finite(float v) { return std::isfinite(v); }

void validate(const AnnotSpec& s) {
    for (const float c : s.color) {
        if (!(c >= 0 && c <= 1)) {
            throw Error(0, "annotation colour components must be 0 to 1");
        }
    }
    if (!(s.opacity > 0 && s.opacity <= 1)) {
        throw Error(0, "annotation opacity must be above 0 and at most 1");
    }
    if (s.contents.size() > kMaxText || s.author.size() > kMaxText) {
        throw Error(0, "annotation text is too long");
    }
    if (is_markup(s.kind)) {
        if (s.quads.empty()) {
            throw Error(0, "a text markup annotation needs at least one quad");
        }
        for (const TextQuad& q : s.quads) {
            for (const float v : {q.ul_x, q.ul_y, q.ur_x, q.ur_y, q.ll_x, q.ll_y, q.lr_x, q.lr_y}) {
                if (!finite(v)) {
                    throw Error(0, "annotation quad must be finite numbers");
                }
            }
        }
    }
    if (uses_rect(s.kind) || s.kind == AnnotKind::Note) {
        const Rect& r = s.rect;
        if (!finite(r.x0) || !finite(r.y0) || !finite(r.x1) || !finite(r.y1)) {
            throw Error(0, "annotation rectangle must be finite numbers");
        }
        if (uses_rect(s.kind) && r.empty()) {
            throw Error(0, "annotation rectangle encloses nothing");
        }
    }
    if (s.kind == AnnotKind::Ink) {
        if (s.strokes.empty()) {
            throw Error(0, "an ink annotation needs at least one stroke");
        }
        for (const auto& stroke : s.strokes) {
            if (stroke.empty()) {
                throw Error(0, "an ink stroke needs at least one point");
            }
            for (const Point& p : stroke) {
                if (!finite(p.x) || !finite(p.y)) {
                    throw Error(0, "ink points must be finite numbers");
                }
            }
        }
    }
    if (s.kind == AnnotKind::Ink || s.kind == AnnotKind::Square || s.kind == AnnotKind::Circle) {
        if (!(s.line_width > 0 && s.line_width <= 100)) {
            throw Error(0, "annotation line width must be above 0 and at most 100");
        }
    }
    if (s.kind == AnnotKind::FreeText && !(s.font_size >= 1 && s.font_size <= 1000)) {
        throw Error(0, "free text size must be 1 to 1000 points");
    }
    if (s.kind == AnnotKind::Stamp &&
        std::find(std::begin(kStamps), std::end(kStamps), s.stamp) == std::end(kStamps)) {
        throw Error(0, "unknown stamp '" + s.stamp + "'");
    }
}

fz_rect to_fz(const Rect& r) { return fz_make_rect(r.x0, r.y0, r.x1, r.y1); }

std::string or_empty(const char* s) { return s != nullptr ? s : ""; }

/// Every non-Popup annotation on a loaded page, as raw pointers the page owns.
std::vector<pdf_annot*> page_annots(fz_context* ctx, pdf_page* page) {
    int count = 0;
    guarded(ctx, [&](fz_context* g) {
        for (pdf_annot* a = pdf_first_annot(g, page); a != nullptr; a = pdf_next_annot(g, a)) {
            ++count;
        }
    });
    std::vector<pdf_annot*> out(static_cast<std::size_t>(count), nullptr);
    pdf_annot** slots = out.data();
    int kept = 0;
    guarded(ctx, [&](fz_context* g) {
        for (pdf_annot* a = pdf_first_annot(g, page); a != nullptr && kept < count;
             a = pdf_next_annot(g, a)) {
            if (pdf_annot_type(g, a) != PDF_ANNOT_POPUP) {
                slots[kept++] = a;
            }
        }
    });
    out.resize(static_cast<std::size_t>(kept));
    return out;
}

}  // namespace

AnnotId add_annotation(const Context& ctx, Document& doc, int page, const AnnotSpec& spec) {
    fz_context* c = ctx.raw();
    (void)detail::require_pdf(c, doc);
    if (page < 0 || page >= doc.page_count()) {
        throw Error(0, "page " + std::to_string(page + 1) + " is out of range");
    }
    validate(spec);

    // Everything MuPDF needs, prepared as plain arrays before the guarded
    // region, which may not allocate C++ objects.
    std::vector<fz_quad> quads;
    for (const TextQuad& q : is_markup(spec.kind) ? spec.quads : std::vector<TextQuad>{}) {
        quads.push_back(fz_quad{{q.ul_x, q.ul_y}, {q.ur_x, q.ur_y}, {q.ll_x, q.ll_y},
                                {q.lr_x, q.lr_y}});
    }
    std::vector<std::vector<fz_point>> strokes;
    for (const auto& stroke : spec.kind == AnnotKind::Ink ? spec.strokes
                                                          : std::vector<std::vector<Point>>{}) {
        std::vector<fz_point> points;
        for (const Point& p : stroke) {
            points.push_back(fz_make_point(p.x, p.y));
        }
        strokes.push_back(std::move(points));
    }
    const enum pdf_annot_type type = to_mupdf(spec.kind);
    const AnnotKind kind = spec.kind;
    const fz_rect rect = kind == AnnotKind::Note
                             ? fz_make_rect(spec.rect.x0, spec.rect.y0, spec.rect.x0 + 20,
                                            spec.rect.y0 + 20)
                             : to_fz(spec.rect);
    const char* contents = spec.contents.empty() ? nullptr : spec.contents.c_str();
    const char* author = spec.author.empty() ? nullptr : spec.author.c_str();
    const char* stamp = spec.stamp.c_str();
    const float* color = spec.color;
    const float opacity = spec.opacity;
    const float font_size = spec.font_size;
    const float line_width = spec.line_width;
    const fz_quad* quad_ptr = quads.data();
    const int quad_count = static_cast<int>(quads.size());

    detail::OwnedPage loaded = detail::load_pdf_page(c, doc, page);
    pdf_page* p = detail::as_pdf_page(c, loaded);
    OwnedAnnot annot{c};
    guarded(c, [&](fz_context* g) {
        *annot.slot() = pdf_create_annot(g, p, type);
        pdf_annot* a = annot.get();
        if (quad_count > 0 && pdf_annot_has_quad_points(g, a)) {
            pdf_set_annot_quad_points(g, a, quad_count, quad_ptr);
        }
        if (kind != AnnotKind::Ink && pdf_annot_has_rect(g, a) &&
            (kind == AnnotKind::Note || !fz_is_empty_rect(rect))) {
            pdf_set_annot_rect(g, a, rect);
        }
        if (kind == AnnotKind::FreeText) {
            pdf_set_annot_default_appearance(g, a, "Helv", font_size, 3, color);
        } else {
            pdf_set_annot_color(g, a, 3, color);
        }
        if (kind == AnnotKind::Stamp) {
            pdf_set_annot_icon_name(g, a, stamp);
        }
        if (pdf_annot_has_border(g, a)) {
            pdf_set_annot_border_width(g, a, line_width);
        }
        pdf_set_annot_opacity(g, a, opacity);
        if (contents != nullptr) {
            pdf_set_annot_contents(g, a, contents);
        }
        if (author != nullptr && pdf_annot_has_author(g, a)) {
            pdf_set_annot_author(g, a, author);
        }
    });
    for (const auto& stroke : strokes) {
        const fz_point* pts = stroke.data();
        const int n = static_cast<int>(stroke.size());
        guarded(c, [&](fz_context* g) {
            pdf_add_annot_ink_list(g, annot.get(), n, const_cast<fz_point*>(pts));
        });
    }

    AnnotId id = 0;
    guarded(c, [&](fz_context* g) {
        pdf_update_annot(g, annot.get());
        id = pdf_to_num(g, pdf_annot_obj(g, annot.get()));
    });
    return id;
}

std::vector<AnnotId> mark_text(const Context& ctx, Document& doc, const std::string& needle,
                               const std::string& pages, const AnnotSpec& base) {
    if (!is_markup(base.kind)) {
        throw Error(0, "mark_text needs a text markup kind (highlight, underline, ...)");
    }
    if (needle.empty()) {
        throw Error(0, "nothing to mark: the search text is empty");
    }
    (void)detail::require_pdf(ctx.raw(), doc);
    std::vector<AnnotId> ids;
    for (const int index : page_set(pages, doc.page_count())) {
        std::vector<SearchHit> hits;
        {
            const TextPage text(ctx, doc, index);
            hits = text.search(needle, 100000);
        }
        for (const SearchHit& hit : hits) {
            AnnotSpec spec = base;
            spec.quads = hit.quads;
            ids.push_back(add_annotation(ctx, doc, index, spec));
        }
    }
    return ids;
}

std::vector<AnnotInfo> list_annotations(const Context& ctx, Document& doc) {
    fz_context* c = ctx.raw();
    (void)detail::require_pdf(c, doc);
    std::vector<AnnotInfo> out;
    const int count = doc.page_count();
    for (int index = 0; index < count; ++index) {
        detail::OwnedPage loaded = detail::load_pdf_page(c, doc, index);
        pdf_page* p = detail::as_pdf_page(c, loaded);
        for (pdf_annot* a : page_annots(c, p)) {
            const char* type = nullptr;
            const char* contents = nullptr;
            const char* author = nullptr;
            fz_rect r{};
            int num = 0;
            guarded(c, [&](fz_context* g) {
                type = pdf_string_from_annot_type(g, pdf_annot_type(g, a));
                contents = pdf_annot_contents(g, a);
                if (pdf_annot_has_author(g, a)) {
                    author = pdf_annot_author(g, a);
                }
                r = pdf_bound_annot(g, a);
                num = pdf_to_num(g, pdf_annot_obj(g, a));
            });
            out.push_back(AnnotInfo{num, index, or_empty(type), Rect{r.x0, r.y0, r.x1, r.y1},
                                    or_empty(contents), or_empty(author)});
        }
    }
    return out;
}

bool delete_annotation(const Context& ctx, Document& doc, AnnotId id) {
    fz_context* c = ctx.raw();
    (void)detail::require_pdf(c, doc);
    if (id <= 0) {
        return false;
    }
    const int count = doc.page_count();
    for (int index = 0; index < count; ++index) {
        detail::OwnedPage loaded = detail::load_pdf_page(c, doc, index);
        pdf_page* p = detail::as_pdf_page(c, loaded);
        for (pdf_annot* a : page_annots(c, p)) {
            bool deleted = false;
            guarded(c, [&](fz_context* g) {
                if (pdf_to_num(g, pdf_annot_obj(g, a)) == id) {
                    pdf_delete_annot(g, p, a);
                    deleted = true;
                }
            });
            if (deleted) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace leht::ops
