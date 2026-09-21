// SPDX-License-Identifier: AGPL-3.0-or-later
//
// leht-worker end to end: spawn the real binary, speak the protocol to it, and
// check every answer against core/ run in-process on the same file. The worker
// must be a transparent relocation of the engine -- same pixels, same text,
// same geometry -- plus containment when a file kills it.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/ipc/process.hpp"
#include "leht/ipc/protocol.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"
#include "test_harness.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <memory>
#include <string>
#include <utility>

using namespace leht::ipc;

namespace {

std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

std::unique_ptr<WorkerProcess> start() {
    auto w = WorkerProcess::spawn(LEHT_WORKER_EXE);
    w->handshake();
    return w;
}

Frame next(WorkerProcess& w) {
    auto f = w.channel().recv();
    CHECK(f.has_value());
    return std::move(*f);
}

/// Sends Open for `path` and returns the first reply.
Frame open(WorkerProcess& w, const std::string& path, std::uint64_t id = 1) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    w.channel().send(id, Open{path.substr(path.rfind('/') + 1)}, fd);
    ::close(fd);  // the worker has its own copy now
    return next(w);
}

Opened open_ok(WorkerProcess& w, const std::string& path) {
    const Frame f = open(w, path);
    const Opened o = decode_as<Opened>(f);
    (void)decode_as<Outline>(next(w));
    return o;
}

void opened_matches_in_process() {
    auto w = start();
    const Frame f = open(*w, corpus("text_10p.pdf"), 42);
    CHECK(f.id == 42);
    const Opened o = decode_as<Opened>(f);

    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, corpus("text_10p.pdf"));
    leht::Renderer r{ctx, doc};
    CHECK(o.base_sizes.size() == 10);
    for (int p = 0; p < 10; ++p) {
        const leht::PageSize s = r.page_size(p, 1.0F);
        CHECK(o.base_sizes[static_cast<std::size_t>(p)].width == s.width);
        CHECK(o.base_sizes[static_cast<std::size_t>(p)].height == s.height);
    }
    const Outline ol = decode_as<Outline>(next(*w));
    CHECK(ol.rows.empty());  // text_10p has no outline
}

void outline_matches_in_process() {
    auto w = start();
    (void)decode_as<Opened>(open(*w, corpus("outlined.pdf")));
    const Outline ol = decode_as<Outline>(next(*w));

    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, corpus("outlined.pdf"));
    const auto tree = doc.outline();
    CHECK(!ol.rows.empty() && !tree.empty());
    CHECK(ol.rows.front().title == tree.front().title);
    CHECK(ol.rows.front().page == tree.front().page);
    CHECK(ol.rows.front().depth == 0);
}

/// The headline guarantee: pixels from the worker are byte-identical to an
/// in-process render.
void renders_are_pixel_identical() {
    auto w = start();
    (void)open_ok(*w, corpus("text_10p.pdf"));

    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, corpus("text_10p.pdf"));
    leht::Renderer r{ctx, doc};

    std::uint64_t id = 100;
    for (int page = 0; page < 10; ++page) {
        for (const auto& [zoom, rot] : {std::pair{1.0F, 0}, std::pair{1.75F, 90},
                                        std::pair{0.5F, 270}}) {
            w->channel().send(id, Render{page, zoom, rot, 1});
            const Frame f = next(*w);
            CHECK(f.id == id);
            const Rendered got = decode_as<Rendered>(f);
            const auto want = r.render(page, zoom, rot);
            CHECK(want.has_value());
            CHECK(got.page == page && got.rotation == rot && got.generation == 1);
            CHECK(got.bitmap.width == want->width && got.bitmap.height == want->height);
            CHECK(got.bitmap.pixels == want->pixels);
            ++id;
        }
    }
}

