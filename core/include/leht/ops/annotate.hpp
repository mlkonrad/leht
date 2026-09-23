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
    /// move_annotation() accepts it. Text markup follows the text under it,
    /// and links and form widgets belong to the document's structure, so
    /// those are not movable.
    bool movable = false;
    /// It can also be resized. A note's icon has a fixed size: movable only.
    bool resizable = false;
    /// Free text: the font size in points and the text colour (RGB, 0 to 1),
    /// so an editor can show it as it will look. 0 and black otherwise.
    float font_size = 0;
    float color[3] = {0, 0, 0};
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

/// Moves the annotation `id` so that its bounds (AnnotInfo::rect) become
/// `to`, in base coordinates. A move keeps the appearance exactly as it is,
/// and so does a resize, scaled to the new box -- a stamp's picture, or
/// another application's drawing, survives. The one exception is free text
/// that changes size: its appearance is generated again so the text reflows.
/// The geometry other readers redraw from (ink strokes, vertices, callout
/// line, popup) moves with it. Returns false if there is no such annotation;
/// throws leht::Error for one that is not movable (see AnnotInfo::movable),
/// a resize of one that is not resizable, or an empty or non-finite `to`.
bool move_annotation(const Context& ctx, Document& doc, AnnotId id, const Rect& to);

/// Replaces the text of a free-text annotation or a note, and draws free
/// text again with it. Returns false if there is no such annotation; throws
/// leht::Error for any other kind, or text over the length limit.
bool set_annotation_contents(const Context& ctx, Document& doc, AnnotId id,
                             const std::string& text);

/// Deletes the annotation `id`, with its popup. Returns false if there is no
/// such annotation (or it is a form field, which forms own).
bool delete_annotation(const Context& ctx, Document& doc, AnnotId id);

}  // namespace leht::ops
