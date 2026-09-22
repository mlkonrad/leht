// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Plain types shared by the editing operations in ops/ (redact, crop,
// watermark, annotate, forms).
//
// Every edit acts on an already-open Document and leaves saving to a separate
// Document::save() call. That one shape serves both frontends: the CLI opens a
// path, edits and saves to another path; the sandboxed worker edits the
// document it already holds and saves into a descriptor the viewer passes it.
// Several edits also compose into one save.

#include <string>
#include <vector>

namespace leht {

/// A rectangle in BASE coordinates: points at zoom 1.0 and rotation 0, origin
/// at the page's top-left, y growing downwards. The same space as TextQuad and
/// the viewer's selection geometry, so a rectangle drawn on screen and divided
/// by the zoom passes through unchanged.
struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    /// True when the rectangle encloses no area (including when inverted).
    [[nodiscard]] bool empty() const { return !(x1 > x0 && y1 > y0); }
    [[nodiscard]] bool intersects(const Rect& o) const {
        return !empty() && !o.empty() && x0 < o.x1 && o.x0 < x1 && y0 < o.y1 &&
               o.y0 < y1;
    }
};

/// A point in base coordinates (see Rect).
struct Point {
    float x = 0, y = 0;
};

struct SaveOptions {
    /// Garbage collection: 0 none, 1 collect, 2 renumber, 3 de-duplicate.
    ///
    /// 1 by default because levels 2 and 3 renumber the objects of the OPEN
    /// document, not just of the file written -- which would change every
    /// ops::AnnotId the caller holds. Use them for a last save before closing
    /// (they make a smaller file). Raised to at least 1 when the document has
    /// been redacted -- see Document::save().
    int garbage = 1;
    bool compress_streams = true;
    /// Linearise for "fast web view". Costs a second pass.
    bool linearize = false;
};

/// Resolves a page-range spec (as for ops::parse_page_ranges) into a sorted
/// list of distinct 0-based pages. An empty spec means every page. Edits apply
/// to a page once, however often a range names it.
[[nodiscard]] std::vector<int> page_set(const std::string& spec, int page_count);

}  // namespace leht
