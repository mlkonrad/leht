// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/renderer.hpp"

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "guards.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <list>
#include <unordered_map>
#include <utility>

namespace leht {

// ---------------------------------------------------------------------------
// Cleanup guards.
//
// These live in the OUTER frame, never inside a guarded() lambda. That is what
// makes them safe: MuPDF's longjmp lands in run_guarded(), which then throws a
// normal C++ exception, and ordinary unwinding runs these destructors. Putting
// them inside the lambda would let the longjmp skip them.
// ---------------------------------------------------------------------------
namespace {

/// 6400%, the ceiling most PDF viewers use. A whole US Letter page at this scale
/// is already ~2 billion pixels, which MuPDF refuses anyway; the limit exists to
/// fail early with a clear message rather than deep inside the allocator.
constexpr float kMaxZoom = 64.0F;

struct PixmapGuard {
    fz_context* ctx = nullptr;
    fz_pixmap* pix = nullptr;
    PixmapGuard() = default;
    PixmapGuard(const PixmapGuard&) = delete;
    PixmapGuard& operator=(const PixmapGuard&) = delete;
    ~PixmapGuard() {
        if (pix != nullptr) {
            fz_drop_pixmap(ctx, pix);
        }
    }
};

struct DeviceGuard {
    fz_context* ctx = nullptr;
    fz_device* dev = nullptr;
    DeviceGuard() = default;
    DeviceGuard(const DeviceGuard&) = delete;
    DeviceGuard& operator=(const DeviceGuard&) = delete;
    ~DeviceGuard() {
        if (dev != nullptr) {
            fz_drop_device(ctx, dev);
        }
    }
};

/// Rejects zoom values MuPDF cannot be trusted with.
///
/// NaN is the important case. fz_round_rect clamps coordinates with ordinary
/// comparisons, and every comparison with NaN is false, so NaN passes straight
/// through the clamp into a float-to-int conversion -- undefined behaviour in
/// C. Infinity is clamped correctly but produces a meaningless error far from
/// the cause. Both are caller mistakes, so they are rejected here, at the API
/// boundary, with a message that names the actual problem.
/// std::to_string prints floats as fixed-point with six decimals, which turns
/// 1e30 into a 31-digit number in an error message. %g is compact and exact
/// enough for a human. The format is a literal, so this is not the kind of
/// snprintf that split() got wrong.
std::string format_zoom(float zoom) {
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%g", static_cast<double>(zoom));
    return buf.data();
}

void validate_zoom(float zoom) {
    if (!std::isfinite(zoom)) {
        throw Error(0, "zoom must be a finite number");
    }
    if (zoom <= 0.0F) {
        throw Error(0, "zoom must be greater than zero, got " +
                           format_zoom(zoom));
    }
    if (zoom > kMaxZoom) {
        throw Error(0, "zoom " + format_zoom(zoom) + " exceeds the maximum of " +
                           format_zoom(kMaxZoom));
    }
}

fz_matrix transform_for(float zoom, int rotation) {
    return fz_pre_rotate(fz_scale(zoom, zoom), static_cast<float>(rotation));
}

}  // namespace

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------
struct Cancel::Impl {
    // fz_cookie is an anonymous struct typedef, so it cannot be forward
    // declared. That is why Cancel is pimpl'd rather than holding a pointer.
    fz_cookie cookie{};
};

Cancel::Cancel() : impl_(std::make_unique<Impl>()) {}
Cancel::~Cancel() = default;

void Cancel::request() noexcept { impl_->cookie.abort = 1; }
bool Cancel::requested() const noexcept { return impl_->cookie.abort != 0; }
void Cancel::reset() noexcept { impl_->cookie = fz_cookie{}; }
int Cancel::progress() const noexcept { return impl_->cookie.progress; }

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
struct Renderer::Impl {
    fz_context* ctx = nullptr;  // borrowed
    fz_document* doc = nullptr;  // borrowed
    std::size_t max_lists = 32;

    /// Display-list cache, most-recently-used at the front.
    std::list<int> lru;
    std::unordered_map<int, std::pair<fz_display_list*, std::list<int>::iterator>>
        cache;

    ~Impl() { clear(); }

    void clear() noexcept {
        for (auto& [page, entry] : cache) {
            (void)page;
            if (entry.first != nullptr) {
                fz_drop_display_list(ctx, entry.first);
            }
        }
        cache.clear();
        lru.clear();
    }

    /// Returns a borrowed display list for `page`, building and caching it on
    /// first use. Parsing is the expensive half of rendering, so a cached list
    /// is what makes re-rendering at a new zoom cheap.
    fz_display_list* list_for(int page) {
        if (const auto it = cache.find(page); it != cache.end()) {
            lru.splice(lru.begin(), lru, it->second.second);  // touch
            return it->second.first;
        }

        fz_display_list* built = nullptr;
        fz_document* d = doc;
        guarded(ctx, [&](fz_context* g) {
            built = fz_new_display_list_from_page_number(g, d, page);
        });
        if (built == nullptr) {
            throw Error(0, "failed to build display list for page " +
                               std::to_string(page));
        }

        lru.push_front(page);
        cache.emplace(page, std::pair{built, lru.begin()});
        evict();
        return built;
    }

