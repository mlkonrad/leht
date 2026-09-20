// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "test_harness.hpp"

#include <string>

using leht::Context;
using leht::Document;

/// A MuPDF longjmp must surface as a C++ exception carrying a real message,
/// not as a crash, a silent null, or an abort.
static void missing_file_throws_error() {
    Context ctx;
    bool threw = false;
    try {
        Document doc = Document::open(ctx, "/nonexistent/definitely-not-here.pdf");
        (void)doc;
    } catch (const leht::Error& e) {
        threw = true;
        CHECK(std::string(e.what()).size() > 0);
    }
    CHECK(threw);
}

/// Repeated failures must leave MuPDF's exception stack balanced. If fz_catch
/// were mispaired, the second throw would corrupt state or abort.
static void repeated_failures_keep_stack_balanced() {
    Context ctx;
    for (int i = 0; i < 100; ++i) {
        try {
            Document doc = Document::open(ctx, "/nonexistent/nope.pdf");
            (void)doc;
            CHECK(false);  // must not reach
        } catch (const leht::Error&) {
            // expected
        }
    }
    CHECK(static_cast<bool>(ctx));
}

/// A file that exists but is not a document must also throw cleanly.
static void garbage_file_throws_error() {
    Context ctx;
    bool threw = false;
    try {
        Document doc = Document::open(ctx, "/etc/hostname");
        (void)doc;
    } catch (const leht::Error&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    RUN(missing_file_throws_error);
    RUN(repeated_failures_keep_stack_balanced);
    RUN(garbage_file_throws_error);
    return 0;
}
