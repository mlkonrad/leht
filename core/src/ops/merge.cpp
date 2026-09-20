// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/merge.hpp"

#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string>

namespace leht::ops {

namespace {

using detail::OwnedBuffer;
using detail::OwnedFzDoc;
using detail::OwnedImage;
using detail::OwnedPdfDoc;
using detail::OwnedPdfObj;

std::string lowercase_extension(const std::string& path) {
    const std::string ext = std::filesystem::path(path).extension().string();
    std::string lowered;
    lowered.reserve(ext.size());
    for (const char c : ext) {
        lowered.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

pdf_write_options write_options(const MergeOptions& options) {
    pdf_write_options opts = pdf_default_write_options;
    opts.do_garbage = options.garbage;
    opts.do_compress = options.compress_streams ? 1 : 0;
    opts.do_compress_images = 1;  // leave already-compressed images alone
    opts.do_compress_fonts = options.compress_streams ? 1 : 0;
    opts.do_linear = options.linearize ? 1 : 0;
    return opts;
}

/// Appends one image as a page sized to the image's own resolution, so a
/// 300 DPI scan produces a correctly sized page rather than a huge one.
void append_image_page(fz_context* ctx, pdf_document* dst,
                       const std::string& path) {
    OwnedImage image{ctx};
    const char* cpath = path.c_str();
    guarded(ctx, [&](fz_context* g) {
        *image.slot() = fz_new_image_from_file(g, cpath);
    });
    if (!image) {
        throw Error(0, "could not read image: " + path);
    }

    fz_image* img = image.get();
    const float xres = img->xres > 0 ? static_cast<float>(img->xres) : 72.0F;
    const float yres = img->yres > 0 ? static_cast<float>(img->yres) : 72.0F;
    const float width = static_cast<float>(img->w) * 72.0F / xres;
    const float height = static_cast<float>(img->h) * 72.0F / yres;

    OwnedPdfObj image_ref{ctx};
    OwnedPdfObj resources{ctx};
    OwnedPdfObj xobjects{ctx};
    OwnedBuffer contents{ctx};
    OwnedPdfObj page{ctx};

    guarded(ctx, [&](fz_context* g) {
        // pdf_add_image copies the original compressed stream when the source
        // encoding is one PDF can carry directly (DCT/JPX/etc). That is the
        // lossless path -- no decode, no re-encode.
        *image_ref.slot() = pdf_add_image(g, dst, img);

        // Both dictionaries are held by guards in the outer frame. pdf_dict_puts
        // takes its own reference, so ours must still be dropped -- holding them
        // here also keeps it correct if MuPDF throws part-way through.
        *xobjects.slot() = pdf_new_dict(g, dst, 1);
        pdf_dict_puts(g, xobjects.get(), "Img", image_ref.get());

        *resources.slot() = pdf_new_dict(g, dst, 1);
        pdf_dict_puts(g, resources.get(), "XObject", xobjects.get());

        *contents.slot() = fz_new_buffer(g, 128);
        // %g takes a double through varargs; promote explicitly.
        fz_append_printf(g, contents.get(), "q %g 0 0 %g 0 0 cm /Img Do Q",
                         static_cast<double>(width),
                         static_cast<double>(height));

        const fz_rect mediabox = fz_make_rect(0, 0, width, height);
        *page.slot() =
            pdf_add_page(g, dst, mediabox, 0, resources.get(), contents.get());
        pdf_insert_page(g, dst, -1, page.get());
    });
}

/// Appends every page of a PDF. pdf_graft_page remaps object numbers and
/// resource names, which is what stops two inputs' /F1 fonts colliding.
int append_pdf_pages(fz_context* ctx, pdf_document* dst,
                     const std::string& path) {
    OwnedFzDoc source{ctx};
    const char* cpath = path.c_str();
    guarded(ctx, [&](fz_context* g) {
        *source.slot() = fz_open_document(g, cpath);
    });
    if (!source) {
        throw Error(0, "could not open input: " + path);
    }

    pdf_document* src_pdf = nullptr;
    guarded(ctx, [&](fz_context* g) {
        src_pdf = pdf_specifics(g, source.get());
    });
    if (src_pdf == nullptr) {
        throw Error(0, "not a PDF and not a supported image: " + path);
    }

    int pages = 0;
    guarded(ctx, [&](fz_context* g) { pages = pdf_count_pages(g, src_pdf); });

    for (int i = 0; i < pages; ++i) {
        guarded(ctx, [&](fz_context* g) {
            pdf_graft_page(g, dst, -1, src_pdf, i);
        });
    }
    return pages;
}

}  // namespace

bool looks_like_image(const std::string& path) {
    static constexpr std::array<const char*, 14> kImageExtensions{
        ".jpg", ".jpeg", ".jpe", ".png",  ".tif", ".tiff", ".bmp",
        ".gif", ".pnm",  ".pgm", ".ppm",  ".pam", ".jp2",  ".jpx"};

    const std::string ext = lowercase_extension(path);
    return std::any_of(kImageExtensions.begin(), kImageExtensions.end(),
                       [&ext](const char* known) { return ext == known; });
}

MergeResult merge(const Context& ctx, const std::vector<std::string>& inputs,
                  const std::string& output, const MergeOptions& options) {
    if (inputs.empty()) {
        throw Error(0, "merge needs at least one input");
    }
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot merge with a moved-from Context");
    }

    OwnedPdfDoc dst{c};
    guarded(c, [&](fz_context* g) { *dst.slot() = pdf_create_document(g); });
    if (!dst) {
        throw Error(0, "could not create the output document");
    }

    MergeResult result;
    for (const std::string& input : inputs) {
        if (looks_like_image(input)) {
            append_image_page(c, dst.get(), input);
            result.pages_written += 1;
        } else {
            result.pages_written += append_pdf_pages(c, dst.get(), input);
        }
        result.inputs_merged += 1;
    }

    pdf_write_options opts = write_options(options);
    pdf_document* doc = dst.get();
    const char* out = output.c_str();
    guarded(c, [&](fz_context* g) { pdf_save_document(g, doc, out, &opts); });

    std::error_code ec;
    const auto size = std::filesystem::file_size(output, ec);
    result.output_bytes = ec ? 0 : static_cast<std::size_t>(size);
    return result;
}

}  // namespace leht::ops
