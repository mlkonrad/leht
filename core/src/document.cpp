// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/document.hpp"

#include "guards.hpp"
#include "mupdf_c.hpp"
#include "leht/context.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

namespace leht {

namespace {

/// State of an fz_stream reading from a file descriptor. Plain C layout: it is
/// allocated with fz_calloc and freed by fd_drop, both on MuPDF's side.
struct FdStream {
    int fd;
    std::int64_t offset;  ///< next byte pread() will fetch
    std::int64_t size;
    unsigned char buffer[8192];
};

// The three callbacks below are called from inside MuPDF, in C frames, where
// fz_throw is the error channel MuPDF expects. They hold only trivially
// destructible locals, so a longjmp out of them is safe.

int fd_next(fz_context* ctx, fz_stream* stm, std::size_t /*max*/) {
    auto* s = static_cast<FdStream*>(stm->state);
    ssize_t n = 0;
    do {
        n = ::pread(s->fd, s->buffer, sizeof(s->buffer), s->offset);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "read error on document descriptor");
    }
    s->offset += n;
    stm->rp = s->buffer;
    stm->wp = s->buffer + n;
    stm->pos = s->offset;
    if (n == 0) {
        return EOF;
    }
    return *stm->rp++;
}

void fd_seek(fz_context* ctx, fz_stream* stm, std::int64_t offset, int whence) {
    auto* s = static_cast<FdStream*>(stm->state);
    std::int64_t target = offset;
    if (whence == SEEK_CUR) {
        target = stm->pos - (stm->wp - stm->rp) + offset;
    } else if (whence == SEEK_END) {
        target = s->size + offset;
    }
    if (target < 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot seek before the start of the document");
    }
    s->offset = target;
    stm->pos = target;
    stm->rp = s->buffer;
    stm->wp = s->buffer;
}

void fd_drop(fz_context* ctx, void* state) {
    auto* s = static_cast<FdStream*>(state);
    ::close(s->fd);
    fz_free(ctx, s);
}

// fz_output over a borrowed descriptor, for Document::save_fd(). The state is
// the fd itself, stored in the pointer, so there is nothing to allocate or
// free. Only write/lseek are used: the worker's sandbox allows exactly those.

int output_fd(void* state) {
    return static_cast<int>(reinterpret_cast<std::intptr_t>(state));
}

void out_write(fz_context* ctx, void* state, const void* data, std::size_t n) {
    const int fd = output_fd(state);
    const auto* p = static_cast<const unsigned char*>(data);
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            fz_throw(ctx, FZ_ERROR_SYSTEM, "write error on output descriptor");
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

void out_seek(fz_context* ctx, void* state, std::int64_t offset, int whence) {
    if (::lseek(output_fd(state), offset, whence) < 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot seek on output descriptor");
    }
}

std::int64_t out_tell(fz_context* ctx, void* state) {
    const off_t pos = ::lseek(output_fd(state), 0, SEEK_CUR);
    if (pos < 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot tell on output descriptor");
    }
    return pos;
}

/// The process umask, read without changing it: umask(2) can only be queried by
/// setting it, which would race with any other thread creating files.
mode_t current_umask() {
    std::FILE* f = std::fopen("/proc/self/status", "re");
    unsigned mask = 022;
    if (f != nullptr) {
        char line[256];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            if (std::sscanf(line, "Umask: %o", &mask) == 1) {
                break;
            }
        }
        std::fclose(f);
    }
    return static_cast<mode_t>(mask);
}

pdf_write_options write_options(const SaveOptions& options, bool redacted) {
    pdf_write_options opts = pdf_default_write_options;
    opts.do_garbage = options.garbage;
    // A redaction removes content from the page's content stream, but the old
    // stream object is still in the xref. Only collection drops it.
    if (redacted && opts.do_garbage < 3) {
        opts.do_garbage = 3;
    }
    opts.do_incremental = 0;
    opts.do_compress = options.compress_streams ? 1 : 0;
    opts.do_compress_images = 1;
    opts.do_compress_fonts = options.compress_streams ? 1 : 0;
    opts.do_linear = options.linearize ? 1 : 0;
    return opts;
}

}  // namespace

Document::Document(fz_context* ctx, fz_document* doc) noexcept
    : ctx_(ctx), doc_(doc) {}

Document Document::open(const Context& ctx, const std::string& path) {
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot open a document from a moved-from Context");
    }

    // The lambda holds only a raw pointer and a const char*, both trivially
    // destructible, so a longjmp out of fz_open_document is safe here.
    fz_document* doc = nullptr;
    const char* cpath = path.c_str();
    guarded(c, [&](fz_context* g) { doc = fz_open_document(g, cpath); });

    if (doc == nullptr) {
        throw Error(0, "fz_open_document returned null for: " + path);
    }
    return Document{c, doc};  // ownership taken after the jump, as required
}

