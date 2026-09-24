// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ocr/ocr.hpp"

#include "leht/error.hpp"

#include <tesseract/capi.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace leht::ocr {

namespace fs = std::filesystem;

std::string default_datadir() {
    if (const char* env = std::getenv("TESSDATA_PREFIX"); env != nullptr && *env != '\0') {
        return env;
    }
    for (const char* dir : {"/usr/share/tesseract/tessdata",         // Fedora
                            "/usr/share/tesseract-ocr/5/tessdata",   // Debian, Ubuntu
                            "/usr/share/tesseract-ocr/4.00/tessdata",
                            "/usr/share/tessdata",                   // upstream, Arch
                            "/usr/local/share/tessdata"}) {
        std::error_code ec;
        if (fs::is_directory(dir, ec)) {
            return dir;
        }
    }
    return {};
}

std::vector<std::string> installed_languages(const std::string& datadir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(datadir, ec)) {
        const fs::path& p = entry.path();
        if (p.extension() == ".traineddata" && p.stem() != "osd") {
            out.push_back(p.stem().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct Recognizer::Impl {
    TessBaseAPI* api = nullptr;
    ~Impl() {
        if (api != nullptr) {
            TessBaseAPIEnd(api);
            TessBaseAPIDelete(api);
        }
    }
};

Recognizer::Recognizer(const std::string& languages, const std::string& datadir)
    : impl_(std::make_unique<Impl>()), languages_(languages) {
    if (datadir.empty()) {
        throw Error(0, "no Tesseract language data found; install tesseract-langpack-eng "
                       "(and -est for Estonian), or set TESSDATA_PREFIX");
    }
    // Every language must be there: Tesseract would otherwise print a warning
    // and carry on with fewer, which reads Estonian as English quietly.
    const auto installed = installed_languages(datadir);
    std::stringstream parts(languages);
    std::string lang;
    int count = 0;
    while (std::getline(parts, lang, '+')) {
        if (std::find(installed.begin(), installed.end(), lang) == installed.end()) {
            std::string have;
            for (const std::string& i : installed) {
                have += (have.empty() ? "" : ", ") + i;
            }
            throw Error(0, "the OCR language '" + lang + "' is not installed (installed: " +
                               (have.empty() ? "none" : have) + ")");
        }
        ++count;
    }
    if (count == 0) {
        throw Error(0, "no OCR language given");
    }
    impl_->api = TessBaseAPICreate();
    if (impl_->api == nullptr ||
        TessBaseAPIInit3(impl_->api, datadir.c_str(), languages.c_str()) != 0) {
        throw Error(0, "Tesseract could not load '" + languages + "' from " + datadir);
    }
    // The page as a whole, blocks of text found automatically.
    TessBaseAPISetPageSegMode(impl_->api, tesseract::PSM_AUTO);
}

Recognizer::~Recognizer() = default;
Recognizer::Recognizer(Recognizer&&) noexcept = default;
Recognizer& Recognizer::operator=(Recognizer&&) noexcept = default;

std::vector<ops::OcrWord> Recognizer::recognize(const Bitmap& page, float zoom,
                                                int min_confidence) {
    if (page.channels != 3 || page.width <= 0 || page.height <= 0 || !(zoom > 0) ||
        page.stride < page.width * 3 ||
        page.pixels.size() < static_cast<std::size_t>(page.stride) *
                                 static_cast<std::size_t>(page.height)) {
        throw Error(0, "not an RGB page image OCR can read");
    }
    TessBaseAPI* api = impl_->api;
    TessBaseAPISetImage(api, page.pixels.data(), page.width, page.height, 3, page.stride);
    // 72 points per inch: this many pixels per point is this many per inch.
    TessBaseAPISetSourceResolution(api, static_cast<int>(zoom * 72.0F + 0.5F));
    if (TessBaseAPIRecognize(api, nullptr) != 0) {
        TessBaseAPIClear(api);
        throw Error(0, "Tesseract could not read the page");
    }

    std::vector<ops::OcrWord> words;
    TessResultIterator* it = TessBaseAPIGetIterator(api);
    if (it != nullptr) {
        const TessPageIterator* pit = TessResultIteratorGetPageIterator(it);
        // Each word takes its text line's top and bottom: a word's own box
        // follows its letters (the q in "quick" hangs lower than "brown"),
        // and words on one line at different heights are read back by every
        // extractor as different lines.
        int line_top = 0;
        int line_bottom = 0;
        do {
            if (TessPageIteratorIsAtBeginningOf(pit, tesseract::RIL_TEXTLINE) != 0) {
                int l = 0;
                int r = 0;
                if (TessPageIteratorBoundingBox(pit, tesseract::RIL_TEXTLINE, &l, &line_top, &r,
                                                &line_bottom) == 0) {
                    line_top = line_bottom = 0;
                }
            }
            char* text = TessResultIteratorGetUTF8Text(it, tesseract::RIL_WORD);
            if (text == nullptr) {
                continue;
            }
            const float confidence = TessResultIteratorConfidence(it, tesseract::RIL_WORD);
            int left = 0;
            int top = 0;
            int right = 0;
            int bottom = 0;
            const bool boxed =
                TessPageIteratorBoundingBox(pit, tesseract::RIL_WORD, &left, &top, &right, &bottom) != 0;
            std::string word(text);
            TessDeleteText(text);
            const bool blank =
                word.find_first_not_of(" \t\r\n") == std::string::npos;
            if (!boxed || blank || confidence < static_cast<float>(min_confidence)) {
                continue;
            }
            if (line_bottom > line_top) {
                top = line_top;
                bottom = line_bottom;
            }
            words.push_back({std::move(word),
                             Rect{static_cast<float>(left) / zoom, static_cast<float>(top) / zoom,
                                  static_cast<float>(right) / zoom,
                                  static_cast<float>(bottom) / zoom}});
        } while (TessResultIteratorNext(it, tesseract::RIL_WORD) != 0);
        TessResultIteratorDelete(it);
    }
    TessBaseAPIClear(api);
    return words;
}

}  // namespace leht::ocr
