// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/crop.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <cmath>
#include <string>

namespace leht::ops {

namespace {

bool finite(const Rect& r) {
    return std::isfinite(r.x0) && std::isfinite(r.y0) && std::isfinite(r.x1) &&
           std::isfinite(r.y1);
}

/// Crops each page in `pages` to `box_for(visible area)`, clipped to the
/// page's media. The callable gets and returns base coordinates.
template <typename BoxFor>
int crop_each(const Context& ctx, Document& doc, const std::string& pages, BoxFor box_for) {
    fz_context* c = ctx.raw();
    (void)detail::require_pdf(c, doc);

    int changed = 0;
    for (const int index : page_set(pages, doc.page_count())) {
        detail::OwnedPage page = detail::load_pdf_page(c, doc, index);
        pdf_page* p = detail::as_pdf_page(c, page);

        fz_rect visible{};
        fz_rect media{};
        guarded(c, [&](fz_context* g) {
            visible = fz_bound_page(g, page.get());
            fz_matrix ctm{};
            pdf_page_transform(g, p, nullptr, &ctm);
            media = fz_transform_rect(
                pdf_to_rect(g, pdf_dict_get_inheritable(g, p->obj, PDF_NAME(MediaBox))), ctm);
        });

        const Rect want = box_for(Rect{visible.x0, visible.y0, visible.x1, visible.y1});
        if (!finite(want)) {
            throw Error(0, "crop box must be finite numbers");
        }
        const fz_rect clipped =
            fz_intersect_rect(fz_make_rect(want.x0, want.y0, want.x1, want.y1), media);
        if (!(clipped.x1 > clipped.x0 && clipped.y1 > clipped.y0)) {
            throw Error(0, "crop box leaves nothing of page " + std::to_string(index + 1));
        }
        guarded(c, [&](fz_context* g) { pdf_set_page_box(g, p, FZ_CROP_BOX, clipped); });
        ++changed;
    }
    return changed;
}

}  // namespace

int crop(const Context& ctx, Document& doc, const std::string& pages, const Rect& box) {
    return crop_each(ctx, doc, pages, [&](const Rect&) { return box; });
}

int crop_margins(const Context& ctx, Document& doc, const std::string& pages,
                 const Margins& m) {
    if (!(m.left >= 0 && m.top >= 0 && m.right >= 0 && m.bottom >= 0)) {
        throw Error(0, "crop margins must be zero or more");
    }
    return crop_each(ctx, doc, pages, [&](const Rect& v) {
        return Rect{v.x0 + m.left, v.y0 + m.top, v.x1 - m.right, v.y1 - m.bottom};
    });
}

}  // namespace leht::ops
