// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal header: includes MuPDF's C API with our stricter warnings relaxed.
// MuPDF is a C library and its headers and macros trip -Wold-style-cast and the
// conversion warnings we build with. Never include this from a public header.
#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#pragma GCC diagnostic pop
