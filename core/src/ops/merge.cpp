// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/merge.hpp"

#include "edit_internal.hpp"
#include "fd_output.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

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

/// Decodes the image at `path`. Throws if it is not one.
OwnedImage image_from_file(fz_context* ctx, const std::string& path) {
    OwnedImage image{ctx};
    const char* cpath = path.c_str();
    guarded(ctx, [&](fz_context* g) {
        *image.slot() = fz_new_image_from_file(g, cpath);
    });
    if (!image) {
        throw Error(0, "could not read image: " + path);
    }
    return image;
}

/// Appends one image as a page sized to the image's own resolution, so a
/// 300 DPI scan produces a correctly sized page rather than a huge one.
void append_image_page(fz_context* ctx, pdf_document* dst, fz_image* img) {
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

/// Appends every page of `src`. pdf_graft_page remaps object numbers and
/// resource names, which is what stops two inputs' /F1 fonts colliding.
int append_pdf_pages(fz_context* ctx, pdf_document* dst, pdf_document* src) {
    int pages = 0;
    guarded(ctx, [&](fz_context* g) { pages = pdf_count_pages(g, src); });

    for (int i = 0; i < pages; ++i) {
        guarded(ctx, [&](fz_context* g) {
            pdf_graft_page(g, dst, -1, src, i);
        });
    }
    return pages;
}

int append_pdf_file(fz_context* ctx, pdf_document* dst, const std::string& path) {
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
    return append_pdf_pages(ctx, dst, src_pdf);
}

/// Closes a descriptor on scope exit, for Merger::add_fd's ownership promise.
struct FdCloser {
    int fd;
    ~FdCloser() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    int release() noexcept { return std::exchange(fd, -1); }
};

/// Reads the whole of `fd` with pread, which the worker's sandbox allows.
OwnedBuffer read_all(fz_context* ctx, int fd, const std::string& name) {
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        throw Error(0, name + " is not a regular file");
    }
    // An image big enough to trip this would not fit the worker's address
    // space decoded anyway; failing here says so clearly.
    constexpr off_t kMaxImageBytes = off_t{512} << 20;
    if (st.st_size > kMaxImageBytes) {
        throw Error(0, name + " is too large to embed as an image");
    }

    OwnedBuffer buffer{ctx};
    const auto size = static_cast<std::size_t>(st.st_size);
    guarded(ctx, [&](fz_context* g) {
        *buffer.slot() = fz_new_buffer(g, size > 0 ? size : 1);
    });
    fz_buffer* buf = buffer.get();
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd, buf->data + done, size - done,
                                  static_cast<off_t>(done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            throw Error(0, "could not read " + name);
        }
        done += static_cast<std::size_t>(n);
    }
    buf->len = done;
    return buffer;
}

