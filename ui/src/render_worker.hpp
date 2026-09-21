// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QAtomicInteger>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include <QSet>

#include "outline_model.hpp"
#include "leht/ipc/process.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace leht {
class PageCache;
}  // namespace leht

/// The viewer's side of the document engine, running on its own thread.
///
/// Since M3 this object never parses a document. Each open spawns a fresh
/// sandboxed `leht-worker` process, hands it the file descriptor, and relays
/// its answers as the same signals the viewer has always consumed -- so
/// MainWindow, PageView and ThumbnailBar are unaware of the process boundary.
/// See docs/robustness.md, "Process isolation".
///
/// All worker I/O happens on this object's thread. The one exception is
/// setGeneration(), called on the GUI thread, which also sends the worker a
/// Cancel so an in-flight render of a page scrolled away from is aborted.
///
/// Requests carry a monotonic `generation`. When the user scrolls or zooms, the
/// view bumps the generation; stale requests are skipped here before being
/// sent, and the worker skips or aborts any it already has.
///
/// If the worker dies (a hostile file crashed MuPDF inside it): during a page
/// request, that page is marked poisoned and left blank, and a fresh worker
/// reopens the document so the rest keeps working. During open, or on a second
/// crash in the same document, failed() is emitted and the file is quarantined
/// for the session, so reopening it fails fast instead of crashing again.
class RenderWorker : public QObject {
    Q_OBJECT

public:
    RenderWorker();
    ~RenderWorker() override;

    /// Called from the GUI thread. Renders older than `generation` are dropped
    /// before being sent, and the worker process is told to skip or abort any
    /// it already has.
    void setGeneration(quint64 generation);

    /// Called from the GUI thread before a new search, or when find is closed.
    /// The running search (if any) stops emitting at once, never reports
    /// searchFinished, and the worker abandons it at the next page -- so a
    /// new search is not stuck behind an old one, nor are renders.
    void cancelSearch();

    /// The current worker process id, or 0 if none is running. For
    /// diagnostics and tests; safe to call from any thread.
    [[nodiscard]] qint64 workerPid() const;

public slots:
    /// Opens a document (worker thread). Emits opened() on success, failed() on
    /// a real error, or passwordRequired() if the file is encrypted -- in which
    /// case the document is held open awaiting authenticate().
    void open(const QString& path);

    /// Tries `password` on a document opened but awaiting one. Continues the
    /// open on success (emits opened()); re-emits passwordRequired(retry=true)
    /// on a wrong password.
    void authenticate(const QString& password);

    /// Renders one page at `zoom` to RGB. Skipped if `generation` is behind the
    /// latest set via setGeneration(). Emits rendered() on success.
    void render(int page, double zoom, int rotation, quint64 generation);

    /// Searches every page for `needle`. Emits pageMatches() per page as it goes
    /// (so highlights appear progressively) then searchFinished(). Match boxes
    /// are in BASE coordinates — page pixels at zoom 1.0 — so the view scales
    /// them to the current zoom and they survive zoom changes without re-search.
    void search(const QString& needle);

    /// Selects text between two points, given in base coordinates, on one page.
    /// `mode` is a leht::SelectMode cast to int. Emits selectionReady().
    void selectRegion(int page, QPointF aBase, QPointF bBase, int mode);

    /// Renders a small thumbnail of `page`, scaled so its width is about
    /// `targetWidth` px. Emits thumbnailReady(). Rendered outside the page
    /// cache so it does not evict full-size pages.
    void renderThumbnail(int page, int targetWidth);

