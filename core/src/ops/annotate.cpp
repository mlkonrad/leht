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

/// Kinds that can be picked up and put somewhere else. Text markup stays with
/// its text; links, widgets, popups and redaction marks are structure.
bool movable_type(enum pdf_annot_type t) {
    switch (t) {
        case PDF_ANNOT_TEXT:
        case PDF_ANNOT_FREE_TEXT:
        case PDF_ANNOT_LINE:
        case PDF_ANNOT_SQUARE:
        case PDF_ANNOT_CIRCLE:
        case PDF_ANNOT_POLYGON:
        case PDF_ANNOT_POLY_LINE:
        case PDF_ANNOT_STAMP:
        case PDF_ANNOT_CARET:
        case PDF_ANNOT_INK:
        case PDF_ANNOT_FILE_ATTACHMENT:
        case PDF_ANNOT_SOUND:
            return true;
        default:
            return false;
    }
}

/// Of those, the ones that are an icon of a fixed size: movable, not resizable.
bool icon_type(enum pdf_annot_type t) {
    return t == PDF_ANNOT_TEXT || t == PDF_ANNOT_FILE_ATTACHMENT || t == PDF_ANNOT_SOUND;
}

/// Calls `f(page, annot)` for the annotation `id`; returns false when there is
/// none. The page stays loaded for the duration of the call.
template <typename F>
bool with_annot(fz_context* c, Document& doc, AnnotId id, F&& f) {
    if (id <= 0) {
        return false;
    }
    const int count = doc.page_count();
    for (int index = 0; index < count; ++index) {
        detail::OwnedPage loaded = detail::load_pdf_page(c, doc, index);
        pdf_page* p = detail::as_pdf_page(c, loaded);
        for (pdf_annot* a : page_annots(c, p)) {
            int num = 0;
            guarded(c, [&](fz_context* g) { num = pdf_to_num(g, pdf_annot_obj(g, a)); });
            if (num == id) {
                f(p, a);
                return true;
            }
        }
    }
    return false;
}

/// Applies `m` to a flat array of x, y pairs, in place.
void transform_pairs(fz_context* g, pdf_obj* array, fz_matrix m) {
    const int n = pdf_array_len(g, array);
    for (int i = 0; i + 1 < n; i += 2) {
        const fz_point p = fz_transform_point_xy(pdf_array_get_real(g, array, i),
                                                 pdf_array_get_real(g, array, i + 1), m);
        pdf_array_put_real(g, array, i, p.x);
        pdf_array_put_real(g, array, i + 1, p.y);
    }
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
            enum pdf_annot_type kind = PDF_ANNOT_UNKNOWN;
            float size = 0;
            int n = 0;
            float color[4] = {0, 0, 0, 0};
            guarded(c, [&](fz_context* g) {
                kind = pdf_annot_type(g, a);
                type = pdf_string_from_annot_type(g, kind);
                contents = pdf_annot_contents(g, a);
                if (pdf_annot_has_author(g, a)) {
                    author = pdf_annot_author(g, a);
                }
                r = pdf_bound_annot(g, a);
                num = pdf_to_num(g, pdf_annot_obj(g, a));
                if (kind == PDF_ANNOT_FREE_TEXT) {
                    const char* font = nullptr;
                    pdf_annot_default_appearance(g, a, &font, &size, &n, color);
                }
            });
            AnnotInfo info{num, index, or_empty(type), Rect{r.x0, r.y0, r.x1, r.y1},
                           or_empty(contents), or_empty(author)};
            info.movable = movable_type(kind);
            info.resizable = info.movable && !icon_type(kind);
            if (kind == PDF_ANNOT_FREE_TEXT) {
                info.font_size = size;
                if (n == 1) {
                    info.color[0] = info.color[1] = info.color[2] = color[0];
                } else if (n == 3) {
                    std::copy(color, color + 3, info.color);
                }
            }
            out.push_back(std::move(info));
        }
    }
    return out;
}