/// True if `fd` starts like a PDF: "%PDF-" within the first KiB.
bool sniffs_as_pdf(int fd) {
    std::array<char, 1024> head{};
    ssize_t n = 0;
    do {
        n = ::pread(fd, head.data(), head.size(), 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        return false;
    }
    const std::string_view view(head.data(), static_cast<std::size_t>(n));
    return view.find("%PDF-") != std::string_view::npos;
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

Merger::Merger(const Context& ctx, const MergeOptions& options)
    : ctx_(&ctx), options_(options) {
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot merge with a moved-from Context");
    }
    guarded(c, [&](fz_context* g) { dst_ = pdf_create_document(g); });
    if (dst_ == nullptr) {
        throw Error(0, "could not create the output document");
    }
}

Merger::~Merger() {
    if (dst_ != nullptr) {
        pdf_drop_document(ctx_->raw(), dst_);
    }
}

Merger::Merger(Merger&& other) noexcept
    : ctx_(other.ctx_),
      dst_(std::exchange(other.dst_, nullptr)),
      options_(other.options_),
      result_(std::exchange(other.result_, {})) {}

Merger& Merger::operator=(Merger&& other) noexcept {
    if (this != &other) {
        if (dst_ != nullptr) {
            pdf_drop_document(ctx_->raw(), dst_);
        }
        ctx_ = other.ctx_;
        dst_ = std::exchange(other.dst_, nullptr);
        options_ = other.options_;
        result_ = std::exchange(other.result_, {});
    }
    return *this;
}

void Merger::require_open() const {
    if (dst_ == nullptr) {
        throw Error(0, "this merge has been moved from");
    }
}

int Merger::add(const std::string& path) {
    require_open();
    fz_context* c = ctx_->raw();
    return append_all_or_nothing([&] {
        if (looks_like_image(path)) {
            OwnedImage image = image_from_file(c, path);
            append_image_page(c, dst_, image.get());
            return 1;
        }
        return append_pdf_file(c, dst_, path);
    });
}

int Merger::add_fd(int fd, const std::string& name) {
    FdCloser closer{fd};
    require_open();
    if (fd < 0) {
        throw Error(0, "invalid input descriptor for " + name);
    }
    fz_context* c = ctx_->raw();

    return append_all_or_nothing([&] {
        if (sniffs_as_pdf(fd)) {
            // open_fd takes the descriptor, and closes it even if it throws.
            Document source = Document::open_fd(*ctx_, closer.release(), "pdf");
            if (source.needs_password()) {
                throw Error(0, name + " is password-protected; decrypt it first");
            }
            return append_pdf_pages(c, dst_, detail::require_pdf(c, source));
        }
        OwnedBuffer bytes = read_all(c, fd, name);
        OwnedImage image{c};
        fz_buffer* buf = bytes.get();
        try {
            guarded(c, [&](fz_context* g) {
                *image.slot() = fz_new_image_from_buffer(g, buf);
            });
        } catch (const Error&) {
            throw Error(0, "not a PDF and not a supported image: " + name);
        }
        if (!image) {
            throw Error(0, "not a PDF and not a supported image: " + name);
        }
        append_image_page(c, dst_, image.get());
        return 1;
    });
}

int Merger::append_all_or_nothing(const std::function<int()>& append) {
    fz_context* c = ctx_->raw();
    pdf_document* dst = dst_;
    int before = 0;
    guarded(c, [&](fz_context* g) { before = pdf_count_pages(g, dst); });
    try {
        const int added = append();
        result_.pages_written += added;
        result_.inputs_merged += 1;
        return added;
    } catch (...) {
        // An input that fails part-way leaves none of its pages behind, so a
        // caller may carry on without it. The orphaned objects are dropped by
        // the garbage collection every merge is written with.
        int now = before;
        try {
            guarded(c, [&](fz_context* g) { now = pdf_count_pages(g, dst); });
            if (now > before) {
                guarded(c, [&](fz_context* g) { pdf_delete_page_range(g, dst, before, now); });
            }
        } catch (const Error&) {
            dst_ = nullptr;  // cannot trust it now; the Merger refuses further use
            pdf_drop_document(c, dst);
        }
        throw;
    }
}

MergeResult Merger::finish(const std::string& output) {
    require_open();
    if (result_.inputs_merged == 0) {
        throw Error(0, "merge needs at least one input");
    }
    fz_context* c = ctx_->raw();
    pdf_write_options opts = write_options(options_);
    pdf_document* doc = dst_;
    detail::refuse_directory_output(output);
    const char* out = output.c_str();
    guarded(c, [&](fz_context* g) { pdf_save_document(g, doc, out, &opts); });

    MergeResult result = result_;
    std::error_code ec;
    const auto size = std::filesystem::file_size(output, ec);
    result.output_bytes = ec ? 0 : static_cast<std::size_t>(size);
    return result;
}

MergeResult Merger::finish_fd(int fd) {
    require_open();
    if (result_.inputs_merged == 0) {
        throw Error(0, "merge needs at least one input");
    }
    detail::write_pdf_fd(ctx_->raw(), dst_, fd, write_options(options_));
    MergeResult result = result_;
    result.output_bytes = detail::fd_size(fd);
    return result;
}

MergeResult merge(const Context& ctx, const std::vector<std::string>& inputs,
                  const std::string& output, const MergeOptions& options) {
    if (inputs.empty()) {
        throw Error(0, "merge needs at least one input");
    }
    Merger merger(ctx, options);
    for (const std::string& input : inputs) {
        merger.add(input);
    }
    return merger.finish(output);
}

}  // namespace leht::ops
