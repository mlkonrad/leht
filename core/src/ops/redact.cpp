// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/redact.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/text.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace leht::ops {

namespace {

using detail::OwnedPage;

pdf_redact_options to_mupdf(const RedactOptions& options) {
    pdf_redact_options opts{};
    opts.black_boxes = options.black_boxes ? 1 : 0;
    opts.text = PDF_REDACT_TEXT_REMOVE;
    switch (options.images) {
        case RedactImages::Pixels: opts.image_method = PDF_REDACT_IMAGE_PIXELS; break;
        case RedactImages::Remove: opts.image_method = PDF_REDACT_IMAGE_REMOVE; break;
        case RedactImages::Keep:   opts.image_method = PDF_REDACT_IMAGE_NONE; break;
    }
    switch (options.line_art) {
        case RedactLineArt::RemoveIfCovered:
            opts.line_art = PDF_REDACT_LINE_ART_REMOVE_IF_COVERED;
            break;
        case RedactLineArt::RemoveIfTouched:
            opts.line_art = PDF_REDACT_LINE_ART_REMOVE_IF_TOUCHED;
            break;
        case RedactLineArt::Keep: opts.line_art = PDF_REDACT_LINE_ART_NONE; break;
    }
    return opts;
}

bool touches(const fz_rect& r, const Rect* areas, std::size_t count) {
    const Rect box{r.x0, r.y0, r.x1, r.y1};
    for (std::size_t i = 0; i < count; ++i) {
        if (box.intersects(areas[i])) {
            return true;
        }
    }
    return false;
}

/// Deletes the first annotation or widget on `page` that overlaps `areas`, or
/// that replies to (/IRT) an annotation no longer on the page. Returns false
/// when there is none left. MuPDF invalidates the iteration on delete, so the
/// caller loops until this returns false.
///
/// Redaction annotations are left for pdf_redact_page to apply; Popups carry
/// no content of their own and go with their parent.
bool delete_one_overlapping(fz_context* ctx, pdf_page* page, const Rect* areas,
                            std::size_t count) {
    bool deleted = false;
    guarded(ctx, [&](fz_context* g) {
        pdf_obj* list = pdf_dict_get(g, page->obj, PDF_NAME(Annots));
        for (pdf_annot* a = pdf_first_annot(g, page); a != nullptr; a = pdf_next_annot(g, a)) {
            const enum pdf_annot_type type = pdf_annot_type(g, a);
            if (type == PDF_ANNOT_REDACT || type == PDF_ANNOT_POPUP) {
                continue;
            }
            pdf_obj* irt = pdf_dict_get(g, pdf_annot_obj(g, a), PDF_NAME(IRT));
            const bool orphan_reply = irt != nullptr && pdf_array_find(g, list, irt) < 0;
            if (orphan_reply || touches(pdf_bound_annot(g, a), areas, count)) {
                pdf_delete_annot(g, page, a);
                deleted = true;
                return;
            }
        }
        for (pdf_annot* w = pdf_first_widget(g, page); w != nullptr; w = pdf_next_widget(g, w)) {
            if (touches(pdf_bound_annot(g, w), areas, count)) {
                pdf_delete_annot(g, page, w);
                deleted = true;
                return;
            }
        }
    });
    return deleted;
}

// --- marked-content text ------------------------------------------------------
//
// Marked content can carry its own copy of the text it marks: a BDC property
// dictionary's /ActualText (the "real" text behind ligatures or drawn glyphs),
// /Alt (a description) and /E (an abbreviation's expansion). MuPDF 1.28's
// redaction edits these only on structure-tree elements, reached via /MCID.
// An inline dictionary in the content stream -- "/Span <</ActualText (...)>>
// BDC" -- is written back untouched, still holding the text whose glyphs were
// just removed. So after redacting, every such entry on the page goes.
// That errs on the side of caution, as MuPDF itself does when it cannot match
// the strings up: a redacted page loses these accessibility hints entirely.

void strip_marked_text(fz_context* ctx, pdf_obj* dict) {
    if (pdf_is_dict(ctx, dict)) {
        pdf_dict_del(ctx, dict, PDF_NAME(ActualText));
        pdf_dict_del(ctx, dict, PDF_NAME(Alt));
        pdf_dict_del(ctx, dict, PDF_NAME(E));
    }
}

/// The output processor's own BDC writer, which strip_BDC forwards to. Set
/// per thread just before each rewrite: every buffer processor shares the same
/// function, and each thread has its own independent Context.
thread_local void (*t_write_BDC)(fz_context*, pdf_processor*, const char*, pdf_obj*,
                                 pdf_obj*) = nullptr;

void strip_BDC(fz_context* ctx, pdf_processor* proc, const char* tag, pdf_obj* raw,
               pdf_obj* cooked) {
    // `raw` is the inline dictionary as written, or the name of a /Properties
    // resource; `cooked` is the dictionary either way. Stripping `cooked`
    // covers both: for a resource it edits the shared dictionary in place.
    strip_marked_text(ctx, raw);
    strip_marked_text(ctx, cooked);
    t_write_BDC(ctx, proc, tag, raw, cooked);
}

/// Re-emits content `stm` (a stream, or an array of them) through MuPDF's own
/// content interpreter into a new buffer, with strip_BDC in the BDC slot.
detail::OwnedBuffer rewrite_without_marked_text(fz_context* ctx, pdf_document* pdf,
                                                pdf_obj* res, pdf_obj* stm) {
    detail::OwnedBuffer buf{ctx};
    detail::Owned<pdf_processor, pdf_drop_processor> proc{ctx};
    guarded(ctx, [&](fz_context* g) {
        *buf.slot() = fz_new_buffer(g, 1024);
        *proc.slot() = pdf_new_buffer_processor(g, buf.get(), 1, 0);
        t_write_BDC = proc.get()->op_BDC;
        proc.get()->op_BDC = strip_BDC;
        pdf_process_contents(g, proc.get(), pdf, res, stm, nullptr, nullptr);
        pdf_close_processor(g, proc.get());
    });
    return buf;
}

/// Every Form XObject reachable from `res`, each once, into `out`. Returns
/// false if there were more than `cap` -- too many to be sure of cleaning.
bool collect_forms(fz_context* g, pdf_obj* res, pdf_obj** out, std::size_t& n,
                   std::size_t cap) {
    pdf_obj* xobjects = pdf_dict_get(g, res, PDF_NAME(XObject));
    const int len = pdf_dict_len(g, xobjects);
    for (int i = 0; i < len; ++i) {
        pdf_obj* x = pdf_dict_get_val(g, xobjects, i);
        if (!pdf_name_eq(g, pdf_dict_get(g, x, PDF_NAME(Subtype)), PDF_NAME(Form))) {
            continue;
        }
        bool seen = false;
        for (std::size_t k = 0; k < n && !seen; ++k) {
            seen = pdf_objcmp(g, out[k], x) == 0;
        }
        if (seen) {
            continue;
        }
        if (n == cap) {
            return false;
        }
        out[n++] = x;
        if (!collect_forms(g, pdf_dict_get(g, x, PDF_NAME(Resources)), out, n, cap)) {
            return false;
        }
    }
    return true;
}

void strip_page_marked_text(fz_context* ctx, pdf_document* pdf, pdf_page* page) {
    pdf_obj* res = nullptr;
    pdf_obj* contents = nullptr;
    guarded(ctx, [&](fz_context* g) {
        res = pdf_page_resources(g, page);
        contents = pdf_page_contents(g, page);
    });

    // Form XObjects first, while the page content still names them all.
    constexpr std::size_t kMaxForms = 1024;
    std::vector<pdf_obj*> forms(kMaxForms, nullptr);
    std::size_t n = 0;
    bool complete = false;
    pdf_obj** out = forms.data();
    guarded(ctx, [&](fz_context* g) { complete = collect_forms(g, res, out, n, kMaxForms); });
    if (!complete) {
        throw Error(0, "page has too many form XObjects to redact safely");
    }
    for (std::size_t i = 0; i < n; ++i) {
        pdf_obj* form = forms[i];
        pdf_obj* form_res = nullptr;
        guarded(ctx, [&](fz_context* g) {
            form_res = pdf_dict_get(g, form, PDF_NAME(Resources));
        });
        detail::OwnedBuffer buf =
            rewrite_without_marked_text(ctx, pdf, form_res != nullptr ? form_res : res, form);
        guarded(ctx, [&](fz_context* g) { pdf_update_stream(g, pdf, form, buf.get(), 0); });
    }

    if (contents == nullptr) {
        return;
    }
    detail::OwnedBuffer buf = rewrite_without_marked_text(ctx, pdf, res, contents);
    guarded(ctx, [&](fz_context* g) {
        pdf_obj* ref = pdf_add_stream(g, pdf, buf.get(), nullptr, 0);
        pdf_dict_put_drop(g, page->obj, PDF_NAME(Contents), ref);
    });
}

/// The redaction itself, on one loaded page. Returns how many redaction boxes
/// were applied (ours plus any already marked on the page).
int redact_loaded_page(fz_context* ctx, pdf_document* pdf, pdf_page* page,
                       const std::vector<Rect>& areas, const pdf_redact_options& opts,
                       int& annotations_removed) {
    const Rect* area_ptr = areas.data();
    const std::size_t count = areas.size();

    while (delete_one_overlapping(ctx, page, area_ptr, count)) {
        ++annotations_removed;
    }

    int applied = 0;
    guarded(ctx, [&](fz_context* g) {
        for (pdf_annot* a = pdf_first_annot(g, page); a != nullptr; a = pdf_next_annot(g, a)) {
            if (pdf_annot_type(g, a) == PDF_ANNOT_REDACT) {
                ++applied;  // marked by some other tool; applied along with ours
            }
        }
    });
    for (std::size_t i = 0; i < count; ++i) {
        const fz_rect r = fz_make_rect(area_ptr[i].x0, area_ptr[i].y0,
                                       area_ptr[i].x1, area_ptr[i].y1);
        detail::Owned<pdf_annot, pdf_drop_annot> annot{ctx};
        guarded(ctx, [&](fz_context* g) {
            *annot.slot() = pdf_create_annot(g, page, PDF_ANNOT_REDACT);
            pdf_set_annot_rect(g, annot.get(), r);
        });
        ++applied;
    }
    if (applied > 0) {
        pdf_redact_options mopts = opts;
        guarded(ctx, [&](fz_context* g) {
            pdf_redact_page(g, pdf, page, &mopts);
            // A thumbnail is a picture of the page before redaction.
            pdf_dict_del(g, page->obj, PDF_NAME(Thumb));
        });
        strip_page_marked_text(ctx, pdf, page);
    }
    return applied;
}

/// Removes the structure tree: its /Alt and /ActualText entries repeat page
/// text, and there is no reliable way to tell which ones describe removed
/// content. /MarkInfo goes with it, since it would claim a tagging that is no
/// longer there.
bool drop_structure_tree(fz_context* ctx, pdf_document* pdf) {
    bool dropped = false;
    guarded(ctx, [&](fz_context* g) {
        pdf_obj* root = pdf_dict_get(g, pdf_trailer(g, pdf), PDF_NAME(Root));
        if (pdf_dict_get(g, root, PDF_NAME(StructTreeRoot)) != nullptr) {
            pdf_dict_del(g, root, PDF_NAME(StructTreeRoot));
            dropped = true;
        }
        pdf_dict_dels(g, root, "MarkInfo");
    });
    return dropped;
}

void check_areas(const std::vector<Rect>& areas) {
    for (const Rect& r : areas) {
        if (r.empty()) {
            throw Error(0, "redaction area encloses nothing");
        }
    }
}

/// Redacts a set of pages, each with its own boxes. Shared by both entry
/// points, so structure dropping and marking happen once per call.
RedactResult redact_pages(const Context& ctx, Document& doc,
                          const std::vector<std::pair<int, std::vector<Rect>>>& work,
                          const RedactOptions& options) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    const int page_count = doc.page_count();
    const pdf_redact_options opts = to_mupdf(options);

