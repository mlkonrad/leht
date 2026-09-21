// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Document::open_fd -- the entry point the sandboxed worker uses. It must be
// indistinguishable from open() for the viewer's purposes, and it must never
// leak or double-close the descriptor it takes ownership of.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/renderer.hpp"
#include "test_harness.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>

using leht::Context;
using leht::Document;
using leht::Renderer;

namespace {

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

int open_ro(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

bool is_closed(int fd) { return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF; }

template <typename F>
bool throws_leht_error(F&& fn) {
    try {
        fn();
    } catch (const leht::Error&) {
        return true;
    }
    return false;
}

void renders_identically_to_open() {
    Context ctx;
    Document by_path = Document::open(ctx, corpus("text_10p.pdf"));
    Document by_fd = Document::open_fd(ctx, open_ro(corpus("text_10p.pdf")));
    CHECK(by_fd.page_count() == by_path.page_count());

    Renderer a{ctx, by_path};
    Renderer b{ctx, by_fd};
    for (int page : {0, 4, 9}) {
        auto x = a.render(page, 1.5F, 90);
        auto y = b.render(page, 1.5F, 90);
        CHECK(x && y);
        CHECK(x->width == y->width && x->height == y->height);
        CHECK(x->pixels == y->pixels);
    }
}

void outline_matches_open() {
    Context ctx;
    Document by_path = Document::open(ctx, corpus("outlined.pdf"));
    Document by_fd = Document::open_fd(ctx, open_ro(corpus("outlined.pdf")));
    const auto x = by_path.outline();
    const auto y = by_fd.outline();
    CHECK(!x.empty() && x.size() == y.size());
    CHECK(x.front().title == y.front().title && x.front().page == y.front().page);
}

void file_position_is_irrelevant() {
    // The viewer's copy of the descriptor may have been read from; pread()
    // at explicit offsets makes that harmless.
    Context ctx;
    const int fd = open_ro(corpus("text_10p.pdf"));
    CHECK(::lseek(fd, 1000, SEEK_SET) == 1000);
    Document doc = Document::open_fd(ctx, fd);
    CHECK(doc.page_count() == 10);
}

void descriptor_is_closed_with_the_document() {
    Context ctx;
    const int fd = open_ro(corpus("text_10p.pdf"));
    {
        Document doc = Document::open_fd(ctx, fd);
        Renderer r{ctx, doc};
        CHECK(r.render(0, 0.5F).has_value());
        CHECK(!is_closed(fd));
    }
    CHECK(is_closed(fd));
}

void descriptor_is_closed_on_failure() {
    Context ctx;

    // Force the PDF handler onto a PNG. MuPDF may reject it or "repair" it
    // into something; either way, once nothing holds the Document the fd must
    // be closed -- exactly once.
    const int garbage = open_ro(corpus("page.png"));
    try {
        (void)Document::open_fd(ctx, garbage, "pdf");
    } catch (const leht::Error&) {
    }
    CHECK(is_closed(garbage));

    const int dir = ::open(LEHT_CORPUS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 0);
    CHECK(throws_leht_error([&] { Document::open_fd(ctx, dir); }));
    CHECK(is_closed(dir));

    CHECK(throws_leht_error([&] { Document::open_fd(ctx, -1); }));
}

void magic_selects_the_handler() {
    Context ctx;
    Document img = Document::open_fd(ctx, open_ro(corpus("page.png")), "page.png");
    CHECK(img.page_count() == 1);
}

void encrypted_documents_authenticate() {
    Context ctx;
    Document doc = Document::open_fd(ctx, open_ro(corpus("locked.pdf")));
    CHECK(doc.needs_password());
    CHECK(!doc.authenticate("wrong"));
    CHECK(doc.authenticate("s3cret"));
    CHECK(doc.page_count() == 10);
}

}  // namespace

int main() {
    RUN(renders_identically_to_open);
    RUN(outline_matches_open);
    RUN(file_position_is_irrelevant);
    RUN(descriptor_is_closed_with_the_document);
    RUN(descriptor_is_closed_on_failure);
    RUN(magic_selects_the_handler);
    RUN(encrypted_documents_authenticate);
    return 0;
}
