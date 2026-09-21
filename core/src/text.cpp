// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/text.hpp"

#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace leht {

namespace {

fz_matrix transform_for(float zoom, int rotation) {
    return fz_pre_rotate(fz_scale(zoom, zoom), static_cast<float>(rotation));
}

TextQuad from_fz(const fz_quad& q) {
    return TextQuad{q.ul.x, q.ul.y, q.ur.x, q.ur.y,
                    q.ll.x, q.ll.y, q.lr.x, q.lr.y};
}

int select_mode_code(SelectMode mode) {
    switch (mode) {
        case SelectMode::Chars: return FZ_SELECT_CHARS;
        case SelectMode::Words: return FZ_SELECT_WORDS;
        case SelectMode::Lines: return FZ_SELECT_LINES;
    }
    return FZ_SELECT_CHARS;
}

}  // namespace

float TextQuad::min_x() const { return std::min({ul_x, ur_x, ll_x, lr_x}); }
float TextQuad::min_y() const { return std::min({ul_y, ur_y, ll_y, lr_y}); }
float TextQuad::max_x() const { return std::max({ul_x, ur_x, ll_x, lr_x}); }
float TextQuad::max_y() const { return std::max({ul_y, ur_y, ll_y, lr_y}); }

struct TextPage::Impl {
    fz_context* ctx = nullptr;    // borrowed
    fz_stext_page* page = nullptr;

    ~Impl() {
        if (page != nullptr) {
            fz_drop_stext_page(ctx, page);
        }
    }
};

TextPage::TextPage(const Context& ctx, Document& doc, int page_index,
                   float zoom, int rotation)
    : impl_(std::make_unique<Impl>()) {
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot build a TextPage on a moved-from Context");
    }
    if (!std::isfinite(zoom) || zoom <= 0.0F) {
        throw Error(0, "TextPage zoom must be a finite positive number");
    }
    impl_->ctx = c;

    fz_document* raw_doc = doc.raw();
    const fz_matrix ctm = transform_for(zoom, rotation);

    // Guards live in this outer frame; a MuPDF longjmp unwinds to run_guarded,
    // which throws, and these destructors then run. Never inside the lambda.
    detail::Owned<fz_page, fz_drop_page> page{c};
    detail::Owned<fz_device, fz_drop_device> device{c};
    fz_stext_page* stext = nullptr;

    guarded(c, [&](fz_context* g) {
        *page.slot() = fz_load_page(g, raw_doc, page_index);

        // Build the text page over the TRANSFORMED bounds, then run the page
        // through the stext device with the same ctm, so every glyph quad comes
        // out in the rendered pixel space rather than in unscaled PDF points.
        const fz_rect bounds = fz_transform_rect(fz_bound_page(g, page.get()), ctm);

        fz_stext_options opts{};  // defaults: reading order, no images
        stext = fz_new_stext_page(g, bounds);
        // Assign into impl_ only after allocation succeeds; if the device or run
        // below throws, the page is already owned by impl_ and gets dropped.
        this->impl_->page = stext;

        *device.slot() = fz_new_stext_device(g, stext, &opts);
        fz_run_page(g, page.get(), device.get(), ctm, nullptr);
        fz_close_device(g, device.get());
    });
}

TextPage::~TextPage() = default;
TextPage::TextPage(TextPage&&) noexcept = default;
TextPage& TextPage::operator=(TextPage&&) noexcept = default;

std::string TextPage::text() const {
    detail::OwnedBuffer buffer{impl_->ctx};
    fz_stext_page* page = impl_->page;
    guarded(impl_->ctx, [&](fz_context* g) {
        *buffer.slot() = fz_new_buffer_from_stext_page(g, page);
    });
    if (!buffer) {
        return {};
    }

    const char* data = nullptr;
    std::size_t len = 0;
    fz_buffer* buf = buffer.get();
    guarded(impl_->ctx, [&](fz_context* g) {
        len = fz_buffer_storage(g, buf, reinterpret_cast<unsigned char**>(
                                            const_cast<char**>(&data)));
    });
    return std::string(data, len);
}

std::vector<SearchHit> TextPage::search(const std::string& needle,
                                        std::size_t max_hits) const {
    if (needle.empty() || max_hits == 0) {
        return {};
    }

    // fz_search_stext_page fills a flat array of quads and marks, with hit_mark
    // non-zero on the first quad of each match. Ask for a few quads per hit so a
    // line-wrapped match is not truncated mid-hit.
    const int cap = static_cast<int>(std::min<std::size_t>(max_hits * 4, 8192));
    std::vector<fz_quad> quads(static_cast<std::size_t>(cap));
    std::vector<int> marks(static_cast<std::size_t>(cap));

    fz_stext_page* page = impl_->page;
    const char* c_needle = needle.c_str();
    fz_quad* quad_ptr = quads.data();
    int* mark_ptr = marks.data();

    int count = 0;
    guarded(impl_->ctx, [&](fz_context* g) {
        count = fz_search_stext_page(g, page, c_needle, mark_ptr, quad_ptr, cap);
    });
    if (count <= 0) {
        return {};
    }

    // Group runs of quads into hits: a new hit begins at each non-zero mark
    // (and always at index 0).
    std::vector<SearchHit> hits;
    for (int i = 0; i < count; ++i) {
        if (i == 0 || marks[static_cast<std::size_t>(i)] != 0) {
            if (hits.size() >= max_hits) {
                break;
            }
            hits.emplace_back();
        }
        hits.back().quads.push_back(from_fz(quads[static_cast<std::size_t>(i)]));
    }
    return hits;
}

Selection TextPage::select(float x0, float y0, float x1, float y1,
                           SelectMode mode) const {
    Selection result;

    fz_point a = fz_make_point(x0, y0);
    fz_point b = fz_make_point(x1, y1);
    const int snap = select_mode_code(mode);
    fz_stext_page* page = impl_->ctx != nullptr ? impl_->page : nullptr;

    // Snapping adjusts the endpoints to char/word/line boundaries in place.
    guarded(impl_->ctx, [&](fz_context* g) {
        fz_snap_selection(g, page, &a, &b, snap);
    });

    // Highlight quads: probe for the count, then fetch. fz_highlight_selection
    // returns how many quads the selection covers, capped at max_quads.
    constexpr int kMaxQuads = 4096;
    std::vector<fz_quad> quads(kMaxQuads);
    fz_quad* quad_ptr = quads.data();
    int n = 0;
    guarded(impl_->ctx, [&](fz_context* g) {
        n = fz_highlight_selection(g, page, a, b, quad_ptr, kMaxQuads);
    });
    result.quads.reserve(static_cast<std::size_t>(std::max(0, n)));
    for (int i = 0; i < n; ++i) {
        result.quads.push_back(from_fz(quads[static_cast<std::size_t>(i)]));
    }

    // The covered text. fz_copy_selection returns a malloc'd UTF-8 string that
    // we must fz_free; copy it into std::string first.
    char* text = nullptr;
    guarded(impl_->ctx, [&](fz_context* g) {
        text = fz_copy_selection(g, page, a, b, /*crlf=*/0);
    });
    if (text != nullptr) {
        result.text = text;
        fz_free(impl_->ctx, text);
    }
    return result;
}

}  // namespace leht
