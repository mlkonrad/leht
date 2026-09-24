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
#include "leht/ops/annotate.hpp"
#include "leht/renderer.hpp"
#ifdef LEHT_HAVE_OCR
#include "leht/ocr/ocr.hpp"
#endif
#include "leht/text.hpp"
#include "leht/crypto/crypto.hpp"
#include "leht/error.hpp"
#include "leht/ops/sign.hpp"
#include "edit_harness.hpp"
#include "test_pki.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
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

// --- editing (M4b) -------------------------------------------------------------

Edited edit_ok(WorkerProcess& w, const Edit& e, std::uint64_t id = 900) {
    w.channel().send(id, e);
    const Frame f = next(w);
    CHECK(f.id == id);
    if (f.type == MsgType::Failed) {
        std::fprintf(stderr, "edit failed: %s\n", decode_as<Failed>(f).message.c_str());
    }
    return decode_as<Edited>(f);
}

Edit highlight_edit(const std::string& path, int page, const char* needle) {
    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, path);
    const auto hits = leht::TextPage(ctx, doc, page).search(needle);
    CHECK(!hits.empty());
    Edit e;
    e.kind = Edit::Kind::AddAnnot;
    e.page = page;
    e.annot.kind = leht::ops::AnnotKind::Highlight;
    e.annot.quads = hits.front().quads;
    e.annot.contents = "worker";
    return e;
}

/// Saves through the sandboxed worker into a temp file; returns its path.
std::string save_via_worker(WorkerProcess& w, const char* name) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    w.channel().send(950, Save{}, fd);
    ::close(fd);
    const Frame f = next(w);
    if (f.type == MsgType::Failed) {
        std::fprintf(stderr, "save failed: %s\n", decode_as<Failed>(f).message.c_str());
    }
    CHECK(decode_as<Saved>(f).bytes > 0);
    return path;
}

void edits_apply_render_and_save() {
    const std::string in = corpus("text_10p.pdf");
    auto w = start();
    (void)open_ok(*w, in);

    // Render before, edit, render after: the cached display list must not
    // hide the change.
    w->channel().send(901, Render{0, 1.0F, 0, 1});
    const Rendered before = decode_as<Rendered>(next(*w));
    const Edited e = edit_ok(*w, highlight_edit(in, 0, "quick brown"));
    CHECK(e.annot_id > 0 && e.pages == std::vector<int>({0}) && e.base_sizes.size() == 10);
    w->channel().send(902, Render{0, 1.0F, 0, 1});
    const Rendered after = decode_as<Rendered>(next(*w));
    CHECK(before.bitmap.pixels != after.bitmap.pixels);

    w->channel().send(903, ListAnnots{});
    const AnnotList list = decode_as<AnnotList>(next(*w));
    CHECK(list.items.size() == 1 && list.items[0].id == e.annot_id &&
          list.items[0].type == "Highlight" && list.items[0].contents == "worker");

    // A crop changes the page size, and Edited says so.
    Edit crop;
    crop.kind = Edit::Kind::CropMargins;
    crop.pages = "2";
    crop.margins = {10, 20, 30, 40};
    const Edited c = edit_ok(*w, crop);
    CHECK(c.pages == std::vector<int>({1}));
    CHECK(c.base_sizes[1].width == e.base_sizes[1].width - 40);
    CHECK(c.base_sizes[1].height == e.base_sizes[1].height - 60);

    // Save from inside the sandbox: write and lseek on a passed fd only.
    const std::string out = save_via_worker(*w, "leht_worker_save.pdf");
    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, out);
    const auto annots = leht::ops::list_annotations(ctx, doc);
    CHECK(annots.size() == 1 && annots[0].id == e.annot_id);
    CHECK(leht::Renderer(ctx, doc).page_size(1, 1.0F).width == c.base_sizes[1].width);
    std::filesystem::remove(out);

    // Deleting by id, then a bad id.
    Edit del;
    del.kind = Edit::Kind::DeleteAnnot;
    del.annot_id = e.annot_id;
    CHECK(edit_ok(*w, del).pages == std::vector<int>({0}));
    w->channel().send(904, del);
    CHECK(next(*w).type == MsgType::Failed);
}

