// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <memory>

typedef struct fz_context fz_context;

namespace leht {

/// Owns an fz_context together with the mutex set that protects MuPDF's shared
/// global state (the object store, the FreeType instance, the glyph cache).
///
/// THREADING MODEL - this is load-bearing, see docs/threading.md:
///
///  * Constructing a Context creates an INDEPENDENT base context with its own
///    private lock set, passed to MuPDF through fz_locks_context::user. Two
///    independent contexts share no locks and so never contend, but they also
///    share no store or glyph cache, so each re-parses and re-rasterises fonts.
///
///  * clone() creates a context that SHARES the parent's store, glyph cache and
///    lock set. Display lists may be passed between a parent and its clones.
///    This is MuPDF's documented multi-threaded pattern.
///
///  * DO NOT use clone() to render in parallel. MuPDF takes FZ_LOCK_ALLOC
///    around every allocation and clones necessarily share one lock set, so
///    they serialise on malloc. Measured on a 13th-gen i7 (20 threads),
///    160 and 500 page corpora, reproducible across runs:
///
///        threads        1      2      4      8
///        clone        1.00x  0.62x  0.55x  0.18x  <- gets SLOWER
///        independent  1.00x  2.0x   3.8x   5.4x
///
///    Two clones are already a net loss; eight are ~5x slower than one thread.
///    Parallel work uses independent Contexts (one per thread), which do not
///    share a lock set and scale near-linearly. Regenerate with
///    bench/thread_scaling; see docs/threading.md for the full analysis.
///
///  * Only one thread may touch a document at a time (open it, load pages,
///    build display lists). A display list may then be rendered from any
///    thread holding its own clone.
class Context {
public:
    /// Creates an independent base context with its own lock set and registers
    /// the standard document handlers. `store_max_bytes == 0` uses MuPDF's
    /// default store budget.
    explicit Context(std::size_t store_max_bytes = 0);

    Context(Context&&) noexcept;
    Context& operator=(Context&&) noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context();

    /// A context sharing this one's store, glyph cache and lock set.
    /// Safe to move to another thread; see the contention note above.
    [[nodiscard]] Context clone() const;

    [[nodiscard]] fz_context* raw() const noexcept { return ctx_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ctx_ != nullptr; }

private:
    struct Locks;

    Context(fz_context* ctx, std::shared_ptr<Locks> locks) noexcept;

    /// Swallows MuPDF's stderr diagnostics; failures surface as leht::Error.
    static void discard_message(void* user, const char* message);

    fz_context* ctx_ = nullptr;
    /// Shared so the mutexes outlive every clone, whatever order they drop in.
    std::shared_ptr<Locks> locks_;
};

}  // namespace leht
