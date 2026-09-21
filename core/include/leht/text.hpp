// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <memory>
#include <vector>

typedef struct fz_context fz_context;

namespace leht {

class Context;
class Document;

/// One glyph run's outline: four corners, matching PDF's text quads. Kept as a
/// quad rather than a rectangle because rotated or sheared text is not
/// axis-aligned, and a bounding box would over-cover it.
///
/// Coordinates are in the SAME pixel space as Renderer::render() at the zoom and
/// rotation this TextPage was built with, so a viewer draws them directly on the
/// rendered bitmap with no further transform. Origin top-left, y downwards.
struct TextQuad {
    float ul_x = 0, ul_y = 0;  ///< upper-left
    float ur_x = 0, ur_y = 0;  ///< upper-right
    float ll_x = 0, ll_y = 0;  ///< lower-left
    float lr_x = 0, lr_y = 0;  ///< lower-right

    /// Axis-aligned bounds, convenient for hit-testing and scroll-to.
    [[nodiscard]] float min_x() const;
    [[nodiscard]] float min_y() const;
    [[nodiscard]] float max_x() const;
    [[nodiscard]] float max_y() const;
};

/// One search match. A match that wraps across a line break is several quads —
/// one per line — so highlighting draws each without covering the gap between.
struct SearchHit {
    std::vector<TextQuad> quads;
};

/// A text selection: the quads to highlight, and the text they cover.
struct Selection {
    std::vector<TextQuad> quads;
    std::string text;
};

/// How a selection snaps to text boundaries.
enum class SelectMode {
    Chars,  ///< character-precise, for click-drag
    Words,  ///< whole words, for double-click-drag
    Lines,  ///< whole lines, for triple-click-drag
};

/// The extracted, positioned text of one page.
///
/// Built at a fixed zoom and rotation: all coordinates it returns are in that
/// rendered pixel space, so a viewer that rendered the page at the same zoom can
/// draw highlights and hit-test clicks against the pixels it already has. Build
/// a new TextPage when the zoom changes; extraction is far cheaper than
/// rendering, and the viewer can cache these the way it caches bitmaps.
///
/// Borrows its Context and Document, both of which must outlive it. NOT
/// thread-safe — one per thread, like the rest of core/ (see docs/threading.md).
class TextPage {
public:
    /// Builds the text layer for `page_index` at `zoom` and `rotation` (a
    /// multiple of 90). Throws leht::Error on failure.
    TextPage(const Context& ctx, Document& doc, int page_index,
             float zoom = 1.0F, int rotation = 0);

    ~TextPage();
    TextPage(TextPage&&) noexcept;
    TextPage& operator=(TextPage&&) noexcept;
    TextPage(const TextPage&) = delete;
    TextPage& operator=(const TextPage&) = delete;

    /// The whole page as plain text, reading order, lines separated by '\n'.
    [[nodiscard]] std::string text() const;

    /// Every occurrence of `needle`, up to `max_hits`. Case-insensitive, as
    /// MuPDF's search is. An empty needle returns nothing.
    [[nodiscard]] std::vector<SearchHit> search(const std::string& needle,
                                                std::size_t max_hits = 500) const;

    /// The selection between two points, in this page's pixel space. `mode`
    /// snaps the endpoints to character, word or line boundaries.
    [[nodiscard]] Selection select(float x0, float y0, float x1, float y1,
                                   SelectMode mode = SelectMode::Chars) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace leht
