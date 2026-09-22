// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/edit.hpp"
#include "leht/text.hpp"

#include <string>
#include <vector>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

enum class AnnotKind {
    Highlight,  ///< text markup: uses `quads`
    Underline,
    StrikeOut,
    Squiggly,
    Note,      ///< a sticky note: an icon at `rect`'s top-left with `contents`
    FreeText,  ///< text drawn in `rect`, `font_size` points
    Ink,       ///< freehand: uses `strokes`
    Square,    ///< outline of `rect`
    Circle,    ///< ellipse inscribed in `rect`
    Stamp,     ///< a standard rubber stamp (`stamp`) filling `rect`
};

/// Everything needed to create one annotation. Geometry is in base
/// coordinates (see Rect); each kind reads only the fields it needs.
struct AnnotSpec {
    AnnotKind kind = AnnotKind::Highlight;
    std::vector<TextQuad> quads;             ///< text markup
    Rect rect;                               ///< note, free text, square, circle, stamp
    std::vector<std::vector<Point>> strokes; ///< ink
    std::string contents;  ///< the note's or free text's text; a comment on others
    std::string author;
    /// RGB, each 0 to 1. The default is highlighter yellow.
    float color[3] = {1.0F, 0.85F, 0.0F};
    float opacity = 1.0F;
    float font_size = 12;     ///< free text
    float line_width = 1.5F;  ///< ink, square, circle
    /// Stamp name: Approved, AsIs, Confidential, Departmental, Draft,
    /// Experimental, Expired, Final, ForComment, ForPublicRelease,
    /// NotApproved, NotForPublicRelease, Sold, TopSecret.
    std::string stamp = "Draft";
};

/// Identifies an annotation while its document stays open: its PDF object
/// number. Edits and ordinary saves leave it alone; only a save with
/// SaveOptions::garbage of 2 or more renumbers objects, and so changes it.
using AnnotId = int;

struct AnnotInfo {
    AnnotId id = 0;
    int page = 0;      ///< 0-based
    std::string type;  ///< the PDF subtype: "Highlight", "Text", "Link", ...
    Rect rect;
    std::string contents;
    std::string author;
};

/// Adds an annotation to `page` (0-based) and generates its appearance, so it
/// looks the same in every reader rather than depending on each one to draw
/// it. Returns its id. Throws leht::Error on a non-PDF, a bad page, or a
/// spec missing what its kind needs.
AnnotId add_annotation(const Context& ctx, Document& doc, int page, const AnnotSpec& spec);

/// Adds one text-markup annotation (`base.kind` must be Highlight, Underline,
/// StrikeOut or Squiggly) over each occurrence of `needle` on `pages` (a
/// page-range spec; empty means all). Returns the ids created.
std::vector<AnnotId> mark_text(const Context& ctx, Document& doc, const std::string& needle,
                               const std::string& pages, const AnnotSpec& base);

/// Every annotation in the document except form-field widgets (see forms)
/// and the Popup windows that belong to other annotations.
std::vector<AnnotInfo> list_annotations(const Context& ctx, Document& doc);

/// Deletes the annotation `id`, with its popup. Returns false if there is no
/// such annotation (or it is a form field, which forms own).
bool delete_annotation(const Context& ctx, Document& doc, AnnotId id);

}  // namespace leht::ops
