// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <string>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

struct WatermarkOptions {
    /// The text to stamp. Drawn in the base-14 Helvetica, so no font is
    /// embedded and the file barely grows -- which also limits it to Latin
    /// text (Windows-1252); other characters are refused rather than drawn
    /// as boxes.
    std::string text;
    /// Font size in points. 0 fits the text across the page's diagonal.
    float font_size = 0;
    /// 0 (invisible) to 1 (opaque).
    float opacity = 0.15F;
    /// Counter-clockwise, in degrees, as the page is displayed.
    float angle = 45;
    /// RGB, each 0 to 1.
    float color[3] = {0.5F, 0.5F, 0.5F};
    /// Draw beneath the page content instead of over it. Under is subtler,
    /// but a page with an opaque background (any scan) hides it entirely.
    bool under = false;
};

/// Stamps `options.text` across each page in `pages` (a page-range spec;
/// empty means all), centred.
///
/// The mark becomes part of the page content -- a Form XObject drawn by a
/// new content stream -- not a Watermark annotation, which viewers let the
/// reader hide with one toggle. The page's existing content is wrapped in its
/// own graphics state first, so whatever state it leaves behind cannot skew
/// or recolour the mark. Returns the pages marked.
int watermark(const Context& ctx, Document& doc, const std::string& pages,
              const WatermarkOptions& options);

}  // namespace leht::ops
