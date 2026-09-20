// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"

#include "mupdf_c.hpp"
#include "leht/error.hpp"

#include <array>
#include <mutex>
#include <utility>

namespace leht {

/// One mutex per MuPDF lock id, reached through fz_locks_context::user.
///
/// Passing the lock set through `user` rather than a file-scope global is the
/// whole point: a global lock set would make every context in the process --
/// including ones that share nothing -- serialise on the same three mutexes.
struct Context::Locks {
    std::array<std::mutex, FZ_LOCK_MAX> mutexes{};
    fz_locks_context ctx{};

    Locks() {
        ctx.user = this;
        ctx.lock = &Locks::lock;
        ctx.unlock = &Locks::unlock;
    }

    Locks(const Locks&) = delete;
    Locks& operator=(const Locks&) = delete;

    static void lock(void* user, int which) noexcept {
        static_cast<Locks*>(user)->mutexes[static_cast<std::size_t>(which)].lock();
    }
    static void unlock(void* user, int which) noexcept {
        static_cast<Locks*>(user)->mutexes[static_cast<std::size_t>(which)].unlock();
    }
};

Context::Context(std::size_t store_max_bytes)
    : locks_(std::make_shared<Locks>()) {
    const std::size_t store =
        store_max_bytes == 0 ? static_cast<std::size_t>(FZ_STORE_DEFAULT)
                             : store_max_bytes;

    ctx_ = fz_new_context(nullptr, &locks_->ctx, store);
    if (ctx_ == nullptr) {
        throw Error(0, "fz_new_context failed (out of memory)");
    }

    // MuPDF prints diagnostics to stderr by default. A library must not, and a
    // CLI must not have MuPDF's chatter interleaved with its own output, so
    // route both streams into nothing. Real failures still arrive as thrown
    // leht::Error via guarded().
    fz_set_error_callback(ctx_, &Context::discard_message, nullptr);
    fz_set_warning_callback(ctx_, &Context::discard_message, nullptr);

    try {
        guarded(ctx_, [](fz_context* c) { fz_register_document_handlers(c); });
    } catch (...) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
        throw;
    }
}

void Context::discard_message(void* /*user*/, const char* /*message*/) {}

Context::Context(fz_context* ctx, std::shared_ptr<Locks> locks) noexcept
    : ctx_(ctx), locks_(std::move(locks)) {}

Context::Context(Context&& other) noexcept
    : ctx_(std::exchange(other.ctx_, nullptr)),
      locks_(std::move(other.locks_)) {}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        if (ctx_ != nullptr) {
            fz_drop_context(ctx_);
        }
        ctx_ = std::exchange(other.ctx_, nullptr);
        locks_ = std::move(other.locks_);
    }
    return *this;
}

Context::~Context() {
    if (ctx_ != nullptr) {
        fz_drop_context(ctx_);
    }
}

Context Context::clone() const {
    if (ctx_ == nullptr) {
        throw Error(0, "cannot clone a moved-from Context");
    }
    fz_context* copy = fz_clone_context(ctx_);
    if (copy == nullptr) {
        throw Error(0, "fz_clone_context failed (out of memory)");
    }
    // Shares locks_ so the mutexes outlive this clone even if the parent drops
    // first. MuPDF refcounts the shared global state independently.
    return Context{copy, locks_};
}

}  // namespace leht
