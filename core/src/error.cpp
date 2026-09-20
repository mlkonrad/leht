// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/error.hpp"

#include "mupdf_c.hpp"

#include <cstddef>
#include <cstring>

namespace {
constexpr std::size_t kMessageMax = 512;
}  // namespace

// This frame is deliberately written like C. fz_throw() longjmps back into it,
// and a longjmp skips destructors, so it must contain no object that has one.
// Every local here is a POD; the std::string is constructed only in the throw
// below, which runs after the fz_try/fz_catch construct has fully unwound and
// MuPDF's exception stack has been popped by fz_catch.
//
// setjmp note: locals modified between setjmp and longjmp would need to be
// volatile (or fz_var'd). Nothing here is written inside the fz_try block --
// `failed`, `code` and `message` are only assigned in fz_catch, which runs
// after the jump -- so no volatile is required.
void leht::detail::run_guarded(fz_context* ctx,
                               void (*fn)(fz_context*, void*),
                               void* arg) {
    int failed = 0;
    int code = 0;
    char message[kMessageMax];
    message[0] = '\0';

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    fz_try(ctx) {
        fn(ctx, arg);
    }
    fz_catch(ctx) {
        failed = 1;
        code = fz_caught(ctx);
        const char* caught = fz_caught_message(ctx);
        if (caught == nullptr) {
            caught = "unknown MuPDF error";
        }
        std::strncpy(message, caught, kMessageMax - 1);
        message[kMessageMax - 1] = '\0';
    }
#pragma GCC diagnostic pop

    // Do not fold this into fz_catch: throwing from inside the macro construct
    // would unwind out of it before fz_do_catch() finishes popping the stack.
    if (failed != 0) {
        throw Error(code, message);
    }
}
