// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/compress.hpp"

#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <filesystem>

namespace leht::ops {

namespace {

using detail::OwnedBuffer;
using detail::OwnedImage;
using detail::OwnedPdfDoc;

/// Ceiling on the pixmap compress() will ask MuPDF to decode for one image,
/// measured at maximum subsampling. See recompress_image().
constexpr std::size_t kMaxDecodeBytes = std::size_t{256} << 20;  // 256 MB

struct PresetValues {
    int max_edge;   // longest edge in pixels, 0 = no image work
    int quality;    // JPEG quality
};

/// Nominal long page edge of 11 inches turns a target DPI into a pixel budget.
/// Measuring true placement size would mean interpreting every content stream
/// that draws the image; this approximation is close enough and never
/// upsamples, because an image is only ever shrunk toward the budget.
PresetValues values_for(CompressPreset preset) {
    switch (preset) {
        case CompressPreset::Lossless:
            return {0, 0};
        case CompressPreset::Print:
            return {300 * 11, 85};
        case CompressPreset::Ebook:
            return {150 * 11, 75};
        case CompressPreset::Screen:
            return {72 * 11, 60};
    }
    return {0, 0};
}

bool is_image_object(fz_context* ctx, pdf_obj* obj) {
    return pdf_name_eq(ctx, pdf_dict_get(ctx, obj, PDF_NAME(Subtype)),
                       PDF_NAME(Image)) != 0;
}

/// Transparency cannot survive a JPEG round trip, so those images are skipped.
bool has_transparency(fz_context* ctx, pdf_obj* obj) {
    return pdf_dict_get(ctx, obj, PDF_NAME(SMask)) != nullptr ||
           pdf_dict_get(ctx, obj, PDF_NAME(Mask)) != nullptr;
}

/// Re-encodes one image object if that makes it smaller. Returns true if the
/// object was replaced.
bool recompress_image(fz_context* ctx, pdf_document* doc, pdf_obj* ref,
                      int max_edge, int quality) {
    OwnedImage image{ctx};
    guarded(ctx, [&](fz_context* g) {
        *image.slot() = pdf_load_image(g, doc, ref);
    });
    if (!image) {
        return false;
    }

    fz_image* img = image.get();
    const int width = img->w;
    const int height = img->h;
    if (width <= 0 || height <= 0) {
        return false;
    }

    // Backstop for images so large that even MuPDF's maximum subsampling (64x
    // per axis) leaves an unreasonable allocation. Such an image is skipped --
    // left exactly as it was -- rather than decoded. Real content never gets
    // near this: a full-page 600 DPI scan is ~34 megapixels before subsampling.
    const auto at_max_subsample = [](int extent) {
        return static_cast<std::size_t>(std::max(1, extent >> 6));
    };
    if (at_max_subsample(width) * at_max_subsample(height) * 4 > kMaxDecodeBytes) {
        return false;
    }

    const int longest = std::max(width, height);
    const double scale =
        longest > max_edge ? static_cast<double>(max_edge) /
                                 static_cast<double>(longest)
                           : 1.0;
    const int target_w = std::max(1, static_cast<int>(width * scale));
    const int target_h = std::max(1, static_cast<int>(height * scale));

    OwnedBuffer encoded{ctx};
    int final_w = target_w;
    int final_h = target_h;

    if (scale < 1.0) {
        // Downsample, then encode the smaller pixmap.
        fz_pixmap* decoded = nullptr;
        fz_pixmap* scaled = nullptr;
        fz_image* rebuilt = nullptr;
        // Ask MuPDF for (roughly) the size we are about to scale to, not the
        // full resolution. Given a ctm, fz_get_pixmap_from_image picks the
        // largest power-of-two subsampling -- up to 64x per axis -- that still
        // covers the requested size, and decodes at that. Without this hint an
        // 888-byte file declaring a 16000x16000 image made compress allocate
        // 773 MB to produce a thumbnail; with it the same file decodes at
        // 2000x2000. This is the decompression-bomb defence.
        const fz_matrix size_hint = fz_scale(static_cast<float>(target_w),
                                             static_cast<float>(target_h));
        guarded(ctx, [&](fz_context* g) {
            fz_matrix hint = size_hint;
            decoded = fz_get_pixmap_from_image(g, img, nullptr, &hint,
                                               nullptr, nullptr);
        });
        detail::Owned<fz_pixmap, fz_drop_pixmap> decoded_guard{ctx, decoded};
        if (decoded == nullptr) {
            return false;
        }

        guarded(ctx, [&](fz_context* g) {
            scaled = fz_scale_pixmap(g, decoded, 0, 0,
                                     static_cast<float>(target_w),
                                     static_cast<float>(target_h), nullptr);
        });
        detail::Owned<fz_pixmap, fz_drop_pixmap> scaled_guard{ctx, scaled};
        if (scaled == nullptr) {
            return false;
        }

        guarded(ctx, [&](fz_context* g) {
            rebuilt = fz_new_image_from_pixmap(g, scaled, nullptr);
        });
        OwnedImage rebuilt_guard{ctx, rebuilt};
        if (rebuilt == nullptr) {
            return false;
        }

        final_w = scaled->w;
        final_h = scaled->h;
        guarded(ctx, [&](fz_context* g) {
            *encoded.slot() = fz_new_buffer_from_image_as_jpeg(
                g, rebuilt, fz_default_color_params, quality, 0);
        });
    } else {
        final_w = width;
        final_h = height;
        guarded(ctx, [&](fz_context* g) {
            *encoded.slot() = fz_new_buffer_from_image_as_jpeg(
                g, img, fz_default_color_params, quality, 0);
        });
    }

    if (!encoded) {
        return false;
    }

    std::size_t new_size = 0;
    std::size_t old_size = 0;
    guarded(ctx, [&](fz_context* g) {
        new_size = fz_buffer_storage(g, encoded.get(), nullptr);
        fz_compressed_buffer* original = fz_compressed_image_buffer(g, img);
        old_size = original != nullptr && original->buffer != nullptr
                       ? fz_buffer_storage(g, original->buffer, nullptr)
                       : 0;
    });

    // Never keep a "compressed" image that got bigger. old_size of 0 means the
    // original was not stored compressed, so any JPEG is an improvement.
    if (new_size == 0 || (old_size != 0 && new_size >= old_size)) {
        return false;
    }

    fz_buffer* payload = encoded.get();
    guarded(ctx, [&](fz_context* g) {
        // Dictionary edits go to the resolved object; the stream replacement
        // needs the indirect reference itself.
        pdf_obj* dict = pdf_resolve_indirect(g, ref);
        // put_int rather than put(..., pdf_new_int(...)): the dict takes its
        // own reference and ours would leak. Same bug as ops/pages.cpp had.
        pdf_dict_put_int(g, dict, PDF_NAME(Width), final_w);
        pdf_dict_put_int(g, dict, PDF_NAME(Height), final_h);
        pdf_dict_put_int(g, dict, PDF_NAME(BitsPerComponent), 8);
        pdf_dict_put(g, dict, PDF_NAME(ColorSpace), PDF_NAME(DeviceRGB));
        pdf_dict_put(g, dict, PDF_NAME(Filter), PDF_NAME(DCTDecode));
        pdf_dict_del(g, dict, PDF_NAME(DecodeParms));
        pdf_dict_del(g, dict, PDF_NAME(Decode));
        // compressed = 1: the buffer already carries DCT encoding.
        pdf_update_stream(g, doc, ref, payload, 1);
    });
    return true;
}

std::size_t file_size_or_zero(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::size_t>(size);
}

}  // namespace

const char* preset_name(CompressPreset preset) {
    switch (preset) {
        case CompressPreset::Lossless: return "lossless";
        case CompressPreset::Print:    return "print";
        case CompressPreset::Ebook:    return "ebook";
        case CompressPreset::Screen:   return "screen";
    }
    return "unknown";
}

CompressResult compress(const Context& ctx, const std::string& input,
                        const std::string& output,
                        const CompressOptions& options) {
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot compress with a moved-from Context");
    }
    if (input == output) {
        throw Error(0, "compress will not write over its input: " + input);
    }

