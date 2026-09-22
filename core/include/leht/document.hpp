// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <string_view>

typedef struct fz_context fz_context;
typedef struct fz_document fz_document;

namespace leht {

class Context;
struct SaveOptions;

/// One entry in a document's outline (its table of contents / bookmarks).
struct OutlineItem {
    std::string title;
    int page = -1;      ///< 0-based target page, or -1 if the entry has no page
    float y = 0.0F;     ///< target offset down the page, in unscaled points
    std::vector<OutlineItem> children;
};

/// An open document.
///
/// Borrows its Context: the Context MUST outlive every Document opened from it.
///
/// NOT thread-safe, and this is a MuPDF constraint rather than a choice -- only
/// one thread may touch a document at a time. Render concurrently by building a
/// display list here, then rasterising it on threads holding their own clones.
class Document {
public:
    /// Opens `path`. Throws leht::Error if the file is missing, unreadable or
    /// not a format MuPDF handles. The file is streamed, not read into memory,
    /// so opening a large document stays cheap.
    static Document open(const Context& ctx, const std::string& path);

    /// Opens a document already in memory. The bytes are copied, so the
    /// caller's buffer need not outlive the Document.
    ///
    /// `magic` hints at the format ("pdf", "application/pdf", or a filename);
    /// empty lets MuPDF sniff it. Throws leht::Error on anything unreadable,
    /// which is what makes this the right entry point for fuzzing.
    static Document open_memory(const Context& ctx, const void* data,
                                std::size_t size,
                                const std::string& magic = "pdf");

    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    ~Document();

    /// Opens the document behind an already-open file descriptor, TAKING
    /// OWNERSHIP of `fd`: it is closed when the Document is destroyed, or
    /// before this throws. Reads use pread() at explicit offsets, so the fd's
    /// own file position is irrelevant.
    ///
    /// This is how the sandboxed worker opens files: the viewer opens the
    /// path and passes the descriptor, so the worker needs no filesystem
    /// access at all. `magic` is as for open_memory(); passing the file name
    /// lets MuPDF pick the handler the same way open() would.
    static Document open_fd(const Context& ctx, int fd,
                            const std::string& magic = "pdf");

    [[nodiscard]] int page_count() const;

    /// True when the document is encrypted and no usable password has been
    /// supplied yet. Page access before authenticating will throw.
    [[nodiscard]] bool needs_password() const;

    /// Returns true if `password` unlocks the document.
    [[nodiscard]] bool authenticate(std::string_view password);

    /// Metadata by MuPDF key ("format", "info:Title", "info:Author", ...).
    /// Returns nullopt when the key is absent.
    [[nodiscard]] std::optional<std::string> metadata(const std::string& key) const;

    /// The document outline (bookmarks / table of contents), as a tree. Empty
    /// when the document has none. Page numbers are 0-based and resolved to
    /// absolute page indices.
    [[nodiscard]] std::vector<OutlineItem> outline() const;

    /// True when the document is a PDF, the only format the edit operations
    /// accept. XPS, EPUB and images open for viewing but not for editing.
    [[nodiscard]] bool is_pdf() const;

    /// Writes the document, with every edit applied so far, to `path`.
    ///
    /// Always a full rewrite, never an incremental update. The bytes go to a
    /// temporary file beside `path` which is fsynced and then renamed over
    /// it, so a failed save never leaves a half-written file behind -- and
    /// saving over the very file the document was opened from is safe, since
    /// the document keeps reading the old inode.
    ///
    /// Throws leht::Error if the document is not a PDF or cannot be written.
    void save(const std::string& path, const SaveOptions& options) const;

    /// As save(), but writes into `fd` from its current offset. Does NOT take
    /// ownership: the caller owns the descriptor, and with it the job of
    /// making the result durable (fsync) and atomic (rename). This is how the
    /// sandboxed worker saves, since it cannot open paths.
    void save_fd(int fd, const SaveOptions& options) const;

    /// Records that content has been redacted from this document. Every later
    /// save then garbage-collects, whatever SaveOptions asks for:
    /// without collection, the unreferenced original content streams would
    /// still be written out, and the redaction would be cosmetic. Called by
    /// ops::redact(); it only ever makes a save stricter.
    void mark_redacted() noexcept { redacted_ = true; }
    [[nodiscard]] bool redacted() const noexcept { return redacted_; }

    [[nodiscard]] fz_document* raw() const noexcept { return doc_; }

private:
    Document(fz_context* ctx, fz_document* doc) noexcept;

    fz_context* ctx_ = nullptr;  // borrowed, not owned
    fz_document* doc_ = nullptr;
    bool redacted_ = false;
};

}  // namespace leht
