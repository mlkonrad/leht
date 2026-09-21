// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What process isolation costs the viewer. Measures the same work in-process
// and through leht-worker:
//
//   open    -- from nothing to the first page's pixels: context (or process
//              spawn + handshake), open, every page size, outline, render p.0
//   turn    -- one uncached page render, averaged over the document
//   spawn   -- process start, MuPDF context, sandbox, handshake: the fixed
//              part of the open overhead
//
// The difference is the price of M3: process start-up, a socket round trip,
// and copying each bitmap across. A cached page turn is not measured: the page
// cache lives in the viewer process, so a hit never reaches the worker.
//
//   bench_worker_latency FILE [zoom] [pages]

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/ipc/process.hpp"
#include "leht/ipc/protocol.hpp"
#include "leht/renderer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace leht::ipc;

namespace {

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Result {
    double open_ms;
    double turn_ms;
    std::size_t frame_bytes;
};

Result in_process(const std::string& path, float zoom, int pages) {
    const auto t0 = Clock::now();
    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, path);
    leht::Renderer r{ctx, doc};
    const int n = doc.page_count();
    for (int p = 0; p < n; ++p) {
        (void)r.page_size(p, 1.0F);
    }
    (void)doc.outline();
    auto first = r.render(0, zoom);
    const double open_ms = ms_since(t0);

    const auto t1 = Clock::now();
    const int count = std::min(pages, n);
    for (int p = 0; p < count; ++p) {
        (void)r.render(p, zoom);
    }
    return {open_ms, ms_since(t1) / count, first ? first->pixels.size() : 0};
}

Result via_worker(const std::string& exe, const std::string& path, float zoom, int pages) {
    const auto t0 = Clock::now();
    auto w = WorkerProcess::spawn(exe);
    w->handshake();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    w->channel().send(1, Open{"doc.pdf"}, fd);
    ::close(fd);
    const Opened opened = decode_as<Opened>(*w->channel().recv());
    (void)decode_as<Outline>(*w->channel().recv());
    w->channel().send(2, Render{0, zoom, 0, 1});
    const Rendered first = decode_as<Rendered>(*w->channel().recv());
    const double open_ms = ms_since(t0);

    const auto t1 = Clock::now();
    const int count = std::min(pages, static_cast<int>(opened.base_sizes.size()));
    for (int p = 0; p < count; ++p) {
        w->channel().send(static_cast<std::uint64_t>(10 + p), Render{p, zoom, 0, 1});
        (void)decode_as<Rendered>(*w->channel().recv());
    }
    return {open_ms, ms_since(t1) / count, first.bitmap.pixels.size()};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s FILE [zoom] [pages]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const float zoom = argc > 2 ? std::strtof(argv[2], nullptr) : 1.5F;
    const int pages = argc > 3 ? std::atoi(argv[3]) : 50;
    constexpr int kRuns = 7;

    std::vector<double> spawn;
    for (int i = 0; i < kRuns; ++i) {
        const auto t0 = Clock::now();
        auto w = WorkerProcess::spawn(LEHT_WORKER_EXE);
        w->handshake();
        spawn.push_back(ms_since(t0));
    }

    std::vector<double> ip_open, ip_turn, w_open, w_turn;
    std::size_t bytes = 0;
    for (int i = 0; i < kRuns; ++i) {
        const Result a = in_process(path, zoom, pages);
        const Result b = via_worker(LEHT_WORKER_EXE, path, zoom, pages);
        ip_open.push_back(a.open_ms);
        ip_turn.push_back(a.turn_ms);
        w_open.push_back(b.open_ms);
        w_turn.push_back(b.turn_ms);
        bytes = b.frame_bytes;
    }

    std::printf("%s  zoom %.2f  (median of %d runs)\n", path.c_str(), static_cast<double>(zoom), kRuns);
    std::printf("                    in-process    worker    overhead\n");
    std::printf("worker spawn + handshake       %8.2f ms\n", median(spawn));
    std::printf("open + first page   %8.2f ms  %8.2f ms  %+8.2f ms\n", median(ip_open),
                median(w_open), median(w_open) - median(ip_open));
    std::printf("uncached page turn  %8.2f ms  %8.2f ms  %+8.2f ms\n", median(ip_turn),
                median(w_turn), median(w_turn) - median(ip_turn));
    std::printf("bitmap per page     %8.2f MB\n", static_cast<double>(bytes) / (1 << 20));
    return 0;
}
