// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace leht {
class Context;
}

namespace leht::ops {

struct MergeOptions {
    /// Garbage collection before writing: 0 none, 1 collect, 2 renumber,
    /// 3 de-duplicate. 3 is what makes merging N similar files not cost N
    /// copies of their shared fonts.
    int garbage = 3;
    bool compress_streams = true;
    /// Linearise for "fast web view". Costs a second pass; off by default.
    bool linearize = false;
};

struct MergeResult {
    int pages_written = 0;
    int inputs_merged = 0;
    std::size_t output_bytes = 0;
};

/// Merges PDFs and images, in argument order, into a single PDF.
///
/// Inputs may be mixed freely: each PDF contributes all of its pages, each
/// image contributes one page sized to the image's own resolution.
///
/// Images are embedded LOSSLESSLY wherever the source encoding permits -- a
/// JPEG's compressed stream is copied into the PDF verbatim rather than being
/// decoded and re-encoded. Re-encoding on import is the standard way these
/// tools quietly degrade a scan, and it is worth the extra care to avoid.
///
/// Throws leht::Error if an input cannot be read or the output cannot be
/// written. Never modifies the inputs.
MergeResult merge(const Context& ctx, const std::vector<std::string>& inputs,
                  const std::string& output, const MergeOptions& options = {});

/// True if `path` looks like an image leht can turn into a page, by extension.
[[nodiscard]] bool looks_like_image(const std::string& path);

}  // namespace leht::ops
