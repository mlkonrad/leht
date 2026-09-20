// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/pages.hpp"

#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

namespace leht::ops {

namespace {

using detail::OwnedPdfDoc;

std::size_t file_size_or_zero(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::size_t>(size);
}

pdf_write_options default_options() {
    pdf_write_options opts = pdf_default_write_options;
    opts.do_garbage = 3;
    opts.do_compress = 1;
    opts.do_compress_images = 1;
    opts.do_compress_fonts = 1;
    return opts;
}

OwnedPdfDoc open_pdf(fz_context* ctx, const std::string& path) {
    OwnedPdfDoc doc{ctx};
    const char* cpath = path.c_str();
    guarded(ctx, [&](fz_context* g) { *doc.slot() = pdf_open_document(g, cpath); });
    if (!doc) {
        throw Error(0, "could not open as PDF: " + path);
    }
    return doc;
}

int count_pages(fz_context* ctx, pdf_document* doc) {
    int pages = 0;
    guarded(ctx, [&](fz_context* g) { pages = pdf_count_pages(g, doc); });
    return pages;
}

/// Copies `selection` (0-based, in order) from `src` into a fresh document.
PagesResult graft_selection(fz_context* ctx, pdf_document* src,
                            const std::vector<int>& selection,
                            const std::string& output) {
    OwnedPdfDoc dst{ctx};
    guarded(ctx, [&](fz_context* g) { *dst.slot() = pdf_create_document(g); });
    if (!dst) {
        throw Error(0, "could not create the output document");
    }

    pdf_document* target = dst.get();
    for (const int page : selection) {
        guarded(ctx, [&](fz_context* g) {
            pdf_graft_page(g, target, -1, src, page);
        });
    }

    pdf_write_options opts = default_options();
    const char* out = output.c_str();
    guarded(ctx, [&](fz_context* g) {
        pdf_save_document(g, target, out, &opts);
    });

    PagesResult result;
    result.pages_written = static_cast<int>(selection.size());
    result.output_bytes = file_size_or_zero(output);
    return result;
}

/// Expands a filename pattern containing exactly one integer field.
///
/// Supports `%d` and zero-padded `%0Nd`; `%%` is a literal percent. Everything
/// else is rejected.
///
/// This deliberately does NOT call snprintf with the caller's pattern. Doing so
/// is an uncontrolled format string (CWE-134): `%s` makes printf dereference the
/// page number as a pointer, and `%n` turns it into an arbitrary write. An
/// earlier version of this function did exactly that and segfaulted on
/// `leht split in.pdf -o '%s.pdf'`. Formatting the number here keeps printf out
/// of reach of user input entirely.
std::string expand_pattern(const std::string& pattern, int value) {
    std::string out;
    out.reserve(pattern.size() + 8);
    int conversions = 0;

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            out.push_back(pattern[i]);
            continue;
        }

        if (i + 1 < pattern.size() && pattern[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }

        std::size_t j = i + 1;
        bool zero_pad = false;
        while (j < pattern.size() && pattern[j] == '0') {
            zero_pad = true;
            ++j;
        }
        std::size_t width = 0;
        while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
            width = width * 10 + static_cast<std::size_t>(pattern[j] - '0');
            ++j;
        }
        if (width > 64) {
            throw Error(0, "field width too large in output pattern: " + pattern);
        }
        if (j >= pattern.size() || (pattern[j] != 'd' && pattern[j] != 'i')) {
            throw Error(0,
                        "output pattern may only contain an integer field such "
                        "as %d or %03d (and %% for a literal percent): " +
                            pattern);
        }

        std::string digits = std::to_string(value);
        if (zero_pad && digits.size() < width) {
            digits.insert(0, width - digits.size(), '0');
        } else if (!zero_pad && digits.size() < width) {
            digits.insert(0, width - digits.size(), ' ');
        }
        out += digits;

        ++conversions;
        i = j;
    }

    if (conversions != 1) {
        throw Error(0, "output pattern needs exactly one integer field, e.g. "
                       "part-%03d.pdf (got " +
                           std::to_string(conversions) + "): " + pattern);
    }
    return out;
}

int parse_int(const std::string& text, const std::string& spec) {
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        if (consumed != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        throw Error(0, "bad page range '" + spec + "': '" + text +
                           "' is not a page number");
    }
}

}  // namespace

