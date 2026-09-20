// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace leht {
class Context;
}

namespace leht::ops {

/// Compression presets, named for what the output is for.
enum class CompressPreset {
    Lossless,  ///< Structural only: GC, de-duplicate, recompress streams.
    Print,     ///< ~300 DPI images, high JPEG quality.
    Ebook,     ///< ~150 DPI images. The sensible default for sharing.
    Screen,    ///< ~72 DPI images. Smallest, visibly softer.
};

struct CompressOptions {
    CompressPreset preset = CompressPreset::Ebook;

    /// 0 uses the preset's value. JPEG quality, 1-100.
    int jpeg_quality = 0;

    /// 0 uses the preset's value. Images whose longest edge exceeds this many
    /// pixels are downsampled to it.
    int max_image_edge = 0;

    /// Linearise for "fast web view". Costs a second pass.
    bool linearize = false;
};

struct CompressResult {
    std::size_t input_bytes = 0;
    std::size_t output_bytes = 0;
    int images_examined = 0;
    int images_recompressed = 0;

    /// Fraction of the original size removed, 0.0 to 1.0. Negative if the file
    /// grew, which is why compress() never overwrites the input.
    [[nodiscard]] double saved_fraction() const {
        if (input_bytes == 0) {
            return 0.0;
        }
        return 1.0 - (static_cast<double>(output_bytes) /
                      static_cast<double>(input_bytes));
    }
};

/// Recompresses `input` into `output`. Never modifies the input, so the caller
/// can show before/after sizes and let the user decide.
///
/// Lossless does structural work only. The lossy presets additionally decode,
/// downsample and re-encode image streams, and only keep a re-encoded image
/// when it actually came out smaller -- a preset must never inflate a file.
///
/// Images carrying transparency (/SMask or /Mask) are left untouched, because
/// JPEG cannot represent an alpha channel and silently flattening it would be
/// worse than not compressing.
///
/// Effective DPI is estimated from the image's pixel dimensions against a
/// nominal page, not measured from its placement in the content stream. It is
/// a heuristic, and a deliberately conservative one.
CompressResult compress(const Context& ctx, const std::string& input,
                        const std::string& output,
                        const CompressOptions& options = {});

/// Human-readable preset name, for CLI output.
[[nodiscard]] const char* preset_name(CompressPreset preset);

}  // namespace leht::ops
