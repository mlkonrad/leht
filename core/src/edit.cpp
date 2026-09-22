// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/edit.hpp"

#include "leht/ops/pages.hpp"

#include <algorithm>

namespace leht {

std::vector<int> page_set(const std::string& spec, int page_count) {
    std::vector<int> pages = ops::parse_page_ranges(spec, page_count);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

}  // namespace leht