    // Marked before anything changes: if a later page throws, the pages
    // already redacted must still get the garbage-collected save.
    if (!work.empty()) {
        doc.mark_redacted();
    }

    RedactResult result;
    for (const auto& [index, areas] : work) {
        if (index < 0 || index >= page_count) {
            throw Error(0, "page " + std::to_string(index + 1) + " is out of range");
        }
        check_areas(areas);
        OwnedPage page = detail::load_pdf_page(c, doc, index);
        const int applied = redact_loaded_page(c, pdf, detail::as_pdf_page(c, page),
                                               areas, opts, result.annotations_removed);
        if (applied > 0) {
            result.areas += applied;
            result.pages.push_back(index);
        }
    }
    if (!result.pages.empty()) {
        result.structure_dropped = drop_structure_tree(c, pdf);
    }
    std::sort(result.pages.begin(), result.pages.end());
    return result;
}

// --- what is left, for redact_text ------------------------------------------

std::string lowered(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool contains(const std::string& haystack, const std::string& needle_lower) {
    return lowered(haystack).find(needle_lower) != std::string::npos;
}

void check_outline(const std::vector<OutlineItem>& items, const std::string& needle,
                   std::vector<std::string>& remaining) {
    for (const OutlineItem& item : items) {
        if (contains(item.title, needle)) {
            remaining.push_back("bookmark \"" + item.title + "\"");
        }
        check_outline(item.children, needle, remaining);
    }
}

/// The document's XMP metadata stream as bytes, or empty.
std::string xmp_packet(fz_context* ctx, pdf_document* pdf) {
    detail::OwnedBuffer buf{ctx};
    guarded(ctx, [&](fz_context* g) {
        pdf_obj* root = pdf_dict_get(g, pdf_trailer(g, pdf), PDF_NAME(Root));
        pdf_obj* meta = pdf_dict_get(g, root, PDF_NAME(Metadata));
        if (pdf_is_stream(g, meta)) {
            *buf.slot() = pdf_load_stream(g, meta);
        }
    });
    if (!buf) {
        return {};
    }
    unsigned char* data = nullptr;
    const std::size_t len = fz_buffer_storage(ctx, buf.get(), &data);
    return {reinterpret_cast<const char*>(data), len};
}

/// Appends to `remaining` every annotation /Contents and form field value on
/// `index` that contains the needle.
void check_annotations(fz_context* ctx, Document& doc, int index,
                       const std::string& needle, std::vector<std::string>& remaining) {
    OwnedPage page = detail::load_pdf_page(ctx, doc, index);
    pdf_page* p = detail::as_pdf_page(ctx, page);

    // Collect raw pointers inside the guard (they point into objects the
    // document keeps alive), then copy them into strings outside it.
    const char* texts[64];
    bool is_field[64];
    std::size_t n = 0;
    guarded(ctx, [&](fz_context* g) {
        for (pdf_annot* a = pdf_first_annot(g, p); a != nullptr && n < 64; a = pdf_next_annot(g, a)) {
            texts[n] = pdf_annot_contents(g, a);
            is_field[n++] = false;
        }
        for (pdf_annot* w = pdf_first_widget(g, p); w != nullptr && n < 64; w = pdf_next_widget(g, w)) {
            texts[n] = pdf_field_value(g, pdf_annot_obj(g, w));
            is_field[n++] = true;
        }
    });
    const std::string where = " on page " + std::to_string(index + 1);
    for (std::size_t i = 0; i < n; ++i) {
        if (texts[i] != nullptr && contains(texts[i], needle)) {
            remaining.push_back(is_field[i] ? "form field value" + where
                                            : "annotation text" + where);
        }
    }
}

/// Document-wide places (metadata, bookmarks) are always checked; page text,
/// annotations and fields only on `pages`, the ones the caller chose to redact.
std::vector<std::string> find_remaining(const Context& ctx, Document& doc,
                                        const std::string& needle,
                                        const std::vector<int>& pages) {
    fz_context* c = ctx.raw();
    const std::string lower = lowered(needle);
    std::vector<std::string> remaining;

    for (const char* key : {"Title", "Author", "Subject", "Keywords", "Creator", "Producer"}) {
        const auto value = doc.metadata(std::string("info:") + key);
        if (value && contains(*value, lower)) {
            remaining.push_back(std::string("document metadata (") + key + ")");
        }
    }
    if (contains(xmp_packet(c, detail::require_pdf(c, doc)), lower)) {
        remaining.push_back("document metadata (XMP)");
    }
    check_outline(doc.outline(), lower, remaining);

    for (const int i : pages) {
        const TextPage text(ctx, doc, i);
        if (!text.search(needle, 1).empty()) {
            remaining.push_back("page " + std::to_string(i + 1) +
                                " text the redaction could not reach");
        }
        check_annotations(c, doc, i, lower, remaining);
    }
    return remaining;
}

}  // namespace