void redaction_through_the_worker() {
    auto w = start();
    (void)open_ok(*w, corpus("text_10p.pdf"));
    Edit e;
    e.kind = Edit::Kind::RedactText;
    e.text = "quick brown";
    const Edited r = edit_ok(*w, e);
    CHECK(r.pages.size() == 10 && r.remaining.empty());

    const std::string out = save_via_worker(*w, "leht_worker_redact.pdf");
    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, out);
    for (int p = 0; p < doc.page_count(); ++p) {
        CHECK(leht::TextPage(ctx, doc, p).search("quick brown").empty());
    }
    std::filesystem::remove(out);
}

void forms_through_the_worker() {
    const std::string form = corpus("form.pdf");
    if (!std::filesystem::exists(form)) {
        std::printf("  skip (tests/corpus/form.pdf not generated)\n");
        return;
    }
    auto w = start();
    (void)open_ok(*w, form);
    w->channel().send(910, ListFields{});
    const FieldList before = decode_as<FieldList>(next(*w));
    CHECK(before.items.size() == 2);

    Edit set;
    set.kind = Edit::Kind::SetField;
    set.name = "name";
    set.text = "Marlon";
    CHECK(edit_ok(*w, set).all_pages);
    w->channel().send(911, ListFields{});
    const FieldList after = decode_as<FieldList>(next(*w));
    const auto it = std::find_if(after.items.begin(), after.items.end(),
                                 [](const auto& f) { return f.name == "name"; });
    CHECK(it != after.items.end() && it->value == "Marlon");

    // A value the field refuses is an ordinary failure, not a dead worker.
    set.text = std::string(50, 'x');  // over MaxLen 20
    w->channel().send(912, set);
    CHECK(next(*w).type == MsgType::Failed);
    w->channel().send(913, ListFields{});
    CHECK(next(*w).type == MsgType::FieldList);
}

void replay_is_deterministic() {
    // Undo and crash recovery replay the edit log into a fresh document. The
    // ids it hands out must be the ones the viewer already holds.
    const std::string in = corpus("text_10p.pdf");
    const Edit a = highlight_edit(in, 0, "quick brown");
    const Edit b = highlight_edit(in, 3, "lazy dog");
    std::vector<int> ids[2];
    for (auto& run : ids) {
        auto w = start();
        (void)open_ok(*w, in);
        run.push_back(edit_ok(*w, a).annot_id);
        run.push_back(edit_ok(*w, b).annot_id);
    }
    CHECK(ids[0] == ids[1]);
    CHECK(ids[0][0] != ids[0][1]);
}

void moves_new_words_and_crop_boxes_replay() {
    // The M1 edits go through the same log: two fresh workers given the same
    // edits must end with the same annotations in the same places.
    const std::string in = corpus("text_10p.pdf");
    std::vector<leht::ops::AnnotInfo> seen[2];
    for (auto& run : seen) {
        auto w = start();
        (void)open_ok(*w, in);
        Edit add;
        add.kind = Edit::Kind::AddAnnot;
        add.page = 0;
        add.annot.kind = leht::ops::AnnotKind::FreeText;
        add.annot.rect = {100, 60, 220, 100};
        add.annot.contents = "first";
        const int id = edit_ok(*w, add).annot_id;

        Edit move;
        move.kind = Edit::Kind::MoveAnnot;
        move.annot_id = id;
        move.rects = {{300, 80, 460, 140}};
        CHECK(edit_ok(*w, move).pages == std::vector<int>({0}));
        Edit retext;
        retext.kind = Edit::Kind::SetAnnotContents;
        retext.annot_id = id;
        retext.text = "second";
        CHECK(edit_ok(*w, retext).pages == std::vector<int>({0}));
        Edit box;
        box.kind = Edit::Kind::CropBox;
        box.pages = "2-3";
        box.rects = {{50, 50, 400, 500}};
        const Edited cropped = edit_ok(*w, box);
        CHECK(cropped.pages == std::vector<int>({1, 2}));
        CHECK(cropped.base_sizes[1].width == 350 && cropped.base_sizes[2].height == 450);

        // Moving what cannot move is an ordinary failure, and the worker goes on.
        const int hl = edit_ok(*w, highlight_edit(in, 0, "quick brown")).annot_id;
        move.annot_id = hl;
        w->channel().send(990, move);
        CHECK(next(*w).type == MsgType::Failed);
        move.annot_id = 999999;
        w->channel().send(991, move);
        CHECK(next(*w).type == MsgType::Failed);

        w->channel().send(992, ListAnnots{});
        run = decode_as<AnnotList>(next(*w)).items;
    }
    CHECK(seen[0].size() == 2 && seen[1].size() == 2);
    for (std::size_t i = 0; i < seen[0].size() && i < seen[1].size(); ++i) {
        CHECK(seen[0][i].id == seen[1][i].id);
        CHECK(seen[0][i].rect.x0 == seen[1][i].rect.x0 && seen[0][i].rect.y1 == seen[1][i].rect.y1);
        CHECK(seen[0][i].contents == seen[1][i].contents);
    }
    const auto& ft = seen[0][0];
    CHECK(ft.type == "FreeText" && ft.contents == "second" && ft.movable && ft.resizable);
    CHECK(std::abs(ft.rect.x0 - 300) < 1 && std::abs(ft.rect.y1 - 140) < 1);
}