std::vector<int> parse_page_ranges(const std::string& spec, int page_count) {
    if (page_count <= 0) {
        throw Error(0, "document has no pages");
    }

    std::vector<int> pages;
    const std::string trimmed =
        spec.find_first_not_of(" \t") == std::string::npos ? "" : spec;
    if (trimmed.empty()) {
        pages.reserve(static_cast<std::size_t>(page_count));
        for (int i = 0; i < page_count; ++i) {
            pages.push_back(i);
        }
        return pages;
    }

    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t comma = trimmed.find(',', start);
        std::string part = trimmed.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start);
        start = comma == std::string::npos ? trimmed.size() + 1 : comma + 1;

        // Strip surrounding whitespace.
        const std::size_t first = part.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        part = part.substr(first, part.find_last_not_of(" \t") - first + 1);

        const std::size_t dash = part.find('-');
        int from = 0;
        int to = 0;
        if (dash == std::string::npos) {
            from = to = parse_int(part, spec);
        } else {
            const std::string lhs = part.substr(0, dash);
            const std::string rhs = part.substr(dash + 1);
            from = lhs.empty() ? 1 : parse_int(lhs, spec);
            to = rhs.empty() ? page_count : parse_int(rhs, spec);
        }

        for (const int page : {from, to}) {
            if (page < 1 || page > page_count) {
                throw Error(0, "page " + std::to_string(page) + " is out of range (" +
                                   std::to_string(page_count) + " pages)");
            }
        }

        // from > to counts backwards, so "5-1" reverses those pages.
        const int step = from <= to ? 1 : -1;
        for (int page = from;; page += step) {
            pages.push_back(page - 1);  // to 0-based
            if (page == to) {
                break;
            }
        }
    }
    return pages;
}

PagesResult extract(const Context& ctx, const std::string& input,
                    const std::string& output, const std::string& ranges) {
    fz_context* c = ctx.raw();
    OwnedPdfDoc doc = open_pdf(c, input);
    const std::vector<int> selection =
        parse_page_ranges(ranges, count_pages(c, doc.get()));
    if (selection.empty()) {
        throw Error(0, "page selection is empty");
    }
    return graft_selection(c, doc.get(), selection, output);
}

PagesResult remove_pages(const Context& ctx, const std::string& input,
                         const std::string& output, const std::string& ranges) {
    fz_context* c = ctx.raw();
    OwnedPdfDoc doc = open_pdf(c, input);
    const int total = count_pages(c, doc.get());

    const std::vector<int> drop = parse_page_ranges(ranges, total);
    const std::set<int> dropped(drop.begin(), drop.end());

    std::vector<int> keep;
    keep.reserve(static_cast<std::size_t>(total));
    for (int page = 0; page < total; ++page) {
        if (dropped.find(page) == dropped.end()) {
            keep.push_back(page);
        }
    }
    if (keep.empty()) {
        throw Error(0, "refusing to write a document with no pages");
    }
    return graft_selection(c, doc.get(), keep, output);
}

PagesResult rotate(const Context& ctx, const std::string& input,
                   const std::string& output, const std::string& ranges,
                   int degrees) {
    if (degrees % 90 != 0) {
        throw Error(0, "rotation must be a multiple of 90 degrees, got " +
                           std::to_string(degrees));
    }
    fz_context* c = ctx.raw();
    OwnedPdfDoc doc = open_pdf(c, input);
    const int total = count_pages(c, doc.get());
    const std::vector<int> selection = parse_page_ranges(ranges, total);

    pdf_document* pdf = doc.get();
    for (const int page : selection) {
        guarded(c, [&](fz_context* g) {
            pdf_obj* page_obj = pdf_lookup_page_obj(g, pdf, page);
            const int current =
                pdf_dict_get_int(g, page_obj, PDF_NAME(Rotate));
            // PDF /Rotate must be a non-negative multiple of 90.
            const int updated = (((current + degrees) % 360) + 360) % 360;
            // put_int, not put(..., pdf_new_int(...)): pdf_dict_put takes its
            // own reference, so the one pdf_new_int returns would never be
            // dropped. ASan caught exactly that leak here.
            pdf_dict_put_int(g, page_obj, PDF_NAME(Rotate), updated);
        });
    }

    pdf_write_options opts = default_options();
    const char* out = output.c_str();
    guarded(c, [&](fz_context* g) { pdf_save_document(g, pdf, out, &opts); });

    PagesResult result;
    result.pages_written = total;
    result.output_bytes = file_size_or_zero(output);
    return result;
}

std::vector<std::string> split(const Context& ctx, const std::string& input,
                               const std::string& output_pattern,
                               int pages_per_file) {
    if (pages_per_file < 1) {
        throw Error(0, "pages per file must be at least 1");
    }
    // Validate the pattern before opening anything, so a bad pattern costs
    // nothing and cannot half-finish a split.
    (void)expand_pattern(output_pattern, 1);

    fz_context* c = ctx.raw();
    OwnedPdfDoc doc = open_pdf(c, input);
    const int total = count_pages(c, doc.get());

    std::vector<std::string> written;
    int index = 1;
    for (int first = 0; first < total; first += pages_per_file, ++index) {
        const int last = std::min(first + pages_per_file, total);

        std::vector<int> selection;
        selection.reserve(static_cast<std::size_t>(last - first));
        for (int page = first; page < last; ++page) {
            selection.push_back(page);
        }

        const std::string path = expand_pattern(output_pattern, index);
        graft_selection(c, doc.get(), selection, path);
        written.push_back(path);
    }
    return written;
}

}  // namespace leht::ops