    CompressResult result;
    result.input_bytes = file_size_or_zero(input);

    OwnedPdfDoc doc{c};
    const char* in = input.c_str();
    guarded(c, [&](fz_context* g) { *doc.slot() = pdf_open_document(g, in); });
    if (!doc) {
        throw Error(0, "could not open as PDF: " + input);
    }

    const PresetValues preset = values_for(options.preset);
    const int max_edge =
        options.max_image_edge > 0 ? options.max_image_edge : preset.max_edge;
    const int quality =
        options.jpeg_quality > 0 ? options.jpeg_quality : preset.quality;

    if (max_edge > 0 && quality > 0) {
        pdf_document* pdf = doc.get();
        int object_count = 0;
        guarded(c, [&](fz_context* g) {
            object_count = pdf_count_objects(g, pdf);
        });

        for (int num = 1; num < object_count; ++num) {
            pdf_obj* ref = nullptr;
            bool interesting = false;
            guarded(c, [&](fz_context* g) {
                // An indirect reference, not a loaded copy: pdf_update_stream
                // replaces the stream behind a reference, and a detached copy
                // would be rejected with "object is not a stream".
                ref = pdf_new_indirect(g, pdf, num, 0);
                if (pdf_is_stream(g, ref) != 0) {
                    pdf_obj* dict = pdf_resolve_indirect(g, ref);
                    interesting = dict != nullptr && pdf_is_dict(g, dict) != 0 &&
                                  is_image_object(g, dict) &&
                                  !has_transparency(g, dict);
                }
            });
            detail::OwnedPdfObj ref_guard{c, ref};
            if (!interesting) {
                continue;
            }

            result.images_examined += 1;
            if (recompress_image(c, pdf, ref, max_edge, quality)) {
                result.images_recompressed += 1;
            }
        }
    }

    pdf_write_options opts = pdf_default_write_options;
    opts.do_garbage = 3;            // collect, renumber, de-duplicate
    opts.do_compress = 1;           // deflate streams
    opts.do_compress_images = 1;
    opts.do_compress_fonts = 1;
    opts.do_linear = options.linearize ? 1 : 0;
    opts.compression_effort = 100;  // slowest, smallest

    pdf_document* pdf = doc.get();
    detail::refuse_directory_output(output);
    const char* out = output.c_str();
    guarded(c, [&](fz_context* g) { pdf_save_document(g, pdf, out, &opts); });

    result.output_bytes = file_size_or_zero(output);
    return result;
}

}  // namespace leht::ops