void search_and_select_match_in_process() {
    auto w = start();
    (void)open_ok(*w, corpus("text_10p.pdf"));

    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, corpus("text_10p.pdf"));

    w->channel().send(7, Search{"lazy dog"});
    std::uint32_t streamed = 0;
    int pages_with_hits = 0;
    for (;;) {
        const Frame f = next(*w);
        CHECK(f.id == 7);
        if (f.type == MsgType::SearchDone) {
            CHECK(decode_as<SearchDone>(f).total == streamed);
            break;
        }
        const PageMatches pm = decode_as<PageMatches>(f);
        leht::TextPage tp{ctx, doc, pm.page, 1.0F};
        std::size_t quads = 0;
        for (const auto& hit : tp.search("lazy dog")) {
            quads += hit.quads.size();
        }
        CHECK(pm.quads.size() == quads);
        streamed += static_cast<std::uint32_t>(pm.quads.size());
        ++pages_with_hits;
    }
    CHECK(pages_with_hits == 10 && streamed > 0);

    Select s;
    s.page = 2;
    s.ax = 0; s.ay = 0; s.bx = 600; s.by = 120;
    s.mode = leht::SelectMode::Lines;
    w->channel().send(8, s);
    const SelectionResult sel = decode_as<SelectionResult>(next(*w));
    leht::TextPage tp{ctx, doc, 2, 1.0F};
    const leht::Selection want = tp.select(0, 0, 600, 120, leht::SelectMode::Lines);
    CHECK(!sel.text.empty() && sel.text == want.text);
    CHECK(sel.quads.size() == want.quads.size());
}

void password_flow() {
    auto w = start();
    CHECK(!decode_as<NeedsPassword>(open(*w, corpus("locked.pdf"))).retry);

    w->channel().send(2, Authenticate{"wrong"});
    CHECK(decode_as<NeedsPassword>(next(*w)).retry);

    w->channel().send(3, Authenticate{"s3cret"});
    const Opened o = decode_as<Opened>(next(*w));
    CHECK(o.base_sizes.size() == 10);
    (void)decode_as<Outline>(next(*w));
}

void stale_and_bad_requests_are_answered() {
    auto w = start();

    // Before any document: every request still gets exactly one answer.
    w->channel().send(1, Render{0, 1.0F, 0, 1});
    CHECK(next(*w).type == MsgType::RenderSkipped);
    w->channel().send(2, Search{"x"});
    CHECK(next(*w).type == MsgType::Failed);

    (void)open_ok(*w, corpus("text_10p.pdf"));

    // A generation older than the latest Cancel is skipped, not rendered.
    w->channel().send(0, Cancel{10});
    w->channel().send(3, Render{0, 1.0F, 0, 5});
    const RenderSkipped rs = decode_as<RenderSkipped>(next(*w));
    CHECK(rs.page == 0 && rs.generation == 5);
    w->channel().send(4, Render{0, 1.0F, 0, 10});
    CHECK(next(*w).type == MsgType::Rendered);

    // A page past the end renders blank rather than failing the worker.
    w->channel().send(5, Render{500, 1.0F, 0, 10});
    CHECK(next(*w).type == MsgType::RenderSkipped);

    // A damaged file either fails the open or is repaired (then its outline
    // follows); either way the worker carries on.
    const Frame damaged = open(*w, corpus("damaged.pdf"));
    CHECK(damaged.type == MsgType::Failed || damaged.type == MsgType::Opened);
    if (damaged.type == MsgType::Opened) {
        (void)decode_as<Outline>(next(*w));
    }
    (void)open_ok(*w, corpus("text_10p.pdf"));

    // Garbage that no handler accepts is an ordinary Failed.
    char junk_path[] = "/tmp/leht_worker_junk_XXXXXX";
    const int junk = ::mkstemp(junk_path);
    CHECK(junk >= 0);
    const std::string bytes(4096, '\xAB');
    CHECK(::write(junk, bytes.data(), bytes.size()) == 4096);
    ::close(junk);
    const Frame bad = open(*w, junk_path);
    ::unlink(junk_path);
    CHECK(bad.type == MsgType::Failed);
    (void)open_ok(*w, corpus("text_10p.pdf"));
}