Document Document::open_memory(const Context& ctx, const void* data,
                               std::size_t size, const std::string& magic) {
    fz_context* c = ctx.raw();
    if (c == nullptr) {
        throw Error(0, "cannot open a document from a moved-from Context");
    }
    if (data == nullptr || size == 0) {
        throw Error(0, "cannot open an empty buffer as a document");
    }

    // Copy the bytes: fz_open_memory borrows its input, and a document outliving
    // the caller's buffer would be a use-after-free waiting to happen.
    fz_buffer* buffer = nullptr;
    const auto* bytes = static_cast<const unsigned char*>(data);
    guarded(c, [&](fz_context* g) {
        buffer = fz_new_buffer_from_copied_data(g, bytes, size);
    });
    detail::OwnedBuffer owned_buffer{c, buffer};
    if (buffer == nullptr) {
        throw Error(0, "could not copy the input buffer");
    }

    fz_stream* stream = nullptr;
    guarded(c, [&](fz_context* g) { stream = fz_open_buffer(g, buffer); });
    detail::Owned<fz_stream, fz_drop_stream> owned_stream{c, stream};
    if (stream == nullptr) {
        throw Error(0, "could not open a stream over the input buffer");
    }

    fz_document* doc = nullptr;
    const char* hint = magic.empty() ? nullptr : magic.c_str();
    guarded(c, [&](fz_context* g) {
        doc = fz_open_document_with_stream(g, hint, stream);
    });
    if (doc == nullptr) {
        throw Error(0, "fz_open_document_with_stream returned null");
    }
    return Document{c, doc};
}

Document Document::open_fd(const Context& ctx, int fd, const std::string& magic) {
    fz_context* c = ctx.raw();
    if (c == nullptr || fd < 0) {
        if (fd >= 0) {
            ::close(fd);
        }
        throw Error(0, c == nullptr
                           ? "cannot open a document from a moved-from Context"
                           : "invalid file descriptor");
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        throw Error(0, "document descriptor is not a regular file");
    }

    FdStream* state = nullptr;
    try {
        guarded(c, [&](fz_context* g) {
            state = static_cast<FdStream*>(fz_calloc(g, 1, sizeof(FdStream)));
        });
    } catch (...) {
        ::close(fd);
        throw;
    }
    state->fd = fd;
    state->offset = 0;
    state->size = st.st_size;

    // From here the fd belongs to `state`: fz_new_stream drops the state (and
    // so closes the fd) itself if it fails, and the stream's drop does it
    // afterwards.
    fz_stream* stream = nullptr;
    guarded(c, [&](fz_context* g) {
        stream = fz_new_stream(g, state, fd_next, fd_drop);
        stream->seek = fd_seek;
    });
    detail::Owned<fz_stream, fz_drop_stream> owned_stream{c, stream};

    fz_document* doc = nullptr;
    const char* hint = magic.empty() ? nullptr : magic.c_str();
    guarded(c, [&](fz_context* g) {
        doc = fz_open_document_with_stream(g, hint, stream);
    });
    if (doc == nullptr) {
        throw Error(0, "fz_open_document_with_stream returned null");
    }
    return Document{c, doc};
}

Document::Document(Document&& other) noexcept
    : ctx_(std::exchange(other.ctx_, nullptr)),
      doc_(std::exchange(other.doc_, nullptr)),
      redacted_(std::exchange(other.redacted_, false)) {}

Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) {
        if (doc_ != nullptr) {
            fz_drop_document(ctx_, doc_);
        }
        ctx_ = std::exchange(other.ctx_, nullptr);
        doc_ = std::exchange(other.doc_, nullptr);
        redacted_ = std::exchange(other.redacted_, false);
    }
    return *this;
}

Document::~Document() {
    if (doc_ != nullptr) {
        fz_drop_document(ctx_, doc_);
    }
}

int Document::page_count() const {
    int count = 0;
    fz_document* doc = doc_;
    guarded(ctx_, [&](fz_context* g) { count = fz_count_pages(g, doc); });
    return count;
}

bool Document::needs_password() const {
    int needs = 0;
    fz_document* doc = doc_;
    guarded(ctx_, [&](fz_context* g) { needs = fz_needs_password(g, doc); });
    return needs != 0;
}

bool Document::authenticate(std::string_view password) {
    // fz_authenticate_password needs a NUL-terminated string; build it out here,
    // never inside the guarded lambda.
    const std::string owned{password};
    const char* pw = owned.c_str();
    fz_document* doc = doc_;

    int result = 0;
    guarded(ctx_, [&](fz_context* g) {
        result = fz_authenticate_password(g, doc, pw);
    });
    return result != 0;
}

