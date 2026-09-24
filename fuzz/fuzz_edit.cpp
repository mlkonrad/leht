// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fuzz target for the M4 editing operations.
//
// Editing reaches much further into a document than viewing does. Redaction
// rewrites content streams and form XObjects through MuPDF's content
// interpreter. Annotations and form filling generate appearance streams from
// attacker-supplied dictionaries. Watermarking and crop rewrite page
// resources and boxes. Every one of these runs on whatever the file says,
// so each gets its own fresh document, and a failure in one cannot hide a
// crash in the next.
//
// Documents open from memory, and saves go to a memfd, so an iteration
// touches no file system.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/crop.hpp"
#include "leht/ops/forms.hpp"
#include "leht/ops/ocr_layer.hpp"
#include "leht/ops/redact.hpp"
#include "leht/ops/watermark.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>

namespace {

leht::Context& shared_context() {
    static leht::Context ctx;
    return ctx;
}

/// An in-memory file to save into, reused across iterations.
int sink() {
    static const int fd = ::memfd_create("leht-fuzz-edit", MFD_CLOEXEC);
    return fd;
}

void save(const leht::Document& doc) {
    const int fd = sink();
    if (fd < 0 || ::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) != 0) {
        return;
    }
    doc.save_fd(fd, leht::SaveOptions{});
}

/// Runs one group of edits on a fresh copy of the input, then saves it.
/// Expected failures on malformed input are absorbed; anything else -- a
/// crash, a sanitizer report -- is the bug being looked for.
template <typename F>
void attempt(const std::uint8_t* data, std::size_t size, F&& edits) {
    try {
        leht::Document doc = leht::Document::open_memory(shared_context(), data, size);
        if (doc.needs_password() || doc.page_count() <= 0) {
            return;
        }
        edits(doc);
        save(doc);
    } catch (const leht::Error&) {
    } catch (const std::bad_alloc&) {
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0 || size > (4U << 20)) {
        return 0;
    }
    leht::Context& ctx = shared_context();

    // Redaction: by text (search, redact, then the leak report's walk over
    // metadata, bookmarks and annotations), and by area.
    attempt(data, size, [&](leht::Document& doc) {
        (void)leht::ops::redact_text(ctx, doc, "e", "1");
        (void)leht::ops::redact(ctx, doc, 0, {{0, 0, 300, 300}});
    });

    // Page marks.
    attempt(data, size, [&](leht::Document& doc) {
        leht::ops::WatermarkOptions mark;
        mark.text = "FUZZ";
        (void)leht::ops::watermark(ctx, doc, "1", mark);
        (void)leht::ops::crop_margins(ctx, doc, "1", {5, 5, 5, 5});
        (void)leht::ops::crop(ctx, doc, "1", {20, 20, 300, 300});
    });

    // Annotations: add one of each appearance-generating kind, list, delete.
    attempt(data, size, [&](leht::Document& doc) {
        using leht::ops::AnnotKind;
        for (const AnnotKind kind : {AnnotKind::Note, AnnotKind::FreeText, AnnotKind::Ink,
                                     AnnotKind::Square, AnnotKind::Stamp}) {
            leht::ops::AnnotSpec spec;
            spec.kind = kind;
            spec.rect = {10, 10, 200, 60};
            spec.strokes = {{{10, 10}, {50, 50}}};
            spec.contents = "fuzz";
            (void)leht::ops::add_annotation(ctx, doc, 0, spec);
        }
        leht::ops::AnnotSpec mark;
        mark.kind = AnnotKind::Highlight;
        (void)leht::ops::mark_text(ctx, doc, "a", "1", mark);
        // Move, resize and retext everything, the fuzzed file's own
        // annotations included: their /InkList, /Vertices, /CL and /Popup
        // are whatever the input says they are.
        for (const auto& a : leht::ops::list_annotations(ctx, doc)) {
            try {
                const leht::Rect& r = a.rect;
                (void)leht::ops::move_annotation(ctx, doc, a.id,
                                                 {r.x0 + 7, r.y0 + 3, r.x1 + 7, r.y1 + 3});
                (void)leht::ops::move_annotation(ctx, doc, a.id,
                                                 {r.x0, r.y0, r.x0 + 2 * (r.x1 - r.x0) + 1,
                                                  r.y0 + (r.y1 - r.y0) / 2 + 1});
            } catch (const leht::Error&) {
                // Not movable, not resizable, or an empty box: refused, go on.
            }
            try {
                (void)leht::ops::set_annotation_contents(ctx, doc, a.id, "fuzz again");
            } catch (const leht::Error&) {
            }
        }
        for (const auto& a : leht::ops::list_annotations(ctx, doc)) {
            (void)leht::ops::delete_annotation(ctx, doc, a.id);
            break;
        }
    });

    // An OCR text layer, twice (the second reuses the first one's font), with
    // words that are awkward on purpose: empty, astral, huge, zero-width.
    attempt(data, size, [&](leht::Document& doc) {
        const std::vector<leht::ops::OcrWord> words = {
            {"Tere", {10, 10, 60, 24}},
            {"õhtust", {70, 10, 130, 24}},
            {"", {1, 1, 5, 5}},
            {"\xF0\x9F\x93\x84", {140, 10, 160, 24}},
            {"big", {0, 0, 1e6F, 1e6F}},
            {"thin", {5, 5, 5.0001F, 30}},
        };
        (void)leht::ops::add_text_layer(ctx, doc, 0, words);
        (void)leht::ops::add_text_layer(ctx, doc, doc.page_count() - 1, words);
        (void)leht::ops::page_has_text(ctx, doc, 0);
    });

    // Forms: set every field to something plausible for its type, flatten.
    attempt(data, size, [&](leht::Document& doc) {
        for (const auto& f : leht::ops::list_fields(ctx, doc)) {
            const std::string value = f.options.empty() ? std::string("x") : f.options.front();
            try {
                leht::ops::set_field(ctx, doc, f.name, value);
            } catch (const leht::Error&) {
                // Read-only, a button, a length limit: refused, keep going.
            }
        }
        (void)leht::ops::flatten(ctx, doc, true);
    });

    return 0;
}