/// Writes a PDF whose outline is nested `depth` levels deep: the MuPDF stack
/// overflow in pdf_test_outline, live in 1.28.4. Same shape as
/// tests/crashes/mupdf_outline_depth_stackoverflow.py, generated here so the
/// test needs no Python.
std::string write_deep_outline(int depth) {
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
        "<< /Type /Outlines /First 5 0 R /Last 5 0 R >>",
    };
    for (int k = 0; k < depth; ++k) {
        const int num = 5 + k;
        std::string o = "<< /Title (x) /Parent " + std::to_string(num - 1) + " 0 R";
        if (k < depth - 1) {
            o += " /First " + std::to_string(num + 1) + " 0 R /Last " +
                 std::to_string(num + 1) + " 0 R";
        }
        objs.push_back(o + " >>");
    }
    std::string out = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objs.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    char line[32];
    for (std::size_t off : offsets) {
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";

    char path[] = "/tmp/leht_deep_outline_XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    CHECK(::write(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size()));
    ::close(fd);
    return path;
}

/// The point of M3. A file that crashes MuPDF kills the worker, the viewer's
/// side sees a clean end-of-stream rather than garbage, and a fresh worker
/// opens the next document normally.
void crash_is_contained() {
    const std::string deep = write_deep_outline(200000);

    auto w = start();
    const int fd = ::open(deep.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    w->channel().send(1, Open{"deep.pdf"}, fd);
    ::close(fd);
    ::unlink(deep.c_str());

    // Opened may or may not arrive before the outline load kills the worker;
    // what must follow is end-of-stream, never a malformed frame.
    bool eof = false;
    for (int i = 0; i < 3 && !eof; ++i) {
        auto f = w->channel().recv();
        if (!f) {
            eof = true;
        } else {
            CHECK(f->type == MsgType::Opened);
        }
    }
    CHECK(eof);
    const auto status = w->wait_for(std::chrono::seconds(10));
    CHECK(status.has_value() && status->crashed());

    auto fresh = start();
    CHECK(open_ok(*fresh, corpus("text_10p.pdf")).base_sizes.size() == 10);
}

/// Every corpus file -- including the decompression bombs and the image
/// formats -- through one sandboxed worker: open, render, search, select. None
/// of it may kill the worker. A seccomp rule missing for some code path shows
/// up here as a death (SIGSYS), which is why the sweep is broad rather than
/// clever.
void corpus_sweep_never_kills_the_worker() {
    auto w = start();
    std::uint64_t id = 1;
    int files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(LEHT_CORPUS_DIR)) {
        if (!entry.is_regular_file() || entry.path().extension() == ".sh") {
            continue;
        }
        ++files;
        const Frame first = open(*w, entry.path().string(), id++);
        std::size_t pages = 0;
        if (first.type == MsgType::NeedsPassword) {
            w->channel().send(id++, Authenticate{"s3cret"});
            pages = decode_as<Opened>(next(*w)).base_sizes.size();
        } else if (first.type == MsgType::Opened) {
            pages = decode_as<Opened>(first).base_sizes.size();
        } else {
            CHECK(first.type == MsgType::Failed);
            continue;
        }
        (void)decode_as<Outline>(next(*w));
        for (int p = 0; p < static_cast<int>(std::min<std::size_t>(pages, 3)); ++p) {
            w->channel().send(id++, Render{p, 1.0F, 0, 1});
            const MsgType t = next(*w).type;
            CHECK(t == MsgType::Rendered || t == MsgType::RenderSkipped);
        }
        w->channel().send(id++, Search{"the"});
        for (;;) {
            const MsgType t = next(*w).type;
            if (t == MsgType::SearchDone || t == MsgType::Failed) {
                break;
            }
            CHECK(t == MsgType::PageMatches);
        }
        Select s;
        s.bx = 500;
        s.by = 500;
        w->channel().send(id++, s);
        const MsgType t = next(*w).type;
        CHECK(t == MsgType::SelectionResult || t == MsgType::Failed);
    }
    CHECK(files >= 10);
    CHECK(!w->try_wait().has_value());  // still alive after all of it
}

/// Confirms the worker really is under seccomp: the kernel reports mode 2
/// (filter) for it. Skipped in sanitizer builds, which run it unsandboxed.
void worker_is_sandboxed() {
#if defined(__SANITIZE_ADDRESS__)
    std::printf("      SKIP sanitizer build runs the worker unsandboxed\n");
#else
    auto w = start();
    std::ifstream status("/proc/" + std::to_string(w->pid()) + "/status");
    std::string line;
    bool seccomp = false;
    while (std::getline(status, line)) {
        if (line.rfind("Seccomp:", 0) == 0) {
            seccomp = line.find('2') != std::string::npos;
        }
    }
    CHECK(seccomp);

    // The namespaces are best effort (unprivileged user namespaces can be
    // disabled by policy), so report rather than require them.
    const auto ns = [](const std::string& pid) {
        return std::filesystem::read_symlink("/proc/" + pid + "/ns/net").string();
    };
    const bool isolated = ns("self") != ns(std::to_string(w->pid()));
    std::printf("      network namespace: %s\n", isolated ? "isolated" : "shared (userns unavailable)");
#endif
}

