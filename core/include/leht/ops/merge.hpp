// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

typedef struct pdf_document pdf_document;

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

/// Builds a merged PDF one input at a time. merge() is this in one call; the
/// sandboxed worker uses it directly, because it receives its inputs as file
/// descriptors, one message each, and may hold only a handful open at once.
///
/// Borrows its Context, which must outlive it. Not thread-safe.
class Merger {
public:
    explicit Merger(const Context& ctx, const MergeOptions& options = {});
    ~Merger();
    Merger(Merger&& other) noexcept;
    Merger& operator=(Merger&& other) noexcept;
    Merger(const Merger&) = delete;
    Merger& operator=(const Merger&) = delete;

    /// Appends a PDF's pages, or an image as one page, deciding which by the
    /// extension as looks_like_image() does. Returns the pages added. An input
    /// that fails adds nothing, so the merge can go on without it.
    int add(const std::string& path);

    /// Appends from `fd`, TAKING OWNERSHIP: it is closed before this returns,
    /// whether or not it throws. A PDF is recognised by its "%PDF-" header
    /// (anywhere in the first KiB, as readers allow); anything else must be an
    /// image MuPDF can decode. `name` is only used in error messages.
    /// Password-protected PDFs are refused. Returns the pages added.
    int add_fd(int fd, const std::string& name);

    /// Writes the merged document to `output`. Throws if nothing was added.
    MergeResult finish(const std::string& output);

    /// As finish(), into `fd`, which is borrowed and written from its offset.
    MergeResult finish_fd(int fd);

    [[nodiscard]] int pages() const noexcept { return result_.pages_written; }
    [[nodiscard]] int inputs() const noexcept { return result_.inputs_merged; }

private:
    void require_open() const;
    /// Runs `append`, which returns the pages it added, and counts the input;
    /// if it throws, removes whatever pages it had added.
    int append_all_or_nothing(const std::function<int()>& append);

    const Context* ctx_ = nullptr;
    pdf_document* dst_ = nullptr;
    MergeOptions options_;
    MergeResult result_;
};

/// True if `path` looks like an image leht can turn into a page, by extension.
[[nodiscard]] bool looks_like_image(const std::string& path);

}  // namespace leht::ops