void edits_on_a_non_pdf_fail_cleanly() {
    auto w = start();
    (void)open_ok(*w, corpus("page.png"));
    Edit e;
    e.kind = Edit::Kind::RedactText;
    e.text = "x";
    w->channel().send(920, e);
    CHECK(next(*w).type == MsgType::Failed);
    w->channel().send(921, ListAnnots{});
    CHECK(next(*w).type == MsgType::Failed);
    // Still alive and serving.
    w->channel().send(922, Render{0, 0.5F, 0, 1});
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

/// The worker prepares, this process signs: the split the whole design rests
/// on. The worker here is the real one, sandboxed, so this also checks that
/// OpenSSL can verify inside seccomp -- everything it needs must be loaded
/// before the sandbox goes up.
void signing_through_the_worker() {
    static const leht::test::Pki pki;
    const leht::crypto::Identity id = pki.identity(pki.rsa, pki.rsa_cert);

    const leht::test::TempPath out("worker_signed.pdf");
    leht::ops::SignatureRequest request;
    request.name = "Mari Maasikas";
    request.reason = "Worker test";
    request.rect = {300, 650, 560, 740};
    request.appearance.lines = {"Mari Maasikas"};
    request.reserve = leht::crypto::estimate_signature_size(id, {});

    leht::ops::ByteRange range;
    {
        auto w = start();
        (void)open_ok(*w, corpus("text_10p.pdf"));
        const int fd = ::open(out.str().c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        CHECK(fd >= 0);
        w->channel().send(2, PrepareSignature{request}, fd);
        const Frame reply = next(*w);
        CHECK(reply.type == MsgType::SignaturePrepared);
        const auto prepared = decode_as<SignaturePrepared>(reply);
        CHECK(prepared.field == "Signature1");
        range = prepared.range;

        // A worker that lied about where the hole is cannot make the key sign
        // anything else: the check is on the file's bytes.
        leht::ops::ByteRange lie = range;
        lie.v[3] += 1;
        bool threw = false;
        try {
            (void)leht::crypto::sign_prepared(fd, lie, id, {});
        } catch (const leht::Error&) {
            threw = true;
        }
        CHECK(threw);

        const auto result = leht::crypto::sign_prepared(fd, range, id, {});
        CHECK(result.der_size > 0);
        ::close(fd);
        w->channel().send(3, Shutdown{});
        (void)w->wait_for(std::chrono::seconds(5));
    }

    // A fresh worker reads it back and verifies it, inside the sandbox.
    auto w = start();
    (void)open_ok(*w, out.str());
    w->channel().send(2, ListSignatures{pki.ca.pem()});
    const Frame reply = next(*w);
    CHECK(reply.type == MsgType::SignatureList);
    const auto list = decode_as<SignatureList>(reply);
    CHECK(list.rows.size() == 1);
    const SignatureRow& row = list.rows.front();
    CHECK(row.field == "Signature1");
    CHECK(row.page == 0);
    CHECK(row.subfilter == "ETSI.CAdES.detached");
    CHECK(row.name == "Mari Maasikas");
    CHECK(row.range_ok);
    CHECK(row.checked);
    CHECK(row.intact);
    CHECK(row.trust == static_cast<std::uint8_t>(leht::crypto::Trust::Trusted));
    CHECK(row.signer.common_name == "Mari Maasikas");
    CHECK(row.chain.size() == 2);
    CHECK(row.has_signing_certificate_v2);
    CHECK(!row.changed_after_signing);

    // Without the CA the same signature is intact but untrusted: the two
    // questions stay apart all the way to the viewer.
    w->channel().send(3, ListSignatures{""});
    const auto bare = decode_as<SignatureList>(next(*w));
    CHECK(bare.rows.size() == 1 && bare.rows[0].intact);
    CHECK(bare.rows[0].trust == static_cast<std::uint8_t>(leht::crypto::Trust::Untrusted));
}

/// The preload in leht-worker's main() is load-bearing, not belt and braces:
/// without it OpenSSL fetches an algorithm the first time it verifies, that
/// fetch opens a file, and seccomp kills the process. Proving it here keeps
/// anyone from tidying the preload away.
void verification_needs_the_preload_inside_the_sandbox() {
#if defined(__SANITIZE_ADDRESS__)
    std::printf("      SKIP sanitizer build runs the worker unsandboxed\n");
    return;
#else
    if (::getenv("LEHT_WORKER_NO_SANDBOX") != nullptr) {
        std::printf("      SKIP the sandbox is off in this environment\n");
        return;
    }
    static const leht::test::Pki pki;
    const leht::crypto::Identity id = pki.identity(pki.rsa, pki.rsa_cert);
    const leht::test::TempPath out("worker_preload.pdf");
    leht::ops::SignatureRequest request;
    request.reserve = leht::crypto::estimate_signature_size(id, {});
    {
        auto w = start();
        (void)open_ok(*w, corpus("text_10p.pdf"));
        const int fd = ::open(out.str().c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        CHECK(fd >= 0);
        w->channel().send(2, PrepareSignature{request}, fd);
        const auto prepared = decode_as<SignaturePrepared>(next(*w));
        (void)leht::crypto::sign_prepared(fd, prepared.range, id, {});
        ::close(fd);
    }
    ::setenv("LEHT_WORKER_NO_PRELOAD", "1", 1);
    auto w = start();
    ::unsetenv("LEHT_WORKER_NO_PRELOAD");
    (void)open_ok(*w, out.str());
    w->channel().send(2, ListSignatures{pki.ca.pem()});
    // No answer: the worker was killed mid-verification.
    CHECK(!w->channel().recv().has_value());
    const auto status = w->wait_for(std::chrono::seconds(5));
    CHECK(status.has_value() && status->crashed());
#endif
}

// --- OCR (M2) -------------------------------------------------------------------

#ifdef LEHT_HAVE_OCR
bool english_installed() {
    const auto langs = leht::ocr::installed_languages(leht::ocr::default_datadir());
    return std::find(langs.begin(), langs.end(), "eng") != langs.end();
}

void the_ocr_worker_reads_pixels_in_its_sandbox() {
    if (!english_installed()) {
        std::printf("      SKIP no Tesseract English data\n");
        return;
    }
    const float zoom = 300.0F / 72.0F;
    leht::Context ctx;
    leht::Document doc = leht::Document::open(ctx, corpus("text_10p.pdf"));
    const auto bmp = leht::Renderer(ctx, doc).render(0, zoom);
    CHECK(bmp.has_value());

    auto w = WorkerProcess::spawn(LEHT_WORKER_EXE, {"--ocr=eng"});
    w->handshake();
    Recognize r;
    r.zoom = zoom;
    r.bitmap = *bmp;
    w->channel().send(7, r);
    const Frame f = next(*w);
    CHECK(f.id == 7 && f.type == MsgType::Words);
    const auto words = decode_as<Words>(f).words;
    CHECK(words.size() > 300);
    CHECK(std::any_of(words.begin(), words.end(),
                      [](const leht::ops::OcrWord& x) { return x.text == "quick"; }));

    // Anything but pixels is refused, and the worker goes on reading.
    w->channel().send(8, ListAnnots{});
    CHECK(next(*w).type == MsgType::Failed);
    w->channel().send(9, r);
    CHECK(next(*w).type == MsgType::Words);

    // A language that is not installed: it cannot start at all.
    auto missing = WorkerProcess::spawn(LEHT_WORKER_EXE, {"--ocr=klingon"});
    bool handshook = true;
    try {
        missing->handshake();
    } catch (const std::exception&) {
        handshook = false;
    }
    CHECK(!handshook);
}

void the_ocr_worker_must_load_before_its_sandbox() {
#if defined(__SANITIZE_ADDRESS__)
    std::printf("      SKIP sanitizer build runs the worker unsandboxed\n");
    return;
#else
    if (::getenv("LEHT_WORKER_NO_SANDBOX") != nullptr || !english_installed()) {
        std::printf("      SKIP the sandbox is off, or no English data\n");
        return;
    }
    // Loading the language data after the sandbox is up opens a file under
    // seccomp: the worker is killed before it can even answer the Hello.
    ::setenv("LEHT_WORKER_NO_PRELOAD", "1", 1);
    auto w = WorkerProcess::spawn(LEHT_WORKER_EXE, {"--ocr=eng"});
    ::unsetenv("LEHT_WORKER_NO_PRELOAD");
    bool handshook = true;
    try {
        w->handshake();
    } catch (const std::exception&) {
        handshook = false;
    }
    CHECK(!handshook);
    const auto status = w->wait_for(std::chrono::seconds(5));
    CHECK(status.has_value() && status->signaled);
#endif
}
#endif

void a_text_layer_replays_and_is_found() {
    // The words ride in the edit log: replaying them needs no OCR at all.
    const std::string in = corpus("text_10p.pdf");
    for (int run = 0; run < 2; ++run) {
        auto w = start();
        (void)open_ok(*w, in);
        w->channel().send(1, ListTextPages{});
        CHECK(decode_as<TextPageList>(next(*w)).pages.size() == 10);  // born-digital
        Edit layer;
        layer.kind = Edit::Kind::AddTextLayer;
        layer.page = 2;
        layer.words = {{"Tere", {72, 30, 120, 48}}, {"õhtust", {126, 30, 190, 48}}};
        CHECK(edit_ok(*w, layer).pages == std::vector<int>({2}));
        w->channel().send(2, Search{"õhtust", 5});
        bool found = false;
        for (;;) {
            const Frame f = next(*w);
            if (f.type == MsgType::PageMatches) {
                found = found || decode_as<PageMatches>(f).page == 2;
            } else {
                CHECK(f.type == MsgType::SearchDone);
                break;
            }
        }
        CHECK(found);
    }
}

/// A document whose signature is nonsense must come back as a report, not as a
/// dead worker: this is hostile DER reaching OpenSSL inside the sandbox.
void a_broken_signature_is_reported_not_fatal() {
    leht::test::PdfWriter pdf;
    pdf.set(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] /SigFlags 3 >> >>");
    pdf.set(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    pdf.set(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [4 0 R] >>");
    pdf.set(4, "<< /FT /Sig /T (Sig1) /V 5 0 R /Type /Annot /Subtype /Widget "
               "/Rect [0 0 0 0] /P 3 0 R /F 132 >>");
    pdf.set(5, "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /ETSI.CAdES.detached "
               "/ByteRange [0 100 200 300] /Contents <deadbeef> >>");
    const leht::test::TempPath path("worker_bad_sig.pdf");
    leht::test::write_file(path.str(), pdf.finish(1));

    auto w = start();
    (void)open_ok(*w, path.str());
    w->channel().send(2, ListSignatures{""});
    const auto list = decode_as<SignatureList>(next(*w));
    CHECK(list.rows.size() == 1);
    CHECK(!list.rows[0].range_ok);
    CHECK(!list.rows[0].intact);
    CHECK(!list.rows[0].range_problem.empty());
    // Still alive and answering.
    w->channel().send(3, ListAnnots{});
    CHECK(next(*w).type == MsgType::AnnotList);
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
    RUN(edits_apply_render_and_save);
    RUN(redaction_through_the_worker);
    RUN(forms_through_the_worker);
    RUN(replay_is_deterministic);
    RUN(moves_new_words_and_crop_boxes_replay);
    RUN(a_text_layer_replays_and_is_found);
#ifdef LEHT_HAVE_OCR
    RUN(the_ocr_worker_reads_pixels_in_its_sandbox);
    RUN(the_ocr_worker_must_load_before_its_sandbox);
#endif
    RUN(edits_on_a_non_pdf_fail_cleanly);
    RUN(signing_through_the_worker);
    RUN(verification_needs_the_preload_inside_the_sandbox);
    RUN(a_broken_signature_is_reported_not_fatal);
    RUN(clean_shutdown);
    RUN(viewer_eof_ends_the_worker);
    return 0;
}
