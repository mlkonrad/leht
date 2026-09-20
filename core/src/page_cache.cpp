// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/page_cache.hpp"

#include <cmath>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>

namespace leht {

namespace {

/// Packs the cache key into one integer: page (32 bits) | zoom bucket (24) |
/// rotation quadrant (8). Avoids needing a custom hash for a struct key.
std::uint64_t make_key(int page, float zoom, int rotation) {
    const auto p = static_cast<std::uint64_t>(static_cast<std::uint32_t>(page));

    const long bucket = std::lround(static_cast<double>(zoom) * 1000.0);
    const auto z = static_cast<std::uint64_t>(bucket < 0 ? 0 : bucket) & 0xFFFFFFU;

    const int normalised = (((rotation % 360) + 360) % 360) / 90;
    const auto r = static_cast<std::uint64_t>(normalised) & 0x3U;

    return (p << 32) | (z << 8) | r;
}

std::size_t bitmap_bytes(const Bitmap& b) { return b.pixels.size(); }

}  // namespace

struct PageCache::Impl {
    std::size_t budget = kDefaultBudget;
    std::size_t used = 0;
    std::size_t hit_count = 0;
    std::size_t miss_count = 0;

    std::list<std::uint64_t> lru;  // front = most recently used
    struct Entry {
        std::shared_ptr<const Bitmap> bitmap;
        std::list<std::uint64_t>::iterator position;
        std::size_t bytes;
    };
    std::unordered_map<std::uint64_t, Entry> entries;

    void evict_to_budget() noexcept {
        while (used > budget && !lru.empty()) {
            const std::uint64_t victim = lru.back();
            lru.pop_back();
            if (const auto it = entries.find(victim); it != entries.end()) {
                used -= it->second.bytes;
                entries.erase(it);
            }
        }
    }
};

PageCache::PageCache(std::size_t budget_bytes)
    : impl_(std::make_shared<Impl>()) {
    impl_->budget = budget_bytes;
}

std::shared_ptr<const Bitmap> PageCache::get(int page, float zoom,
                                             int rotation) {
    const std::uint64_t key = make_key(page, zoom, rotation);
    const auto it = impl_->entries.find(key);
    if (it == impl_->entries.end()) {
        ++impl_->miss_count;
        return nullptr;
    }
    ++impl_->hit_count;
    impl_->lru.splice(impl_->lru.begin(), impl_->lru, it->second.position);
    return it->second.bitmap;
}

std::shared_ptr<const Bitmap> PageCache::put(int page, float zoom, int rotation,
                                             Bitmap bitmap) {
    const std::uint64_t key = make_key(page, zoom, rotation);
    const std::size_t size = bitmap_bytes(bitmap);
    auto shared = std::make_shared<const Bitmap>(std::move(bitmap));

    // Replacing an existing key must not double-count its bytes.
    if (const auto existing = impl_->entries.find(key);
        existing != impl_->entries.end()) {
        impl_->used -= existing->second.bytes;
        impl_->lru.erase(existing->second.position);
        impl_->entries.erase(existing);
    }

    // One oversized page must not evict everything else for nothing.
    if (size > impl_->budget) {
        return shared;
    }

    impl_->lru.push_front(key);
    impl_->entries.emplace(
        key, Impl::Entry{shared, impl_->lru.begin(), size});
    impl_->used += size;
    impl_->evict_to_budget();
    return shared;
}

void PageCache::clear() noexcept {
    impl_->entries.clear();
    impl_->lru.clear();
    impl_->used = 0;
}

void PageCache::set_budget(std::size_t bytes) {
    impl_->budget = bytes;
    impl_->evict_to_budget();
}

std::size_t PageCache::budget() const noexcept { return impl_->budget; }
std::size_t PageCache::bytes() const noexcept { return impl_->used; }
std::size_t PageCache::count() const noexcept { return impl_->entries.size(); }
std::size_t PageCache::hits() const noexcept { return impl_->hit_count; }
std::size_t PageCache::misses() const noexcept { return impl_->miss_count; }

void PageCache::reset_stats() noexcept {
    impl_->hit_count = 0;
    impl_->miss_count = 0;
}

}  // namespace leht
