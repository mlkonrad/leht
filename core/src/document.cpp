// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/document.hpp"

#include "guards.hpp"
#include "mupdf_c.hpp"
#include "leht/context.hpp"
#include "leht/error.hpp"

#include <utility>
#include <vector>

namespace leht {

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

Document::Document(Document&& other) noexcept
    : ctx_(std::exchange(other.ctx_, nullptr)),
      doc_(std::exchange(other.doc_, nullptr)) {}

Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) {
        if (doc_ != nullptr) {
            fz_drop_document(ctx_, doc_);
        }
        ctx_ = std::exchange(other.ctx_, nullptr);
        doc_ = std::exchange(other.doc_, nullptr);
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

}  // namespace leht
