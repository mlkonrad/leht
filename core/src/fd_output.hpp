// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal header: writing a PDF into a file descriptor instead of a path.
// This is how the sandboxed worker produces files: the viewer opens (or
// creates) the path and passes the descriptor, and the worker only writes.
#pragma once

#include "mupdf_c.hpp"

#include <cstddef>

namespace leht::detail {

/// An fz_output over `fd`, which it BORROWS: dropping the output leaves the
/// descriptor open. Only write, lseek, pread and fstat are used, which is what
/// the worker's sandbox allows. Call inside guarded(); it throws MuPDF-style.
fz_output* new_fd_output(fz_context* ctx, int fd);

/// Writes `pdf` in full into `fd` with `opts`, then closes the output. Must not
/// be used for incremental saves: see Document::save_fd() for those.
void write_pdf_fd(fz_context* ctx, pdf_document* pdf, int fd,
                  const pdf_write_options& opts);

/// The size of the file behind `fd`, or 0 if it cannot be read.
std::size_t fd_size(int fd) noexcept;

}  // namespace leht::detail