bool move_annotation(const Context& ctx, Document& doc, AnnotId id, const Rect& to) {
    fz_context* c = ctx.raw();
    (void)detail::require_pdf(c, doc);
    if (!finite(to.x0) || !finite(to.y0) || !finite(to.x1) || !finite(to.y1) || to.empty()) {
        throw Error(0, "an annotation can only be moved to a non-empty, finite rectangle");
    }
    return with_annot(c, doc, id, [&](pdf_page* p, pdf_annot* a) {
        enum pdf_annot_type kind = PDF_ANNOT_UNKNOWN;
        fz_rect from{};
        fz_matrix ctm = fz_identity;
        guarded(c, [&](fz_context* g) {
            kind = pdf_annot_type(g, a);
            from = pdf_bound_annot(g, a);
            pdf_page_transform(g, p, nullptr, &ctm);
        });
        if (!movable_type(kind)) {
            throw Error(0, kind == PDF_ANNOT_HIGHLIGHT || kind == PDF_ANNOT_UNDERLINE ||
                                   kind == PDF_ANNOT_STRIKE_OUT || kind == PDF_ANNOT_SQUIGGLY
                               ? "text markup follows the text under it and cannot be moved; "
                                 "delete it and mark the text again"
                               : "this kind of annotation cannot be moved");
        }
        const float fw = from.x1 - from.x0;
        const float fh = from.y1 - from.y0;
        const float tw = to.x1 - to.x0;
        const float th = to.y1 - to.y0;
        const bool resized = std::fabs(fw - tw) > 0.01F || std::fabs(fh - th) > 0.01F;
        if (resized && icon_type(kind)) {
            throw Error(0, "a note or attachment icon has a fixed size; it can only be moved");
        }
        // The map from the old bounds to the new, in base (page) space, then
        // carried into PDF user space, where the dictionary's numbers live.
        const float sx = fw > 0 ? tw / fw : 1;
        const float sy = fh > 0 ? th / fh : 1;
        const fz_matrix in_page = fz_make_matrix(sx, 0, 0, sy, to.x0 - from.x0 * sx,
                                                 to.y0 - from.y0 * sy);
        const fz_matrix m = fz_concat(fz_concat(ctm, in_page), fz_invert_matrix(ctm));
        const bool regenerate = resized && kind == PDF_ANNOT_FREE_TEXT;

        guarded(c, [&](fz_context* g) {
            pdf_obj* obj = pdf_annot_obj(g, a);
            const fz_rect old_rect = pdf_dict_get_rect(g, obj, PDF_NAME(Rect));
            pdf_dict_put_rect(g, obj, PDF_NAME(Rect), fz_transform_rect(old_rect, m));
            pdf_obj* ink = pdf_dict_get(g, obj, PDF_NAME(InkList));
            for (int i = 0; i < pdf_array_len(g, ink); ++i) {
                transform_pairs(g, pdf_array_get(g, ink, i), m);
            }
            transform_pairs(g, pdf_dict_get(g, obj, PDF_NAME(Vertices)), m);
            transform_pairs(g, pdf_dict_get(g, obj, PDF_NAME(L)), m);
            transform_pairs(g, pdf_dict_get(g, obj, PDF_NAME(CL)), m);
            // The popup window travels with its annotation but keeps its size.
            pdf_obj* popup = pdf_dict_get(g, obj, PDF_NAME(Popup));
            if (pdf_is_dict(g, popup)) {
                const fz_point anchor = fz_transform_point_xy(old_rect.x0, old_rect.y1, m);
                const fz_rect pr = pdf_dict_get_rect(g, popup, PDF_NAME(Rect));
                const float dx = anchor.x - old_rect.x0;
                const float dy = anchor.y - old_rect.y1;
                pdf_dict_put_rect(g, popup, PDF_NAME(Rect),
                                  fz_make_rect(pr.x0 + dx, pr.y0 + dy, pr.x1 + dx, pr.y1 + dy));
            }
            if (regenerate) {
                // Free text reflows into its new box. Rich text (/RC) is kept
                // only by the reader that wrote it; the plain /Contents is
                // what is drawn again.
                pdf_dirty_annot(g, a);
                pdf_update_annot(g, a);
            }
        });
    });
}

bool set_annotation_contents(const Context& ctx, Document& doc, AnnotId id,
                             const std::string& text) {
    fz_context* c = ctx.raw();
    (void)detail::require_pdf(c, doc);
    if (text.size() > kMaxText) {
        throw Error(0, "annotation text is too long");
    }
    const char* value = text.c_str();
    return with_annot(c, doc, id, [&](pdf_page*, pdf_annot* a) {
        enum pdf_annot_type kind = PDF_ANNOT_UNKNOWN;
        guarded(c, [&](fz_context* g) { kind = pdf_annot_type(g, a); });
        if (kind != PDF_ANNOT_FREE_TEXT && kind != PDF_ANNOT_TEXT) {
            throw Error(0, "only free text and notes have text to edit");
        }
        guarded(c, [&](fz_context* g) {
            // Rich text would still say the old words to readers that prefer it.
            pdf_dict_del(g, pdf_annot_obj(g, a), PDF_NAME(RC));
            pdf_set_annot_contents(g, a, value);
            pdf_update_annot(g, a);
        });
    });
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
