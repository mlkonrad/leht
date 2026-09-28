// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/organize.hpp"
#include "leht/ops/pages.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace leht::ops {

namespace {

/// Every page in its current place except `moving`, then `moving` put back at
/// the place `before` stood: the new order, as old indices.
std::vector<int> order_after_move(int count, const std::vector<int>& moving, int before) {
    const std::set<int> set(moving.begin(), moving.end());
    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(count));
    int insert_at = 0;
    for (int i = 0; i < count; ++i) {
        if (set.count(i) != 0) {
            continue;
        }
        if (i < before) {
            ++insert_at;
        }
        order.push_back(i);
    }
    order.insert(order.begin() + insert_at, moving.begin(), moving.end());
    return order;
}

/// Rebuilds the page tree as `order`. MuPDF fixes the outline and links, and
/// keeps the structure tree (tagged PDF): dropping it would cost a screen
/// reader the document's reading order for a page move.
void rearrange(fz_context* c, pdf_document* pdf, const std::vector<int>& order) {
    const int n = static_cast<int>(order.size());
    const int* pages = order.data();
    guarded(c, [&](fz_context* g) { pdf_rearrange_pages(g, pdf, n, pages, PDF_CLEAN_STRUCTURE_KEEP); });
}

void check_position(int at, int count, const char* what) {
    if (at < 0 || at > count) {
        throw Error(0, std::string(what) + " must be a page position from 0 to " + std::to_string(count));
    }
}

}  // namespace

int rotate_pages(const Context& ctx, Document& doc, const std::string& pages, int degrees) {
    if (degrees % 90 != 0) {
        throw Error(0, "pages turn by a multiple of 90 degrees");
    }
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    const std::vector<int> set = page_set(pages, doc.page_count());
    for (const int index : set) {
        guarded(c, [&](fz_context* g) {
            pdf_obj* page = pdf_lookup_page_obj(g, pdf, index);
            // Inherited from the page tree unless the page has its own; the
            // new value goes on the page, where it wins.
            const int current = pdf_to_int(g, pdf_dict_get_inheritable(g, page, PDF_NAME(Rotate)));
            int turned = (current + degrees) % 360;
            if (turned < 0) {
                turned += 360;
            }
            pdf_dict_put_int(g, page, PDF_NAME(Rotate), turned);
        });
    }
    return static_cast<int>(set.size());
}

int delete_pages(const Context& ctx, Document& doc, const std::string& pages) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    const int count = doc.page_count();
    const std::vector<int> gone = page_set(pages, count);
    if (static_cast<int>(gone.size()) >= count) {
        throw Error(0, "a document must keep at least one page");
    }
    const std::set<int> set(gone.begin(), gone.end());
    std::vector<int> keep;
    for (int i = 0; i < count; ++i) {
        if (set.count(i) == 0) {
            keep.push_back(i);
        }
    }
    rearrange(c, pdf, keep);
    return static_cast<int>(gone.size());
}

int move_pages(const Context& ctx, Document& doc, const std::string& pages, int before) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    const int count = doc.page_count();
    check_position(before, count, "the place to move pages to");
    const std::vector<int> moving = page_set(pages, count);
    const std::vector<int> order = order_after_move(count, moving, before);
    std::vector<int> identity(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        identity[static_cast<std::size_t>(i)] = i;
    }
    if (order != identity) {
        rearrange(c, pdf, order);
    }
    return static_cast<int>(moving.size());
}

int insert_pages(const Context& ctx, Document& doc, int at, Document& source, const std::string& pages) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    pdf_document* src = detail::require_pdf(c, source);
    check_position(at, doc.page_count(), "the place to insert pages");
    // In the order given, not page_set()'s sorted one: "3,1" inserts page 3
    // first. The same page twice is allowed, as for extract.
    const std::vector<int> chosen = pages.empty() ? page_set(pages, source.page_count())
                                                  : parse_page_ranges(pages, source.page_count());
    if (chosen.empty()) {
        return 0;
    }
    // One graft map for the batch, so a font or image the pages share is
    // copied once, not once per page.
    detail::Owned<pdf_graft_map, pdf_drop_graft_map> map{c};
    guarded(c, [&](fz_context* g) { *map.slot() = pdf_new_graft_map(g, pdf); });
    int where = at;
    for (const int page : chosen) {
        pdf_graft_map* m = map.get();
        guarded(c, [&](fz_context* g) { pdf_graft_mapped_page(g, m, where, src, page); });
        ++where;
    }
    return static_cast<int>(chosen.size());
}

void insert_blank_page(const Context& ctx, Document& doc, int at, float width, float height) {
    if (!(std::isfinite(width) && std::isfinite(height) && width >= 3 && height >= 3 && width <= 14400 &&
          height <= 14400)) {
        throw Error(0, "a page is from 3 to 14400 points on each side");
    }
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    check_position(at, doc.page_count(), "the place to insert a page");
    // Owned in this frame, per guards.hpp: released even if MuPDF throws.
    detail::OwnedPdfObj resources{c};
    detail::OwnedBuffer contents{c};
    detail::OwnedPdfObj page{c};
    guarded(c, [&](fz_context* g) {
        *resources.slot() = pdf_add_new_dict(g, pdf, 1);
        *contents.slot() = fz_new_buffer(g, 0);
        *page.slot() = pdf_add_page(g, pdf, fz_make_rect(0, 0, width, height), 0, resources.get(),
                                    contents.get());
        pdf_insert_page(g, pdf, at, page.get());
    });
}

}  // namespace leht::ops
