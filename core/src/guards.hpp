// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal header: RAII wrappers for MuPDF's reference-counted types.
//
// These MUST be declared in the outer frame, never inside a guarded() lambda.
// A MuPDF longjmp skips destructors; it lands in run_guarded(), which throws a
// normal C++ exception, and ordinary unwinding then runs these destructors
// correctly. Assign into one via slot() from inside the lambda.
#pragma once

#include "mupdf_c.hpp"

#include "leht/error.hpp"

#include <filesystem>
#include <string>
#include <utility>

namespace leht::detail {

template <typename T, void (*DropFn)(fz_context*, T*)>
class Owned {
public:
    Owned() = default;
    explicit Owned(fz_context* ctx) noexcept : ctx_(ctx) {}
    Owned(fz_context* ctx, T* ptr) noexcept : ctx_(ctx), ptr_(ptr) {}

    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    Owned(Owned&& other) noexcept
        : ctx_(other.ctx_), ptr_(std::exchange(other.ptr_, nullptr)) {}

    Owned& operator=(Owned&& other) noexcept {
        if (this != &other) {
            reset();
            ctx_ = other.ctx_;
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }

    ~Owned() { reset(); }

    void reset() noexcept {
        if (ptr_ != nullptr) {
            DropFn(ctx_, ptr_);
            ptr_ = nullptr;
        }
    }

    [[nodiscard]] T* get() const noexcept { return ptr_; }

    /// Write target for use inside a guarded() lambda: the pointer lands in
    /// this outer-frame object, so it is released even if MuPDF throws later.
    [[nodiscard]] T** slot() noexcept { return &ptr_; }

    [[nodiscard]] T* release() noexcept { return std::exchange(ptr_, nullptr); }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    fz_context* ctx_ = nullptr;
    T* ptr_ = nullptr;
};

using OwnedBuffer = Owned<fz_buffer, fz_drop_buffer>;
using OwnedImage = Owned<fz_image, fz_drop_image>;
using OwnedPdfObj = Owned<pdf_obj, pdf_drop_obj>;
using OwnedPdfDoc = Owned<pdf_document, pdf_drop_document>;
using OwnedFzDoc = Owned<fz_document, fz_drop_document>;

/// Call before any MuPDF save to a path (pdf_save_document,
/// fz_save_pixmap_as_png): MuPDF opens its output by remove()-ing the path
/// first, and remove() deletes an EMPTY DIRECTORY. `-o somedir` would replace
/// the directory with the output file -- CI once lost /tmp this way.
inline void refuse_directory_output(const std::string& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        throw Error(0, path + " is a directory; give a file name to write to");
    }
}

}  // namespace leht::detail
