// SPDX-License-Identifier: AGPL-3.0-or-later
#include "session.hpp"

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/crop.hpp"
#include "leht/ops/forms.hpp"
#include "leht/ops/redact.hpp"
#include "leht/ops/watermark.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <exception>
#include <functional>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

namespace leht::worker {

using namespace leht::ipc;

namespace {

/// The biggest bitmap worth rendering is one that still fits in a frame.
constexpr std::size_t kMaxBitmapBytes = kMaxPayload - 4096;

void flatten(const std::vector<OutlineItem>& items, int depth, std::vector<OutlineRow>& out) {
    for (const OutlineItem& item : items) {
        out.push_back(OutlineRow{depth, item.title, item.page, item.y});
        flatten(item.children, depth + 1, out);
    }
}

}  // namespace

Session::Session(Channel& channel, Context& ctx)
    : channel_(channel), ctx_(ctx), cancel_(std::make_unique<leht::Cancel>()) {}

Session::~Session() { close_document(); }

void Session::close_document() noexcept {
    // Reverse dependency order: everything below borrows the document.
    text_pages_.clear();
    renderer_.reset();
    doc_.reset();
}

void Session::reader_loop() {
    try {
        while (auto frame = channel_.recv()) {
            if (frame->type == MsgType::Cancel) {
                const std::uint64_t gen = decode_as<ipc::Cancel>(*frame).generation;
                latest_generation_.store(gen);
                // Abort the in-flight render if it is older. See run() for why
                // this ordering cannot miss one.
                if (in_flight_generation_.load() < gen) {
                    cancel_->request();
                }
                continue;
            }
            if (frame->type == MsgType::CancelSearch) {
                const std::uint64_t epoch = decode_as<CancelSearch>(*frame).epoch;
                std::uint64_t seen = search_cancel_epoch_.load();
                while (seen < epoch && !search_cancel_epoch_.compare_exchange_weak(seen, epoch)) {
                }
                continue;
            }
            const bool shutdown = frame->type == MsgType::Shutdown;
            {
                const std::scoped_lock lock(mutex_);
                queue_.push_back(std::move(*frame));
            }
            ready_.notify_one();
            if (shutdown) {
                return;
            }
        }
    } catch (const std::exception& e) {
        // The viewer is the trusted side; garbage from it means something is
        // badly wrong. Stop rather than guess.
        std::fprintf(stderr, "leht-worker: %s\n", e.what());
        reader_exit_ = 2;
    }
    {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
    }
    ready_.notify_one();
}

int Session::run() {
    std::thread reader([this] { reader_loop(); });
    int exit_code = 0;

    for (;;) {
        Frame frame;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return !queue_.empty() || closed_; });
            if (queue_.empty()) {
                break;  // EOF: the viewer is gone
            }
            frame = std::move(queue_.front());
            queue_.pop_front();
        }
        if (frame.type == MsgType::Shutdown) {
            break;
        }
        try {
            dispatch(frame);
        } catch (const ProtocolError& e) {
            std::fprintf(stderr, "leht-worker: bad request: %s\n", e.what());
            exit_code = 2;
            break;
        } catch (const std::system_error&) {
            break;  // a send failed: the viewer has gone, so there is no one to serve
        }
    }

    channel_.shutdown();  // unblocks the reader if it is still in recv()
    reader.join();
    return exit_code != 0 ? exit_code : reader_exit_;
}

void Session::dispatch(Frame& frame) {
    const std::uint64_t id = frame.id;
    try {
        switch (frame.type) {
        case MsgType::Hello:
            channel_.send(id, HelloAck{});
            return;
        case MsgType::Open:
            on_open(id, frame);
            return;
        case MsgType::Authenticate:
            on_authenticate(id, decode_as<Authenticate>(frame));
            return;
        case MsgType::Render:
            on_render(id, decode_as<Render>(frame));
            return;
        case MsgType::Search:
            on_search(id, decode_as<Search>(frame));
            return;
        case MsgType::Select:
            on_select(id, decode_as<Select>(frame));
            return;
        case MsgType::Edit:
            on_edit(id, decode_as<Edit>(frame));
            return;
        case MsgType::Save:
            on_save(id, frame);
            return;
        case MsgType::ListAnnots:
            (void)decode_as<ListAnnots>(frame);
            on_list_annots(id);
            return;
        case MsgType::ListFields:
            (void)decode_as<ListFields>(frame);
            on_list_fields(id);
            return;
        default:
            throw ProtocolError("not a request type");
        }
    } catch (const ProtocolError&) {
        throw;
    } catch (const std::bad_alloc&) {
        // Hitting RLIMIT_AS is how a decompression or allocation bomb ends up
        // here: an ordinary failure of this request, not of the worker.
        channel_.send(id, Failed{"out of memory"});
    } catch (const std::exception& e) {
        channel_.send(id, Failed{e.what()});
    }
}

