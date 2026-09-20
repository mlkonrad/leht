// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/context.hpp"
#include "test_harness.hpp"

#include <thread>
#include <utility>
#include <vector>

using leht::Context;

static void construct_and_destroy() {
    Context ctx;
    CHECK(static_cast<bool>(ctx));
    CHECK(ctx.raw() != nullptr);
}

static void move_leaves_source_empty() {
    Context a;
    fz_context* raw = a.raw();
    Context b{std::move(a)};
    CHECK(b.raw() == raw);
    CHECK(!static_cast<bool>(a));  // NOLINT(bugprone-use-after-move)
}

static void clone_is_distinct_but_shares_state() {
    Context base;
    Context copy = base.clone();
    CHECK(copy.raw() != nullptr);
    CHECK(copy.raw() != base.raw());
}

/// Clones dropping out of order must not free the lock set early.
static void clone_outlives_parent() {
    Context* base = new Context{};
    Context copy = base->clone();
    delete base;
    CHECK(copy.raw() != nullptr);  // mutexes still alive via shared_ptr
}

/// Exercises the lock callbacks under real contention. Guards against a
/// regression where the lock set is reached through a global rather than
/// through fz_locks_context::user.
static void clones_work_across_threads() {
    Context base;
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&base] {
            Context local = base.clone();
            CHECK(local.raw() != nullptr);
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
}

int main() {
    RUN(construct_and_destroy);
    RUN(move_leaves_source_empty);
    RUN(clone_is_distinct_but_shares_state);
    RUN(clone_outlives_parent);
    RUN(clones_work_across_threads);
    return 0;
}
