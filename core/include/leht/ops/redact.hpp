// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/edit.hpp"

#include <string>
#include <vector>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

/// What happens to an image that a redaction box overlaps.
enum class RedactImages {
    Pixels,  ///< Black out only the pixels under the box. The default.
    Remove,  ///< Remove the whole image.
    Keep,    ///< Leave images untouched. Only for text-only redaction.
};

/// What happens to vector drawing (paths) a redaction box overlaps.
enum class RedactLineArt {
    RemoveIfCovered,  ///< Remove paths lying wholly inside a box. The default.
    RemoveIfTouched,  ///< Remove any path a box touches at all.
    Keep,
};

/// There is deliberately no option to keep text: text under a box is always
/// removed, since keeping it is exactly the failure redaction exists to
/// prevent. MuPDF's REMOVE_UNLESS_INVISIBLE image mode is not offered either;
/// MuPDF's own header says that it leaks.
struct RedactOptions {
    RedactImages images = RedactImages::Pixels;
    RedactLineArt line_art = RedactLineArt::RemoveIfCovered;
    /// Paint a black box where content was removed. Without it the area is
    /// left blank, which hides the fact that anything was there.
    bool black_boxes = true;
};

struct RedactResult {
    int areas = 0;                ///< redaction boxes applied, ours and pre-existing
    std::vector<int> pages;       ///< pages changed, 0-based, ascending
    int annotations_removed = 0;  ///< annotations and form fields a box overlapped
    bool structure_dropped = false;
    /// Signatures the document held before redacting. A redacted document is
    /// always rewritten in full, which drops the revisions they sign: every one
    /// of them is broken by the save.
    int signatures_invalidated = 0;

    /// redact_text() only: places where the needle still appears after
    /// redacting -- document metadata or bookmarks anywhere, and on the pages
    /// searched, annotations or form fields a box did not overlap, or text the
    /// redaction could not reach (inside a pattern or a Type 3 glyph). Empty
    /// means none was found.
    /// These are reported, not rewritten: deleting a bookmark or rewriting a
    /// title is the user's call.
    std::vector<std::string> remaining;
};

/// Removes everything under `areas` on `page` (0-based): the text, the image
/// pixels and the line art that the options select. Coordinates are base
/// coordinates (see Rect).
///
/// This is real removal. The content is taken out of the page's content
/// stream, not covered up. A box drawn over text that stays in the file is
/// the most common way redaction fails, and it is exactly what this avoids.
/// Beyond the page content, the op also:
///   * removes annotations and form fields whose area overlaps a box, and any
///     replies to them, since their text can repeat what was removed;
///   * removes the page's thumbnail image (/Thumb), a picture of the page as
///     it was before;
///   * drops the document's structure tree, whose /Alt and /ActualText
///     entries repeat page text. That costs tagged-PDF accessibility, which
///     is the price of the redaction being real;
///   * marks the document so that every later save garbage-collects, which
///     drops the original content stream and all earlier revisions.
///
/// Redaction annotations already on the page (marked, but not yet applied, by
/// another tool) are applied too.
///
/// Throws leht::Error if the document is not a PDF or `page` is out of range.
RedactResult redact(const Context& ctx, Document& doc, int page,
                    const std::vector<Rect>& areas,
                    const RedactOptions& options = {});

/// Finds every occurrence of `needle` (case-insensitive, as search is) on the
/// pages in `pages` (a page-range spec; empty means all), and redacts them as
/// redact() does. Afterwards it checks what is left, and fills
/// RedactResult::remaining with everywhere the needle can still be found.
RedactResult redact_text(const Context& ctx, Document& doc,
                         const std::string& needle,
                         const std::string& pages = "",
                         const RedactOptions& options = {});

}  // namespace leht::ops
