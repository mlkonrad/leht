// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/renderer.hpp"

#include <cstddef>
#include <memory>

namespace leht {

/// Byte-budgeted LRU cache of rendered pages.
///
/// Keyed on (page, zoom bucket, rotation). Zoom is bucketed to 1/1000 so that
/// floating-point noise cannot silently produce a cache miss on every frame --
/// a scroll handler recomputing zoom as 1.4999998 must still hit.
///
/// Entries are handed out as shared_ptr because the viewer may still be
/// blitting a bitmap when eviction removes it from the cache. Refcounting keeps
/// that bitmap alive until the last user drops it.
///
/// NOT thread-safe. One cache per render thread; see docs/threading.md.
class PageCache {
public:
    static constexpr std::size_t kDefaultBudget = std::size_t{256} << 20;  // 256 MB

    explicit PageCache(std::size_t budget_bytes = kDefaultBudget);

    /// Returns the cached bitmap, or nullptr on a miss. A hit is promoted to
    /// most-recently-used.
    [[nodiscard]] std::shared_ptr<const Bitmap> get(int page, float zoom,
                                                    int rotation = 0);

    /// Inserts a bitmap and returns it, evicting least-recently-used entries
    /// until the budget is met. A bitmap larger than the whole budget is
    /// returned without being cached rather than emptying the cache for it.
    std::shared_ptr<const Bitmap> put(int page, float zoom, int rotation,
                                      Bitmap bitmap);

    void clear() noexcept;

    /// Shrinks to fit immediately if the new budget is smaller.
    void set_budget(std::size_t bytes);

    [[nodiscard]] std::size_t budget() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;

    /// Hit rate is the number worth watching when tuning prerender distance.
    [[nodiscard]] std::size_t hits() const noexcept;
    [[nodiscard]] std::size_t misses() const noexcept;
    void reset_stats() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace leht
