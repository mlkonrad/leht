// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Measures how MuPDF rendering scales across threads under the two strategies
// leht can use, so the viewer's pool size is set from data rather than hope.
//
//   clone       one Context, N clones sharing its store and glyph cache.
//               Display lists are built once on the main thread and rendered
//               by the workers. MuPDF's documented pattern -- and the one that
//               contends on FZ_LOCK_ALLOC, taken around every allocation.
//
//   independent N base Contexts, each with its own private lock set, each
//               opening the file itself. No shared store, so no shared locks,
//               but parsing and font work are duplicated per thread.
//
// Upstream reports 10 threads running 13.3x SLOWER than 1 on a 6-core machine
// with a shared lock set (github.com/messense/mupdf-rs issues/260). This tells
// us what actually happens here.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"

#include "mupdf_c.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    const std::chrono::duration<double> elapsed = Clock::now() - start;
    return elapsed.count();
}

fz_matrix zoom_matrix(float zoom) { return fz_scale(zoom, zoom); }

/// Renders one display list and discards the result. Obeys the guarded()
/// contract: the lambda holds only raw pointers and PODs.
void render_list(leht::Context& ctx, fz_display_list* list, float zoom) {
    fz_pixmap* pix = nullptr;
    const fz_matrix ctm = zoom_matrix(zoom);
    fz_context* raw = ctx.raw();

    leht::guarded(raw, [&](fz_context* g) {
        pix = fz_new_pixmap_from_display_list(g, list, ctm, fz_device_rgb(g), 0);
    });
    if (pix != nullptr) {
        fz_drop_pixmap(raw, pix);
    }
}

/// Renders one page straight from a document, no display list in between.
void render_page_number(leht::Context& ctx, fz_document* doc, int number,
                        float zoom) {
    fz_pixmap* pix = nullptr;
    const fz_matrix ctm = zoom_matrix(zoom);
    fz_context* raw = ctx.raw();

    leht::guarded(raw, [&](fz_context* g) {
        pix = fz_new_pixmap_from_page_number(g, doc, number, ctm,
                                             fz_device_rgb(g), 0);
    });
    if (pix != nullptr) {
        fz_drop_pixmap(raw, pix);
    }
}

/// Strategy A: shared store, cloned contexts, display lists prebuilt.
/// Only the render phase is timed -- list construction is one-off setup that a
/// real viewer does once per page and caches.
double run_clone_mode(const std::string& path, int threads, int pages,
                      float zoom) {
    leht::Context base;
    leht::Document doc = leht::Document::open(base, path);

    const int total = std::min(pages, doc.page_count());
    std::vector<fz_display_list*> lists(static_cast<std::size_t>(total), nullptr);

    fz_document* raw_doc = doc.raw();
    for (int i = 0; i < total; ++i) {
        fz_display_list* list = nullptr;
        leht::guarded(base.raw(), [&](fz_context* g) {
            list = fz_new_display_list_from_page_number(g, raw_doc, i);
        });
        lists[static_cast<std::size_t>(i)] = list;
    }

    std::atomic<int> next{0};
    const Clock::time_point start = Clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            leht::Context local = base.clone();
            for (;;) {
                const int i = next.fetch_add(1);
                if (i >= total) {
                    break;
                }
                render_list(local, lists[static_cast<std::size_t>(i)], zoom);
            }
        });
    }
    for (std::thread& w : workers) {
        w.join();
    }
    const double elapsed = seconds_since(start);

    for (fz_display_list* list : lists) {
        if (list != nullptr) {
            fz_drop_display_list(base.raw(), list);
        }
    }
    return elapsed;
}

/// Strategy B: one independent base context per thread, each opening the file.
/// Per-thread open and parse cost is included because it is unavoidable here.
double run_independent_mode(const std::string& path, int threads, int pages,
                            float zoom) {
    int total = 0;
    {
        leht::Context probe;
        leht::Document doc = leht::Document::open(probe, path);
        total = std::min(pages, doc.page_count());
    }

    std::atomic<int> next{0};
    const Clock::time_point start = Clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            leht::Context local;  // independent: its own lock set
            leht::Document doc = leht::Document::open(local, path);
            fz_document* raw_doc = doc.raw();
            for (;;) {
                const int i = next.fetch_add(1);
                if (i >= total) {
                    break;
                }
                render_page_number(local, raw_doc, i, zoom);
            }
        });
    }
    for (std::thread& w : workers) {
        w.join();
    }
    return seconds_since(start);
}

void report(const char* label, int threads, double elapsed, double baseline,
            int pages) {
    const double speedup = elapsed > 0.0 ? baseline / elapsed : 0.0;
    const double pps = elapsed > 0.0 ? static_cast<double>(pages) / elapsed : 0.0;
    std::printf("  %-12s %2d  %8.3f s  %6.2fx  %8.1f pages/s\n", label, threads,
                elapsed, speedup, pps);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path =
        argc > 1 ? argv[1] : "tests/corpus/text_160p.pdf";
    const int max_pages = argc > 2 ? std::atoi(argv[2]) : 160;
    const float zoom = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 1.5F;

    const unsigned hw = std::thread::hardware_concurrency();
    std::printf("leht thread-scaling benchmark\n");
    std::printf("  file        %s\n", path.c_str());
    std::printf("  pages       %d at zoom %.2f\n", max_pages,
                static_cast<double>(zoom));
    std::printf("  hardware    %u logical cores\n\n", hw);

    const std::vector<int> counts{1, 2, 4, 8};

    try {
        std::printf("  strategy  thr    elapsed  speedup     throughput\n");
        std::printf("  -----------------------------------------------------\n");

        double clone_baseline = 0.0;
        for (const int t : counts) {
            const double e = run_clone_mode(path, t, max_pages, zoom);
            if (t == 1) {
                clone_baseline = e;
            }
            report("clone", t, e, clone_baseline, max_pages);
        }
        std::printf("\n");

        double indep_baseline = 0.0;
        for (const int t : counts) {
            const double e = run_independent_mode(path, t, max_pages, zoom);
            if (t == 1) {
                indep_baseline = e;
            }
            report("independent", t, e, indep_baseline, max_pages);
        }
        std::printf("\n");
    } catch (const leht::Error& e) {
        std::fprintf(stderr, "benchmark failed: %s\n", e.what());
        return 1;
    }
    return 0;
}
