// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The invisible text layer OCR leaves on a scanned page.
//
// The recognising is done elsewhere (leht::ocr, on pixels); this is the PDF
// half: given words and where they are, write them onto the page as text that
// is never drawn but is found, selected and copied -- search on a scan.

#include "leht/edit.hpp"

#include <string>
#include <vector>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

/// One recognised word: its text (UTF-8) and its box in base coordinates.
struct OcrWord {
    std::string text;
    Rect box;
};

/// Writes `words` onto `page` (0-based) as invisible text: text render mode 3,
/// in a glyphless font, each word stretched to fill its box so a selection
/// covers the word in the picture. The page looks exactly as before. Any
/// script works: the font maps every character to a glyph that draws nothing,
/// and its ToUnicode map gives the text back. The font is added to the
/// document once and reused by later pages. Words with no text or an empty
/// or non-finite box are skipped. Returns the number of words written.
int add_text_layer(const Context& ctx, Document& doc, int page,
                   const std::vector<OcrWord>& words);

/// Whether `page` already carries text (at least `min_chars` characters that
/// are not spaces) -- a born-digital page, or one OCR'd before. OCR skips such
/// pages unless told otherwise, so a page never gets a second copy of its text.
bool page_has_text(const Context& ctx, Document& doc, int page, int min_chars = 16);

}  // namespace leht::ops