std::optional<std::string> Document::metadata(const std::string& key) const {
    fz_document* doc = doc_;
    const char* ckey = key.c_str();

    // Probe for the required size first...
    int needed = 0;
    guarded(ctx_, [&](fz_context* g) {
        needed = fz_lookup_metadata(g, doc, ckey, nullptr, 0);
    });
    if (needed < 0) {
        return std::nullopt;
    }

    // ...allocate outside the guarded region (std::vector has a destructor)...
    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1, '\0');
    char* out = buffer.data();
    const std::size_t capacity = static_cast<std::size_t>(needed) + 1;

    // ...then fill it with a second guarded call that owns nothing.
    int written = 0;
    guarded(ctx_, [&](fz_context* g) {
        written = fz_lookup_metadata(g, doc, ckey, out, capacity);
    });
    if (written < 0) {
        return std::nullopt;
    }
    return std::string(buffer.data());
}

bool Document::is_pdf() const {
    fz_document* doc = doc_;
    pdf_document* pdf = nullptr;
    guarded(ctx_, [&](fz_context* g) { pdf = pdf_document_from_fz_document(g, doc); });
    return pdf != nullptr;
}

void Document::save_fd(int fd, const SaveOptions& options) const {
    if (doc_ == nullptr) {
        throw Error(0, "cannot save a moved-from Document");
    }
    if (fd < 0) {
        throw Error(0, "invalid output descriptor");
    }
    fz_document* doc = doc_;
    pdf_document* pdf = nullptr;
    guarded(ctx_, [&](fz_context* g) { pdf = pdf_document_from_fz_document(g, doc); });
    if (pdf == nullptr) {
        throw Error(0, "saving requires a PDF");
    }

    pdf_write_options opts = write_options(options, redacted_);
    void* state = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
    detail::Owned<fz_output, fz_drop_output> out{ctx_};
    guarded(ctx_, [&](fz_context* g) {
        *out.slot() = fz_new_output(g, 8192, state, out_write, nullptr, nullptr);
        out.get()->seek = out_seek;
        out.get()->tell = out_tell;
        pdf_write_document(g, pdf, out.get(), &opts);
        fz_close_output(g, out.get());
    });
}

void Document::save(const std::string& path, const SaveOptions& options) const {
    namespace fs = std::filesystem;
    const fs::path target{path};
    const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path{"."};
    std::string temp = (dir / ("." + target.filename().string() + ".leht-XXXXXX")).string();

    const int fd = ::mkostemp(temp.data(), O_CLOEXEC);
    if (fd < 0) {
        throw Error(0, "cannot create a temporary file beside " + path + ": " +
                           std::strerror(errno));
    }
    // mkstemp creates 0600. Overwriting keeps the file's mode; a new file gets
    // what open(2) would have given it.
    struct stat st {};
    (void)::fchmod(fd, ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777)
                                                        : (0666 & ~current_umask()));

    try {
        save_fd(fd, options);
        if (::fsync(fd) != 0) {
            throw Error(0, "cannot flush " + temp + ": " + std::strerror(errno));
        }
    } catch (...) {
        ::close(fd);
        ::unlink(temp.c_str());
        throw;
    }
    ::close(fd);
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        throw Error(0, "cannot replace " + path + ": " + std::strerror(err));
    }
    // Make the rename itself durable. Best effort: the data is already safe.
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
}

namespace {

/// Converts one fz_outline node (with its siblings and children) into leht's
/// plain tree. This allocates std::string / std::vector, so it must NOT run
/// inside a guarded() lambda. It is called after the outline is loaded, on
/// pointers MuPDF keeps alive, with no throwing fz_ call in between.
void convert(fz_context* ctx, fz_document* doc, fz_outline* node,
             std::vector<OutlineItem>& out) {
    for (; node != nullptr; node = node->next) {
        OutlineItem item;
        item.title = node->title != nullptr ? node->title : "";
        item.y = node->y;
        // A bookmark may point nowhere (a plain heading); page stays -1.
        if (node->uri != nullptr) {
            item.page = fz_page_number_from_location(ctx, doc, node->page);
        }
        if (node->down != nullptr) {
            convert(ctx, doc, node->down, item.children);
        }
        out.push_back(std::move(item));
    }
}

}  // namespace

std::vector<OutlineItem> Document::outline() const {
    fz_document* doc = doc_;

    // fz_load_outline can throw, so it is the only thing in the guard; the tree
    // walk that allocates runs after, on the returned pointer MuPDF owns until
    // we drop it.
    fz_outline* root = nullptr;
    guarded(ctx_, [&](fz_context* g) { root = fz_load_outline(g, doc); });
    if (root == nullptr) {
        return {};
    }

    std::vector<OutlineItem> result;
    try {
        convert(ctx_, doc, root, result);
    } catch (...) {
        fz_drop_outline(ctx_, root);
        throw;
    }
    fz_drop_outline(ctx_, root);
    return result;
}

}  // namespace leht
