// SPDX-License-Identifier: AGPL-3.0-or-later
#include "session.hpp"

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"

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

void Session::finish_open(std::uint64_t id) {
    renderer_ = std::make_unique<Renderer>(ctx_, *doc_);
    text_pages_.clear();

    Opened opened;
    const int pages = doc_->page_count();
    opened.base_sizes.reserve(static_cast<std::size_t>(pages));
    for (int p = 0; p < pages; ++p) {
        try {
            opened.base_sizes.push_back(renderer_->page_size(p, 1.0F));
        } catch (const Error&) {
            opened.base_sizes.push_back(PageSize{});  // a broken page, not a broken document
        }
    }
    channel_.send(id, opened);

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
    std::uint32_t total = 0;
    if (!m.needle.empty()) {
        const int pages = doc_->page_count();
        for (int p = 0; p < pages; ++p) {
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

}  // namespace leht::worker
