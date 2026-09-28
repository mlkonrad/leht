// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Organising the pages of an open document: turn, delete, move, insert. These
// are the in-place counterparts of ops/pages.hpp (which writes a new file):
// the viewer's Pages mode applies them to the document the worker holds, as
// edits in its log, so undo and a respawned worker replay them like any other.
//
// Page numbers are 0-based; `pages` is a range spec as for page_set(). None of
// these is a redaction: a deleted page's objects stay in an incremental save,
// and go only with a full rewrite.

#include <string>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

/// Turns each page in `pages` by `degrees` (a multiple of 90, may be
/// negative), on top of its current /Rotate. Returns the pages turned.
int rotate_pages(const Context& ctx, Document& doc, const std::string& pages, int degrees);

/// Removes the pages in `pages`. Outline entries and links that pointed at
/// them go too. Refuses to remove every page: a PDF must have one. Returns the
/// pages removed.
int delete_pages(const Context& ctx, Document& doc, const std::string& pages);

/// Moves the pages in `pages`, kept in their order, to stand just before the
/// page now at `before` (0..page_count; page_count means the end) -- the drop
/// position of a drag in a page grid. Returns the pages moved.
int move_pages(const Context& ctx, Document& doc, const std::string& pages, int before);

/// Copies `pages` of `source` into `doc` before page `at` (0..page_count),
/// with what they use (fonts, images) copied once. `source` is only read.
/// Returns the pages inserted.
int insert_pages(const Context& ctx, Document& doc, int at, Document& source,
                 const std::string& pages);

/// Inserts an empty page of `width` x `height` points before page `at`.
void insert_blank_page(const Context& ctx, Document& doc, int at, float width, float height);

}  // namespace leht::ops
