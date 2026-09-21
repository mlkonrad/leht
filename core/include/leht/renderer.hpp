// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace leht {

class Context;
class Document;

/// A rendered raster owned by leht. Deliberately a plain struct of plain types:
/// no MuPDF pixmap ever crosses the public API.
struct Bitmap {
    int width = 0;
    int height = 0;
    int stride = 0;                     ///< bytes per row, may exceed width*channels
    int channels = 0;                   ///< 3 = RGB, 4 = RGBA
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool empty() const noexcept { return pixels.empty(); }
};

/// Pixel dimensions a page would occupy at a given zoom and rotation.
struct PageSize {
    int width = 0;
    int height = 0;
};

/// A cancellation token for an in-flight render.
///
/// Pinned in memory -- neither copyable nor movable -- because Renderer hands
/// its address to MuPDF for the duration of a render.
///
/// One-shot per render: MuPDF's contract is that once abort has been set to 1
/// during a render, it must not be changed again. Call reset() only between
/// renders, never while one is running.
class Cancel {
public:
    Cancel();
    ~Cancel();
    Cancel(const Cancel&) = delete;
    Cancel& operator=(const Cancel&) = delete;
    Cancel(Cancel&&) = delete;
    Cancel& operator=(Cancel&&) = delete;

    /// Asks the in-flight render to stop. Safe to call from another thread.
    /// MuPDF checks this periodically, so the render stops soon, not instantly.
    void request() noexcept;

    [[nodiscard]] bool requested() const noexcept;

    /// Rearms the token. Only valid when no render is using it.
    void reset() noexcept;

    /// Rendering progress, incremented by MuPDF as the page is drawn.
    [[nodiscard]] int progress() const noexcept;

private:
    friend class Renderer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Renders pages of one Document, caching a display list per page.
///
/// The display list is the reason zoom feels instant: parsing a page is the
/// expensive half, and a cached list replays at any scale without re-parsing.
///
/// Borrows its Context and Document, both of which must outlive it.
///
/// NOT thread-safe, by design rather than omission. One Renderer belongs to one
/// thread. For parallel work give each thread its own independent Context,
/// Document and Renderer -- never a cloned Context. See docs/threading.md.
/// Writes a Bitmap to a PNG file. Throws leht::Error on failure.
void write_png(const Context& ctx, const Bitmap& bitmap,
               const std::string& path);

class Renderer {
public:
    /// `max_cached_lists` bounds how many pages keep a display list in memory.
    Renderer(const Context& ctx, Document& doc, std::size_t max_cached_lists = 32);
    ~Renderer();

    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Renders one page to RGB.
    ///
    /// Returns nullopt if `cancel` was triggered -- cancellation is ordinary
    /// control flow for a viewer, not an error. Genuine failures throw
    /// leht::Error.
    [[nodiscard]] std::optional<Bitmap> render(int page_index, float zoom,
                                               int rotation = 0,
                                               Cancel* cancel = nullptr);

    /// Pixel size of a page without rendering it. Reads only the page's
    /// dictionary, not its content, so sizing every page of a large document
    /// is cheap enough to do on open. Always equal to the size render()
    /// produces for the same zoom and rotation.
    [[nodiscard]] PageSize page_size(int page_index, float zoom,
                                     int rotation = 0);

    void clear_cache() noexcept;
    [[nodiscard]] std::size_t cached_list_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace leht
