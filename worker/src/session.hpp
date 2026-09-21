// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/ipc/channel.hpp"
#include "leht/ipc/protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

namespace leht {
class Cancel;
class Context;
class Document;
class Renderer;
class TextPage;
}  // namespace leht

namespace leht::worker {

/// One worker's whole life: the document it holds and the requests it serves.
///
/// Two threads. The reader thread only receives frames: it queues requests,
/// and handles Cancel immediately -- recording the newest generation and
/// aborting an in-flight render older than it -- as it does CancelSearch,
/// which stops a running search at the next page. The main thread (run()) pops
/// requests and does all MuPDF work, so the document is only ever touched by
/// one thread, as core/ requires.
class Session {
public:
    Session(ipc::Channel& channel, Context& ctx);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Serves requests until Shutdown or EOF. Returns the process exit code.
    int run();

private:
    void reader_loop();
    void dispatch(ipc::Frame& frame);

    void on_open(std::uint64_t id, ipc::Frame& frame);
    void on_authenticate(std::uint64_t id, const ipc::Authenticate& m);
    void on_render(std::uint64_t id, const ipc::Render& m);
    void on_search(std::uint64_t id, const ipc::Search& m);
    void on_select(std::uint64_t id, const ipc::Select& m);

    void finish_open(std::uint64_t id);
    void close_document() noexcept;
    TextPage& text_page(int page);

    ipc::Channel& channel_;
    Context& ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Renderer> renderer_;
    std::map<int, std::unique_ptr<TextPage>> text_pages_;

    // Queue between the reader thread and run().
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<ipc::Frame> queue_;
    bool closed_ = false;      ///< reader saw EOF or a protocol error
    int reader_exit_ = 0;

    static constexpr std::uint64_t kNone = std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> latest_generation_{0};
    std::atomic<std::uint64_t> in_flight_generation_{kNone};
    std::atomic<std::uint64_t> search_cancel_epoch_{0};  ///< searches older than this stop
    std::unique_ptr<Cancel> cancel_;
};

}  // namespace leht::worker