    void evict() noexcept {
        while (cache.size() > max_lists && !lru.empty()) {
            const int victim = lru.back();
            lru.pop_back();
            if (const auto it = cache.find(victim); it != cache.end()) {
                if (it->second.first != nullptr) {
                    fz_drop_display_list(ctx, it->second.first);
                }
                cache.erase(it);
            }
        }
    }
};

Renderer::Renderer(const Context& ctx, Document& doc,
                   std::size_t max_cached_lists)
    : impl_(std::make_unique<Impl>()) {
    if (ctx.raw() == nullptr) {
        throw Error(0, "cannot build a Renderer on a moved-from Context");
    }
    impl_->ctx = ctx.raw();
    impl_->doc = doc.raw();
    impl_->max_lists = max_cached_lists == 0 ? 1 : max_cached_lists;
}

Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

PageSize Renderer::page_size(int page_index, float zoom, int rotation) {
    validate_zoom(zoom);
    fz_display_list* list = impl_->list_for(page_index);
    const fz_matrix ctm = transform_for(zoom, rotation);

    fz_irect bbox{};
    guarded(impl_->ctx, [&](fz_context* g) {
        const fz_rect bounds = fz_bound_display_list(g, list);
        bbox = fz_round_rect(fz_transform_rect(bounds, ctm));
    });
    return PageSize{bbox.x1 - bbox.x0, bbox.y1 - bbox.y0};
}

std::optional<Bitmap> Renderer::render(int page_index, float zoom, int rotation,
                                       Cancel* cancel) {
    validate_zoom(zoom);
    fz_context* ctx = impl_->ctx;
    fz_display_list* list = impl_->list_for(page_index);
    const fz_matrix ctm = transform_for(zoom, rotation);

    // Reaches into Cancel's pimpl; Renderer is a friend. Null when the caller
    // does not want cancellation, which MuPDF accepts.
    fz_cookie* cookie = cancel != nullptr ? &cancel->impl_->cookie : nullptr;

    PixmapGuard pixmap;
    DeviceGuard device;
    pixmap.ctx = ctx;
    device.ctx = ctx;

    guarded(ctx, [&](fz_context* g) {
        const fz_rect bounds = fz_bound_display_list(g, list);
        const fz_irect bbox = fz_round_rect(fz_transform_rect(bounds, ctm));

        pixmap.pix =
            fz_new_pixmap_with_bbox(g, fz_device_rgb(g), bbox, nullptr, 0);
        fz_clear_pixmap_with_value(g, pixmap.pix, 0xFF);  // white page

        device.dev = fz_new_draw_device(g, ctm, pixmap.pix);
        fz_run_display_list(g, list, device.dev, fz_identity, fz_infinite_rect,
                            cookie);
        fz_close_device(g, device.dev);
    });

    // An aborted run stops early without throwing, so the cookie is the only
    // way to tell a cancelled render from a finished one.
    if (cookie != nullptr && cookie->abort != 0) {
        return std::nullopt;
    }

    fz_pixmap* pix = pixmap.pix;
    Bitmap out;
    out.width = fz_pixmap_width(ctx, pix);
    out.height = fz_pixmap_height(ctx, pix);
    out.stride = static_cast<int>(fz_pixmap_stride(ctx, pix));
    out.channels = fz_pixmap_components(ctx, pix);

    const std::size_t bytes =
        static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height);
    out.pixels.resize(bytes);

    // A degenerate page -- zoom of zero, or an empty box -- yields a pixmap
    // with no samples. memcpy with a null pointer is undefined even when the
    // length is zero, which UBSan correctly flags, so guard rather than rely on
    // it being harmless in practice.
    if (bytes > 0) {
        const unsigned char* samples = fz_pixmap_samples(ctx, pix);
        if (samples == nullptr) {
            throw Error(0, "MuPDF returned a pixmap with no sample data");
        }
        std::memcpy(out.pixels.data(), samples, bytes);
    }

    return out;
}

void write_png(const Context& ctx, const Bitmap& bitmap,
               const std::string& path) {
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot write a PNG with a moved-from Context");
    }
    if (bitmap.empty() || bitmap.channels < 3) {
        throw Error(0, "refusing to write an empty or non-RGB bitmap");
    }

    fz_pixmap* pix = nullptr;
    const int w = bitmap.width;
    const int h = bitmap.height;
    guarded(c, [&](fz_context* g) {
        pix = fz_new_pixmap(g, fz_device_rgb(g), w, h, nullptr, 0);
    });
    detail::Owned<fz_pixmap, fz_drop_pixmap> guard{c, pix};
    if (pix == nullptr) {
        throw Error(0, "could not allocate a pixmap for: " + path);
    }

    // Copy row by row: the source stride need not match the destination's.
    const auto dest_stride = static_cast<std::size_t>(fz_pixmap_stride(c, pix));
    const auto src_stride = static_cast<std::size_t>(bitmap.stride);
    const std::size_t row_bytes =
        std::min(dest_stride, static_cast<std::size_t>(w) * 3);
    unsigned char* dest = fz_pixmap_samples(c, pix);
    for (int y = 0; y < h; ++y) {
        std::memcpy(dest + static_cast<std::size_t>(y) * dest_stride,
                    bitmap.pixels.data() + static_cast<std::size_t>(y) * src_stride,
                    row_bytes);
    }

    const char* out = path.c_str();
    guarded(c, [&](fz_context* g) { fz_save_pixmap_as_png(g, pix, out); });
}

void Renderer::clear_cache() noexcept { impl_->clear(); }

std::size_t Renderer::cached_list_count() const noexcept {
    return impl_->cache.size();
}

}  // namespace leht