    /// Renders `page` at `zoom` and RETURNS the image, for callers that need it
    /// synchronously (printing). Invoked from the GUI thread with a blocking
    /// queued connection; returns a null image on failure. Does not touch the
    /// page cache or the generation.
    Q_INVOKABLE QImage renderAt(int page, double zoom);

signals:
    void opened(int pageCount, QVector<QSize> baseSizes);
    void outlineReady(QVector<OutlineRow> rows);
    void passwordRequired(bool retry);
    void failed(const QString& message);
    void rendered(int page, double zoom, int rotation, quint64 generation, QImage image);
    /// `page` crashed the worker and will stay blank for this document.
    void pageFailed(int page);
    /// A search is about to report. Emitted from this thread, so it reaches the
    /// GUI after any matches an abandoned search had already sent: clearing
    /// highlights on it can never leave an old search's matches on screen.
    void searchStarted();
    void pageMatches(int page, QVector<QRectF> boxes);
    void searchFinished(int totalMatches);
    void selectionReady(int page, QVector<QRectF> boxes, QString text);
    void thumbnailReady(int page, QImage image);

private:
    /// What the viewer was doing when a worker died, which decides the response.
    enum class Phase { Open, Page, Search };

    /// Spawns and handshakes a fresh worker. Emits failed() and returns false
    /// if it cannot start.
    bool startWorker();

    /// Opens path_ in the current worker and handles the reply (and, when
    /// `password` is set, authenticates). Returns false after emitting what it
    /// needed to. With `silent`, success emits nothing: used to restore the
    /// document in a respawned worker.
    bool openInWorker(bool silent);

    /// Receives one frame. Returns nullopt if the worker has gone -- EOF, I/O
    /// failure, or a malformed frame, which is treated as a compromised worker.
    std::optional<leht::ipc::Frame> receive();

    /// Sends a request; false if the worker has gone.
    template <typename Msg>
    bool sendRequest(const Msg& msg, int fd = -1);

    /// The worker has gone while doing `phase` (on `page`, for a page
    /// request). Decides whether the document is to blame (see
    /// lossWasTheDocument), acts on it, and returns true only when the worker
    /// was killed from outside and a fresh one is ready: then, and only then,
    /// the caller may retry the request once.
    bool workerLost(Phase phase, int page);

    /// Whether a lost worker's end is evidence against the document: a crash
    /// signal, a non-zero exit, or garbage we killed it for -- yes; SIGKILL or
    /// SIGTERM from outside (the OOM killer, a user) -- no. Reaps the process.
    bool lossWasTheDocument(leht::ipc::WorkerProcess& proc);

    /// Kills the worker for sending a malformed frame and marks the loss as
    /// the document's fault.
    void distrust();

    /// Sends a request that has exactly one reply and returns it. Handles a
    /// lost worker, retrying once if it was killed from outside; nullopt
    /// means there is no reply and the loss has been dealt with.
    template <typename Msg>
    std::optional<leht::ipc::Frame> roundTrip(const Msg& msg, Phase phase, int page);

    /// Emits opened() and outlineReady() from the worker's replies.
    void publishOpened(const leht::ipc::Opened& result, const leht::ipc::Outline& outline);

    /// A synchronous render outside the generation machinery (thumbnails,
    /// printing). Null image on any failure.
    QImage renderUncached(int page, double zoom);

    void closeDocument();

    /// The live worker. Shared and atomic because setGeneration() reads it
    /// from the GUI thread while this thread may be replacing it.
    std::atomic<std::shared_ptr<leht::ipc::WorkerProcess>> proc_;
    std::uint64_t nextId_ = 1;

    QString path_;
    std::optional<std::string> password_;  ///< kept to re-authenticate after a respawn
    QVector<QSize> baseSizes_;
    QSet<int> poisoned_;                   ///< pages that crashed a worker
    int crashes_ = 0;                      ///< worker deaths blamed on this document
    int externalKills_ = 0;                ///< worker deaths from outside (OOM, kill)
    bool hostile_ = false;                 ///< the current worker was killed for sending garbage
    static constexpr int kMaxExternalKills = 3;

    std::unique_ptr<leht::PageCache> cache_;
    QAtomicInteger<quint64> generation_ = 0;
    QAtomicInteger<quint64> searchEpoch_ = 0;  ///< bumped by cancelSearch()
};
