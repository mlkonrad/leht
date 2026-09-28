// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// A document's descriptive fields: its title, author, subject and keywords, in
// the trailer's /Info dictionary -- what a file manager, a search index and
// the title bar of most readers show.

#include <string>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

/// Whether `key` is one set_info() takes: "Title", "Author", "Subject" or
/// "Keywords". The rest of /Info (Creator, Producer, the dates) is the
/// software's record, not the author's, and is left alone.
[[nodiscard]] bool is_editable_info(const std::string& key);

/// Sets /Info's `key` to `value` (UTF-8), or removes it when `value` is empty.
/// Throws for a key is_editable_info() refuses, or a document that is not a
/// PDF.
void set_info(const Context& ctx, Document& doc, const std::string& key, const std::string& value);

}  // namespace leht::ops
