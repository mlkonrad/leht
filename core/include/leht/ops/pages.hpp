// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace leht {
class Context;
}

namespace leht::ops {

/// Parses a page range specification into 0-based page indices.
///
/// Accepts 1-based, inclusive ranges: "1-5,8,10-" or "3". A trailing "-" runs
/// to the last page. "N-M" with N > M counts backwards, so "5-1" reverses.
/// Duplicates are preserved, because "1,1,2" is a legitimate way to repeat a
/// page. An empty spec means every page.
///
/// Throws leht::Error on malformed input or an out-of-bounds page.
[[nodiscard]] std::vector<int> parse_page_ranges(const std::string& spec,
                                                 int page_count);

struct PagesResult {
    int pages_written = 0;
    std::size_t output_bytes = 0;
};

/// Writes only the pages named by `ranges`, in the order given.
PagesResult extract(const Context& ctx, const std::string& input,
                    const std::string& output, const std::string& ranges);

/// Writes everything EXCEPT the pages named by `ranges`.
PagesResult remove_pages(const Context& ctx, const std::string& input,
                         const std::string& output, const std::string& ranges);

/// Rotates the named pages by `degrees` (a multiple of 90, may be negative),
/// relative to their current rotation. Other pages are copied unchanged.
PagesResult rotate(const Context& ctx, const std::string& input,
                   const std::string& output, const std::string& ranges,
                   int degrees);

/// Splits into one file per `pages_per_file` pages. `output_pattern` must
/// contain a single printf-style integer field, e.g. "chapter-%03d.pdf".
/// Returns the paths written, in order.
std::vector<std::string> split(const Context& ctx, const std::string& input,
                               const std::string& output_pattern,
                               int pages_per_file = 1);

}  // namespace leht::ops