/// Each thing the sandbox exists to stop, attempted for real after lockdown.
/// The worker must die of SIGSYS; returning at all means the policy leaked.
void sandbox_forbids_escape_routes() {
    for (const char* what : {"open", "socket", "exec", "fork", "mmap-exec"}) {
        auto w = WorkerProcess::spawn(LEHT_WORKER_EXE,
                                      {std::string("--selftest-sandbox=") + what});
        const auto status = w->wait_for(std::chrono::seconds(10));
        CHECK(status.has_value());
        if (!status->signaled && status->code == 77) {
            std::printf("      SKIP sandbox selftests: sanitizer build\n");
            return;
        }
        if (!(status->signaled && status->code == SIGSYS)) {
            std::fprintf(stderr, "sandbox selftest '%s' did not die of SIGSYS\n", what);
        }
        CHECK(status->signaled && status->code == SIGSYS);
    }
}

/// Runs a search to completion, returning (pages with matches, SearchDone total).
std::pair<int, std::uint32_t> drain_search(WorkerProcess& w, std::uint64_t id) {
    int pages = 0;
    for (;;) {
        const Frame f = next(w);
        CHECK(f.id == id);
        if (f.type == MsgType::SearchDone) {
            return {pages, decode_as<SearchDone>(f).total};
        }
        (void)decode_as<PageMatches>(f);
        ++pages;
    }
}

/// CancelSearch stops a long search between pages, and the stream still ends
/// with a SearchDone so the viewer stays in step. A cancel that arrives before
/// a search starts belongs to an earlier one and must not stop it.
void search_can_be_cancelled() {
    auto w = start();
    (void)open_ok(*w, corpus("text_500p.pdf"));

    w->channel().send(1, CancelSearch{1});  // cancels epoch 0 only
    w->channel().send(2, Search{"quick", 1});
    const auto full = drain_search(*w, 2);
    CHECK(full.first == 500);               // "quick" is on every page

    // Sent back to back, the cancel may reach the worker before the search has
    // even started: it must still take effect.
    w->channel().send(3, Search{"quick", 1});
    w->channel().send(0, CancelSearch{2});
    const auto cut = drain_search(*w, 3);
    CHECK(cut.first < 500);
    std::printf("      cancelled after %d of 500 pages\n", cut.first);

    // A search newer than the cancel runs in full.
    w->channel().send(4, Search{"quick", 2});
    CHECK(drain_search(*w, 4).first == 500);

    // The worker is fine afterwards.
    w->channel().send(5, Render{0, 1.0F, 0, 1});
    CHECK(next(*w).type == MsgType::Rendered);
}

void clean_shutdown() {
    auto w = start();
    w->channel().send(1, Shutdown{});
    const auto status = w->wait_for(std::chrono::seconds(5));
    CHECK(status.has_value() && !status->crashed());
}

void viewer_eof_ends_the_worker() {
    auto w = start();
    w->channel().shutdown();
    const auto status = w->wait_for(std::chrono::seconds(5));
    CHECK(status.has_value() && !status->crashed());
}

}  // namespace

int main() {
    RUN(opened_matches_in_process);
    RUN(outline_matches_in_process);
    RUN(renders_are_pixel_identical);
    RUN(search_and_select_match_in_process);
    RUN(password_flow);
    RUN(stale_and_bad_requests_are_answered);
    RUN(crash_is_contained);
    RUN(corpus_sweep_never_kills_the_worker);
    RUN(worker_is_sandboxed);
    RUN(sandbox_forbids_escape_routes);
    RUN(search_can_be_cancelled);
    RUN(clean_shutdown);
    RUN(viewer_eof_ends_the_worker);
    return 0;
}
