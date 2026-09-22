// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/edit.hpp"

#include <string>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

/// Points to trim from each edge of a page, as the page is displayed.
struct Margins {
    float left = 0, top = 0, right = 0, bottom = 0;
};

/// Sets the visible area of each page in `pages` (a page-range spec; empty
/// means all) to `box`, in base coordinates of the page as it is displayed
/// now. The box is clipped to the page's media. Returns the pages changed.
///
/// CROPPING HIDES, IT DOES NOT REMOVE. It sets the page's /CropBox, and
/// everything outside it stays in the file, one click of "show full page"
/// away in most editors. To take content out, use redact().
///
/// Throws leht::Error if the document is not a PDF, or the box (after
/// clipping) encloses nothing on some page.
int crop(const Context& ctx, Document& doc, const std::string& pages, const Rect& box);

/// As crop(), trimming `margins` from each page's current visible edges, so
/// pages of different sizes each lose the same border.
int crop_margins(const Context& ctx, Document& doc, const std::string& pages,
                 const Margins& margins);

}  // namespace leht::ops
