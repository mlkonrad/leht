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

#include "edit_model.hpp"
#include "outline_model.hpp"
#include "leht/ipc/process.hpp"
#include "leht/ipc/protocol.hpp"

#include <QColor>
#include <QPolygonF>
#include <QStringList>

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

    // --- Editing (M4b) -------------------------------------------------------
    //
    // Every edit goes to the worker as an ipc::Edit and, once applied, joins
    // the edit log: the list of edits since the file was opened or last saved.
    // The log is the source of truth. Undo reopens the file and replays all
    // but the last edit; a worker that crashes is respawned and gets the
    // whole log. So the worker's document is always (file + log), and nothing
    // is lost to a crash.
    //
    // Geometry is in base coordinates. Each emits documentEdited() and
    // editStateChanged() on success, or editFailed() with a reason.

    /// Highlights the text under `boxes` (base-coordinate rects, one per line).
    void addHighlight(int page, QVector<QRectF> boxes, QColor color);
    /// A sticky note at `at` saying `text`.
    void addNote(int page, QPointF at, QString text);
    /// A freehand drawing: one polygon per stroke.
    void addInk(int page, QVector<QPolygonF> strokes, QColor color);
    /// Removes everything under `box` (see ops::redact).
    void redactArea(int page, QRectF box);
    /// Removes every occurrence of `needle`. May emit redactionIncomplete().
    void redactText(QString needle);
    void deleteAnnotation(int id);
    void setFieldValue(QString name, QString value);
    void addWatermark(QString text);
    void cropMargins(double points);

    void undo();
    void redo();

    /// Writes the edited document to `path`: into a temporary file beside it,
    /// written by the worker through a passed descriptor, then fsynced and
    /// renamed over `path`. The saved file becomes the document: the edit log
    /// and undo history start afresh from it. Emits saved() or saveFailed().
    void save(QString path);

    /// Emit annotationsReady() / fieldsReady() with the current lists.
    void listAnnotations();
    void listFields();

    // --- Signing (M5) --------------------------------------------------------
    //
    // The private key never reaches the worker, which is the process that
    // parses hostile PDFs. The worker writes the document plus a signature
    // whose /Contents is a hole of zeros; this side then checks, on the bytes
    // of the file itself, that the signed ranges are everything but that hole,
    // signs them, and fills it in. A worker that lied about the hole can
    // produce an invalid signature, never a signature over other bytes.

    /// Signs the document (with any unsaved edits, in the same revision) and
    /// writes it to `path`. Emits saved() -- signing IS a save, so the edit log
    /// restarts from the signed file -- or saveFailed().
    void signDocument(QString path, SignSpec spec);

    /// Verifies every signature and emits signaturesReady(). Verification runs
    /// in the worker; the trust store travels there as PEM.
    void listSignatures();

    /// Certificates to trust beyond the system's, remembered between sessions.
    void addTrustedCertificate(QString pemPath);

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

    /// The document changed. Rendered images of `pages` (of every page, if
    /// `allPages`) are out of date, and `baseSizes` are the page sizes now.
    void documentEdited(QVector<int> pages, bool allPages, QVector<QSize> baseSizes);
    /// `modified`: there are edits since the file was opened or last saved.
    void editStateChanged(bool canUndo, bool canRedo, bool modified);
    void editFailed(QString message);
    /// A text redaction was applied, but the text still appears in `where`.
    void redactionIncomplete(QStringList where);
    void saved(QString path);
    void saveFailed(QString message);
    void annotationsReady(QVector<AnnotRow> rows);
    void fieldsReady(QVector<FieldRow> rows);
    void signaturesReady(QVector<SigRow> rows);

private:
    /// What the viewer was doing when a worker died, which decides the response.
    enum class Phase { Open, Page, Search, Edit };

    /// Spawns and handshakes a fresh worker. Emits failed() and returns false
    /// if it cannot start.
    bool startWorker();

    /// Opens path_ in the current worker and handles the reply (and, when
    /// `password` is set, authenticates). Returns false after emitting what it
    /// needed to. With `silent`, success emits nothing: used to restore the
    /// document in a respawned worker.
    bool openInWorker(bool silent);

    /// Receives one frame. Returns nullopt if the worker has gone -- EOF, I/O
    /// failure, a malformed frame (treated as a compromised worker), or no
    /// frame within the request timeout (the worker is killed and the file
    /// blamed, as for a crash).
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

    /// Sends `edit`; on success appends it to the log and publishes the
    /// change. `fromRedo` keeps the redo stack; a fresh edit clears it.
    void applyEdit(const leht::ipc::Edit& edit, bool fromRedo = false);
    /// Replays the whole log into the current worker, which has just opened
    /// the file. False if the worker was lost doing it (already handled).
    bool replayLog();
    /// Reopens the file in the current worker and replays the log, then
    /// publishes every page as changed. Used by undo and after a failed edit.
    void rebuild();
    void publishEdited(const leht::ipc::Edited& edited);
    void publishEditState();

    std::vector<leht::ipc::Edit> log_;   ///< applied since open or last save
    std::vector<leht::ipc::Edit> redo_;  ///< undone, newest last

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

    /// The system trust store plus the user's own certificates, as PEM.
    /// Reading files is this side's job; the worker cannot open any.
    [[nodiscard]] std::string trustPem();

    std::unique_ptr<leht::PageCache> cache_;
    QAtomicInteger<quint64> generation_ = 0;
    QAtomicInteger<quint64> searchEpoch_ = 0;  ///< bumped by cancelSearch()
};
