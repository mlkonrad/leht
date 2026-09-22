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
        for (const auto& a : leht::ops::list_annotations(ctx, doc)) {
            (void)leht::ops::delete_annotation(ctx, doc, a.id);
            break;
        }
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
