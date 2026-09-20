// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/page_cache.hpp"
#include "test_harness.hpp"

#include <memory>
#include <utility>

using leht::Bitmap;
using leht::PageCache;

namespace {

/// A bitmap of a known byte size, so budget arithmetic is exact.
Bitmap make_bitmap(std::size_t bytes, std::uint8_t fill = 0x00) {
    Bitmap b;
    b.width = 1;
    b.height = static_cast<int>(bytes);
    b.stride = 1;
    b.channels = 1;
    b.pixels.assign(bytes, fill);
    return b;
}

void miss_then_hit() {
    PageCache cache{1024};
    CHECK(cache.get(0, 1.0F) == nullptr);
    CHECK(cache.misses() == 1);

    cache.put(0, 1.0F, 0, make_bitmap(100));
    const auto got = cache.get(0, 1.0F);
    CHECK(got != nullptr);
    CHECK(cache.hits() == 1);
    CHECK(cache.bytes() == 100);
    CHECK(cache.count() == 1);
}

/// The reason zoom is bucketed: a scroll handler recomputing zoom in floating
/// point must not miss the cache on every single frame.
void zoom_is_bucketed() {
    PageCache cache{1024};
    cache.put(0, 1.5F, 0, make_bitmap(50));

    CHECK(cache.get(0, 1.5F) != nullptr);
    CHECK(cache.get(0, 1.4999998F) != nullptr);   // same bucket
    CHECK(cache.get(0, 1.5000001F) != nullptr);   // same bucket
    CHECK(cache.get(0, 1.6F) == nullptr);         // genuinely different
}

void rotation_and_page_are_distinct_keys() {
    PageCache cache{1024};
    cache.put(0, 1.0F, 0, make_bitmap(10));
    CHECK(cache.get(0, 1.0F, 90) == nullptr);
    CHECK(cache.get(1, 1.0F, 0) == nullptr);
    CHECK(cache.get(0, 1.0F, 0) != nullptr);
    // 360 must normalise onto 0.
    CHECK(cache.get(0, 1.0F, 360) != nullptr);
}

void evicts_least_recently_used_to_meet_budget() {
    PageCache cache{250};
    cache.put(0, 1.0F, 0, make_bitmap(100));
    cache.put(1, 1.0F, 0, make_bitmap(100));

    // Touch page 0 so page 1 becomes the eviction victim.
    CHECK(cache.get(0, 1.0F) != nullptr);

    cache.put(2, 1.0F, 0, make_bitmap(100));
    CHECK(cache.bytes() <= 250);
    CHECK(cache.get(0, 1.0F) != nullptr);   // recently used, kept
    CHECK(cache.get(1, 1.0F) == nullptr);   // evicted
    CHECK(cache.get(2, 1.0F) != nullptr);
}

/// An evicted bitmap must stay alive while the viewer is still blitting it.
void evicted_bitmap_survives_via_shared_ptr() {
    PageCache cache{150};
    const auto held = cache.put(0, 1.0F, 0, make_bitmap(100, 0x7F));
    CHECK(held != nullptr);

    cache.put(1, 1.0F, 0, make_bitmap(100));  // forces eviction of page 0
    CHECK(cache.get(0, 1.0F) == nullptr);     // gone from the cache

    CHECK(held->pixels.size() == 100);        // still valid for its owner
    CHECK(held->pixels[0] == 0x7F);
}

/// A page too big for the whole budget must be returned, not cached, and must
/// not flush everything else on the way out.
void oversized_bitmap_is_not_cached() {
    PageCache cache{100};
    cache.put(0, 1.0F, 0, make_bitmap(50));

    const auto big = cache.put(1, 1.0F, 0, make_bitmap(500));
    CHECK(big != nullptr);
    CHECK(big->pixels.size() == 500);
    CHECK(cache.get(1, 1.0F) == nullptr);
    CHECK(cache.get(0, 1.0F) != nullptr);  // survivor
}

/// Re-putting a key must replace, not double-count, its bytes.
void replacing_a_key_does_not_leak_bytes() {
    PageCache cache{1024};
    cache.put(0, 1.0F, 0, make_bitmap(100));
    cache.put(0, 1.0F, 0, make_bitmap(100));
    CHECK(cache.bytes() == 100);
    CHECK(cache.count() == 1);
}

void shrinking_budget_evicts_immediately() {
    PageCache cache{1000};
    cache.put(0, 1.0F, 0, make_bitmap(400));
    cache.put(1, 1.0F, 0, make_bitmap(400));
    CHECK(cache.bytes() == 800);

    cache.set_budget(500);
    CHECK(cache.bytes() <= 500);
    CHECK(cache.budget() == 500);
}

void clear_resets_everything() {
    PageCache cache{1024};
    cache.put(0, 1.0F, 0, make_bitmap(100));
    cache.clear();
    CHECK(cache.count() == 0);
    CHECK(cache.bytes() == 0);

    cache.reset_stats();
    CHECK(cache.hits() == 0);
    CHECK(cache.misses() == 0);
}

}  // namespace

int main() {
    RUN(miss_then_hit);
    RUN(zoom_is_bucketed);
    RUN(rotation_and_page_are_distinct_keys);
    RUN(evicts_least_recently_used_to_meet_budget);
    RUN(evicted_bitmap_survives_via_shared_ptr);
    RUN(oversized_bitmap_is_not_cached);
    RUN(replacing_a_key_does_not_leak_bytes);
    RUN(shrinking_budget_evicts_immediately);
    RUN(clear_resets_everything);
    return 0;
}