RedactResult redact(const Context& ctx, Document& doc, int page,
                    const std::vector<Rect>& areas, const RedactOptions& options) {
    if (areas.empty()) {
        detail::require_pdf(ctx.raw(), doc);
        return {};
    }
    return redact_pages(ctx, doc, {{page, areas}}, options);
}

RedactResult redact_text(const Context& ctx, Document& doc, const std::string& needle,
                         const std::string& pages, const RedactOptions& options) {
    detail::require_pdf(ctx.raw(), doc);
    if (needle.empty()) {
        throw Error(0, "nothing to redact: the search text is empty");
    }

    // Far above any real page; hitting it means we cannot be sure we saw every
    // occurrence, and a redaction that silently misses some is worse than none.
    constexpr std::size_t kMaxHits = 100000;

    const std::vector<int> selected = page_set(pages, doc.page_count());
    std::vector<std::pair<int, std::vector<Rect>>> work;
    for (const int index : selected) {
        const TextPage text(ctx, doc, index);
        const std::vector<SearchHit> hits = text.search(needle, kMaxHits);
        if (hits.size() >= kMaxHits) {
            throw Error(0, "too many matches on page " + std::to_string(index + 1));
        }
        std::vector<Rect> areas;
        for (const SearchHit& hit : hits) {
            for (const TextQuad& q : hit.quads) {
                areas.push_back({q.min_x(), q.min_y(), q.max_x(), q.max_y()});
            }
        }
        if (!areas.empty()) {
            work.emplace_back(index, std::move(areas));
        }
    }

    RedactResult result = redact_pages(ctx, doc, work, options);
    result.remaining = find_remaining(ctx, doc, needle, selected);
    return result;
}

}  // namespace leht::ops
