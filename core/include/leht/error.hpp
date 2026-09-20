// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

typedef struct fz_context fz_context;

namespace leht {

/// Raised when a MuPDF call fails. `code` is the value reported by fz_caught().
class Error : public std::runtime_error {
public:
    Error(int code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    [[nodiscard]] int code() const noexcept { return code_; }

private:
    int code_;
};

namespace detail {
/// The ONLY function in this codebase permitted to use fz_try/fz_catch.
/// Defined in error.cpp; see that file for why it is shaped this way.
void run_guarded(fz_context* ctx, void (*fn)(fz_context*, void*), void* arg);
}  // namespace detail

/// Runs `fn` with MuPDF errors translated into a thrown leht::Error.
///
/// ---------------------------------------------------------------------------
/// CONTRACT - read before writing a lambda for this.
///
/// MuPDF reports errors via fz_throw(), which is a longjmp. A longjmp across a
/// C++ frame does NOT run destructors, so any RAII object alive in the jumped
/// frames leaks or corrupts. guarded() confines the fz_try/fz_catch construct
/// to a single destructor-free frame inside error.cpp, and throws the C++
/// exception only after that construct has fully unwound.
///
/// Your callable therefore must:
///   * hold only trivially-destructible locals (raw pointers, PODs, references)
///   * NOT construct std::string, std::vector, unique_ptr, or any RAII type
///   * NOT throw a C++ exception of its own
///   * contain raw fz_* / pdf_* calls and little else
///
/// Capture results by reference and move them into RAII types AFTER it returns:
///
///     fz_document* doc = nullptr;
///     guarded(ctx, [&](fz_context* c) { doc = fz_open_document(c, path); });
///     DocumentHandle owned{ctx, doc};   // safe: we are past the longjmp
/// ---------------------------------------------------------------------------
template <typename F>
void guarded(fz_context* ctx, F&& fn) {
    using Callable = std::remove_reference_t<F>;
    static_assert(std::is_invocable_v<Callable&, fz_context*>,
                  "callable passed to guarded() must accept an fz_context*");

    detail::run_guarded(
        ctx,
        [](fz_context* c, void* p) { (*static_cast<Callable*>(p))(c); },
        static_cast<void*>(std::addressof(fn)));
}

}  // namespace leht