void Session::on_open(std::uint64_t id, Frame& frame) {
    const Open m = decode_as<Open>(frame);
    close_document();
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to Open"});
        return;
    }
    const std::string hint = m.name.empty() ? std::string("pdf") : m.name;
    doc_ = std::make_unique<Document>(Document::open_fd(ctx_, frame.fd.release(), hint));
    if (doc_->needs_password()) {
        channel_.send(id, NeedsPassword{false});
        return;
    }
    finish_open(id);
}

void Session::on_authenticate(std::uint64_t id, const Authenticate& m) {
    if (!doc_) {
        channel_.send(id, Failed{"no document is open"});
        return;
    }
    if (!doc_->needs_password()) {
        finish_open(id);  // already unlocked; answer as if it just opened
        return;
    }
    if (doc_->authenticate(m.password)) {
        finish_open(id);
    } else {
        channel_.send(id, NeedsPassword{true});
    }
}

std::vector<PageSize> Session::base_sizes() {
    std::vector<PageSize> sizes;
    const int pages = doc_->page_count();
    sizes.reserve(static_cast<std::size_t>(pages));
    for (int p = 0; p < pages; ++p) {
        try {
            sizes.push_back(renderer_->page_size(p, 1.0F));
        } catch (const Error&) {
            sizes.push_back(PageSize{});  // a broken page, not a broken document
        }
    }
    return sizes;
}

void Session::finish_open(std::uint64_t id) {
    renderer_ = std::make_unique<Renderer>(ctx_, *doc_);
    text_pages_.clear();
    channel_.send(id, Opened{base_sizes()});

    Outline outline;
    try {
        flatten(doc_->outline(), 0, outline.rows);
    } catch (const Error&) {
        outline.rows.clear();  // a damaged outline is optional; the pages are not
    }
    channel_.send(id, outline);
}

void Session::on_render(std::uint64_t id, const Render& m) {
    const RenderSkipped skipped{m.page, m.generation};
    if (!renderer_) {
        channel_.send(id, skipped);
        return;
    }

    // Publish what is in flight BEFORE checking staleness, and rearm the token
    // only then. The reader stores the new generation before reading ours, so
    // either we see its generation here and skip, or it sees ours and aborts
    // the render -- a cancel cannot fall between the two.
    in_flight_generation_.store(m.generation);
    cancel_->reset();
    struct Clear {
        std::atomic<std::uint64_t>& g;
        ~Clear() { g.store(kNone); }
    } clear{in_flight_generation_};

    if (m.generation < latest_generation_.load()) {
        channel_.send(id, skipped);
        return;
    }

    try {
        const PageSize size = renderer_->page_size(m.page, m.zoom, m.rotation);
        const auto bytes = static_cast<std::size_t>(size.width) *
                           static_cast<std::size_t>(size.height) * 3U;
        if (size.width <= 0 || size.height <= 0 || bytes > kMaxBitmapBytes) {
            channel_.send(id, skipped);
            return;
        }
        auto bmp = renderer_->render(m.page, m.zoom, m.rotation, cancel_.get());
        if (!bmp) {
            channel_.send(id, skipped);  // cancelled
            return;
        }
        Rendered out;
        out.page = m.page;
        out.zoom = m.zoom;
        out.rotation = m.rotation;
        out.generation = m.generation;
        out.bitmap = std::move(*bmp);
        channel_.send(id, out);
    } catch (const Error&) {
        channel_.send(id, skipped);  // a bad page renders blank
    }
}

TextPage& Session::text_page(int page) {
    auto it = text_pages_.find(page);
    if (it == text_pages_.end()) {
        // Zoom 1.0: coordinates in base page pixels, which the viewer scales.
        it = text_pages_.emplace(page, std::make_unique<TextPage>(ctx_, *doc_, page, 1.0F)).first;
    }
    return *it->second;
}

