// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// leht::ocr -- reading the words in a picture of a page.
//
// Tesseract is linked PRIVATE and no header here names a Tesseract type, the
// rule core/ keeps for MuPDF and crypto/ for OpenSSL. This library never
// parses a PDF: it sees pixels, and gives back words with boxes. Writing them
// into a document is core's (ops::add_text_layer).
//
// Who runs it matters: loading a language opens files, which the sandboxed
// OCR worker cannot do once its sandbox is up. So a Recognizer loads
// everything in its constructor, and recognize() never touches the disk.

#include "leht/ops/ocr_layer.hpp"
#include "leht/renderer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace leht::ocr {

/// Where Tesseract's language data is: $TESSDATA_PREFIX when set, else the
/// first of the usual Fedora, Debian and upstream locations that exists.
/// Empty when none does.
[[nodiscard]] std::string default_datadir();

/// The languages installed in `datadir` ("eng", "est", ...), sorted, without
/// Tesseract's own orientation model "osd".
[[nodiscard]] std::vector<std::string> installed_languages(const std::string& datadir);

class Recognizer {
public:
    /// Loads `languages` ("est+eng": Tesseract's syntax) from `datadir`, all
    /// of it, now. Throws leht::Error naming a language that is not
    /// installed, or when Tesseract cannot start.
    Recognizer(const std::string& languages, const std::string& datadir);
    ~Recognizer();
    Recognizer(Recognizer&&) noexcept;
    Recognizer& operator=(Recognizer&&) noexcept;
    Recognizer(const Recognizer&) = delete;
    Recognizer& operator=(const Recognizer&) = delete;

    /// The words in `page`, an RGB rendering at `zoom` pixels per point, with
    /// boxes in base coordinates (points, as the page is displayed). Words
    /// Tesseract is less than `min_confidence` percent sure of are left out:
    /// on a speck of dust it will still guess, and a guess must not become
    /// searchable text. Reads nothing from disk.
    [[nodiscard]] std::vector<ops::OcrWord> recognize(const Bitmap& page, float zoom,
                                                      int min_confidence = 30);

    [[nodiscard]] const std::string& languages() const { return languages_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string languages_;
};

}  // namespace leht::ocr