void Session::on_search(std::uint64_t id, const Search& m) {
    if (!doc_ || !renderer_) {
        channel_.send(id, Failed{"no document is open"});
        return;
    }
    const auto cancelled = [&] { return m.epoch < search_cancel_epoch_.load(); };
    std::uint32_t total = 0;
    if (!m.needle.empty()) {
        const int pages = doc_->page_count();
        for (int p = 0; p < pages && !cancelled(); ++p) {
            PageMatches matches;
            matches.page = p;
            try {
                for (const SearchHit& hit : text_page(p).search(m.needle)) {
                    matches.quads.insert(matches.quads.end(), hit.quads.begin(), hit.quads.end());
                }
            } catch (const Error&) {
                continue;  // an unreadable page contributes no matches
            }
            if (!matches.quads.empty()) {
                total += static_cast<std::uint32_t>(matches.quads.size());
                channel_.send(id, matches);
            }
        }
    }
    channel_.send(id, SearchDone{total});
}

void Session::on_select(std::uint64_t id, const Select& m) {
    if (!doc_ || !renderer_) {
        channel_.send(id, Failed{"no document is open"});
        return;
    }
    Selection sel = text_page(m.page).select(m.ax, m.ay, m.bx, m.by, m.mode);
    channel_.send(id, SelectionResult{m.page, std::move(sel.quads), std::move(sel.text)});
}

bool Session::require_document(std::uint64_t id) {
    if (!doc_ || !renderer_) {
        channel_.send(id, Failed{"no document is open"});
        return false;
    }
    return true;
}

void Session::on_edit(std::uint64_t id, const Edit& m) {
    if (!require_document(id)) {
        return;
    }
    Edited out;
    switch (m.kind) {
    case Edit::Kind::Redact: {
        const ops::RedactResult r = ops::redact(ctx_, *doc_, m.page, m.rects);
        out.pages = r.pages;
        break;
    }
    case Edit::Kind::RedactText: {
        ops::RedactResult r = ops::redact_text(ctx_, *doc_, m.text);
        out.pages = r.pages;
        out.remaining = std::move(r.remaining);
        break;
    }
    case Edit::Kind::AddAnnot:
        out.annot_id = ops::add_annotation(ctx_, *doc_, m.page, m.annot);
        out.pages = {m.page};
        break;
    case Edit::Kind::DeleteAnnot: {
        int page = -1;
        for (const ops::AnnotInfo& a : ops::list_annotations(ctx_, *doc_)) {
            if (a.id == m.annot_id) {
                page = a.page;
            }
        }
        if (page < 0 || !ops::delete_annotation(ctx_, *doc_, m.annot_id)) {
            channel_.send(id, Failed{"no annotation with that id"});
            return;
        }
        out.pages = {page};
        break;
    }
    case Edit::Kind::SetField:
        ops::set_field(ctx_, *doc_, m.name, m.text);
        out.all_pages = true;  // a field's widgets may be on several pages
        break;
    case Edit::Kind::Watermark:
        (void)ops::watermark(ctx_, *doc_, m.pages, m.watermark);
        out.pages = page_set(m.pages, doc_->page_count());
        break;
    case Edit::Kind::CropMargins:
        (void)ops::crop_margins(ctx_, *doc_, m.pages, m.margins);
        out.pages = page_set(m.pages, doc_->page_count());
        break;
    }
    // Every cached display list and text layer may now be out of date.
    renderer_->clear_cache();
    text_pages_.clear();
    out.base_sizes = base_sizes();
    channel_.send(id, out);
}

void Session::on_save(std::uint64_t id, Frame& frame) {
    (void)decode_as<Save>(frame);
    if (!require_document(id)) {
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to Save"});
        return;
    }
    // The descriptor stays owned by the frame and closes with it. The viewer
    // makes the file durable and renames it; the worker can do neither.
    doc_->save_fd(frame.fd.get(), SaveOptions{});
    struct stat st {};
    const std::uint64_t bytes =
        ::fstat(frame.fd.get(), &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    channel_.send(id, Saved{bytes});
}

void Session::on_list_annots(std::uint64_t id) {
    if (!require_document(id)) {
        return;
    }
    channel_.send(id, AnnotList{ops::list_annotations(ctx_, *doc_)});
}

void Session::on_list_fields(std::uint64_t id) {
    if (!require_document(id)) {
        return;
    }
    channel_.send(id, FieldList{ops::list_fields(ctx_, *doc_)});
}

}  // namespace leht::worker
