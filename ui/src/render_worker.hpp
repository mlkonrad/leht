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
#include "leht/crypto/crypto.hpp"
#include "outline_model.hpp"
#include "leht/ipc/process.hpp"
#include "leht/ipc/protocol.hpp"

#include <QColor>
#include <QPolygonF>
#include <QStringList>

#include <atomic>
#include <memory>
#include <functional>
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
    /// An underline or strike-out over the text `boxes` cover.
    void addTextMarkup(int page, QVector<QRectF> boxes, bool strikeOut, QColor color);
    /// A standard rubber stamp ("Approved", "Draft", ...) filling `box`.
    void addStamp(int page, QRectF box, QString name);
    /// A freehand drawing: one polygon per stroke.
    void addInk(int page, QVector<QPolygonF> strokes, QColor color);
    /// Removes everything under `box` (see ops::redact).
    void redactArea(int page, QRectF box);
    /// Removes every occurrence of `needle`. May emit redactionIncomplete().
    void redactText(QString needle);
    void deleteAnnotation(int id);
    void setFieldValue(QString name, QString value);
    /// Bakes every form field into its page: shown as filled, no longer editable.
    void flattenForm();
    /// Moves annotation `id` so its bounds become `to` (see ops::move_annotation).
    void moveAnnotation(int id, QRectF to);
    /// Free text written in `box`, at `size` points in `color`.
    void addFreeText(int page, QRectF box, QString text, double size, QColor color);
    /// New words for a free-text annotation or a note.
    void setAnnotationText(int id, QString text);
    /// A comment's colour, opacity, line width, text size and author.
    void setAnnotationStyle(int id, QColor color, double opacity, double lineWidth,
                            double fontSize, QString author);
    /// `pages` is a page-range spec; empty means every page.
    void addWatermark(QString pages, leht::ops::WatermarkOptions options);
    void cropMargins(QString pages, leht::ops::Margins margins);
    /// Keeps only `box` of each page in `pages`: hides the rest (see ops::crop).
    void cropBox(QString pages, QRectF box);
    // Organising pages (ops/organize.hpp); `pages` is a 1-based range spec,
    // positions are 0-based ("before the page now at").
    void rotatePages(QString pages, int degrees);
    void deletePages(QString pages);
    void movePages(QString pages, int before);
    /// `data` is the whole PDF file, read by the caller; the worker parses it.
    void insertPages(int at, QByteArray data, QString pages);
    void insertBlankPage(int at, QSizeF size);
    /// File > Document Properties: answered by infoReady().
    void requestInfo();
    /// File > Export > Text: answered by textReady(), one string per page.
    void extractText(QString pages);
    /// One page's text for a screen reader: answered by pageTextRead(), apart
    /// from textReady() so it never lands in an export.
    void readPageText(int page);
    /// Sets Title, Author, Subject or Keywords ("" removes it), as an edit.
    void setInfo(QString key, QString value);

    void undo();
    void redo();
    /// Edits made between these two calls undo and redo as one step: an OCR
    /// run is an edit per page, and one Undo takes the whole run back.
    void beginEditGroup();
    void endEditGroup();

    /// OCR: renders each page of `pages` (a range spec; empty is all) at
    /// `dpi`, has a sandboxed OCR worker read it in `languages` ("est+eng"),
    /// and writes the words as an invisible text layer. Pages that already
    /// have text are left out when `skipPagesWithText`. One undo step for the
    /// whole run; pages finished before a cancel are kept. Emits ocrProgress
    /// per page and ocrFinished at the end.
    void recognizeText(QString pages, QString languages, int dpi, bool skipPagesWithText);

public:
    /// What is cached of the EU trusted lists, for the window's note: none,
    /// when (and whether it is overdue for an update).
    struct TrustedListState {
        bool present = false;
        qint64 built = 0;
        bool overdue = false;  ///< the LOTL or a list is past its next update
    };
    static TrustedListState trustedListState();

    /// Stops a recognizeText() run after the page it is on. Safe to call
    /// from any thread -- the GUI calls it while this one is busy.
    void cancelRecognition() { ocrCancel_ = true; }
    /// Stops waiting on a phone (Smart-ID, Mobile-ID) within about a second;
    /// nothing is written. Safe to call from any thread.
    void cancelPhoneSigning() { phoneCancel_ = true; }

public slots:

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

    // --- Long-term validation (M4) --------------------------------------------
    //
    // The worker, which parses the document, says what to fetch and parses what
    // comes back; this thread only moves the bytes. These, and a timestamp
    // authority, are the only times the viewer touches the network, and only
    // when asked. networkUsed() names the hosts first.

    /// Embeds validation data for every signature (B-LT) and, with `tsaUrl`,
    /// a document timestamp over it all (B-LTA), writing `path`. Refused while
    /// there are unsaved edits: they would ride along in a revision that
    /// claims to add only validation data. Emits saved() or saveFailed().
    void addLongTermValidation(QString path, QString tsaUrl);

    /// Verifies every signature against revocation data fetched now, as well
    /// as the document's own; nothing is embedded. Emits signaturesReady().
    void checkRevocationOnline();

    // --- The EU trusted lists (queue M5) -----------------------------------------
    //
    // Fetched only when asked. This thread moves bytes; leht-worker
    // --trusted-list, in its own sandbox, reads and verifies the XML. The
    // verified lists are cached where the CLI keeps them too, and every
    // signature list afterwards is verified with them.

    /// Fetches and verifies the lists, and caches them. Emits networkUsed()
    /// before each round of fetching, then trustedListUpdated() or
    /// trustedListFailed().
    void updateTrustedList();

    /// Certificates to trust beyond the system's, remembered between sessions.
    void addTrustedCertificate(QString pemPath);

signals:
    /// `done` of `total` pages read; `page` is the one being read now, -1 at the end.
    void ocrProgress(int done, int total, int page);
    /// A recognizeText() run ended: `words` read on `pages` pages. `error` is
    /// empty unless it stopped for a reason other than a cancel.
    void ocrFinished(int words, int pages, bool cancelled, QString error);
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
    /// Pages were moved, deleted or inserted (or such an edit undone): what
    /// was shown for page N is not page N any more. Comes just before the
    /// documentEdited() for it.
    void pagesRearranged();
    /// The document's facts, as Document::metadata() keys and values.
    void infoReady(QStringList keys, QStringList values);
    /// `texts[i]` is page `pages[i]`'s text (0-based); empty lists on failure.
    void textReady(QVector<int> pages, QStringList texts);
    void pageTextRead(int page, QString text);
    void documentEdited(QVector<int> pages, bool allPages, QVector<QSize> baseSizes);
    /// `modified`: there are edits since the file was opened or last saved.
    void editStateChanged(bool canUndo, bool canRedo, bool modified);
    void editFailed(QString message);
    /// A text redaction was applied, but the text still appears in `where`.
    void redactionIncomplete(QStringList where);
    void saved(QString path);
    void saveFailed(QString message);
    /// Signing with a phone: it began (`service` is "Smart-ID" or
    /// "Mobile-ID"); a QR link to show, renewed every second; a verification
    /// code to compare; what is happening. Ended comes last, whatever the
    /// outcome -- after saved() or saveFailed(), or alone when cancelled.
    void phoneSigningStarted(QString service);
    void phoneLink(QString link);
    void phoneCode(QString code);
    void phoneStatus(QString text);
    void phoneSigningEnded(bool cancelled);
    void annotationsReady(QVector<AnnotRow> rows);
    void fieldsReady(QVector<FieldRow> rows);
    void signaturesReady(QVector<SigRow> rows);
    /// About to fetch revocation data from these hosts.
    void networkUsed(QString hosts);
    /// addLongTermValidation() wrote what it says; `timestamp` is 0 when no
    /// document timestamp was added.
    void longTermValidationAdded(int certs, int ocsps, int crls, qint64 timestamp);
    /// updateTrustedList() cached `verified` of `lists` national lists, with
    /// `services` qualified services; `failed` names the ones that failed.
    void trustedListUpdated(int verified, int lists, int services, QStringList failed);
    void trustedListFailed(QString why);

private:
    /// The phone behind a SignSpec's phoneMethod (sk.cpp does the talking).
    leht::crypto::Identity phoneIdentity(const SignSpec& spec, const QString& path);
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

    /// Signature list, verified with `online` responses on top of the
    /// document's own validation data.
    void listSignaturesWith(const std::vector<leht::ipc::FetchedRow>& online);
    /// What the worker says to fetch for this document, fetched. nullopt
    /// after reporting a failure through `fail`.
    std::optional<std::vector<leht::ipc::FetchedRow>> fetchRevocation(
        const std::function<void(QString)>& fail);
    /// Sends `msg` with `fd` for the worker to write into and returns its
    /// reply of type `want`. On anything else -- a failure, a dead worker,
    /// garbage -- reports through saveFailed(), prefixed by `context`, and
    /// returns nullopt.
    template <typename Msg>
    std::optional<leht::ipc::Frame> fdRequest(const Msg& msg, int fd, leht::ipc::MsgType want,
                                              const QString& context);

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
    void rebuild(bool rearranged = false);
    void publishEdited(const leht::ipc::Edited& edited);
    void publishEditState();

    std::vector<leht::ipc::Edit> log_;   ///< applied since open or last save
    std::vector<leht::ipc::Edit> redo_;  ///< undone, newest last
    // Undo groups, one per entry of log_ and redo_: entries sharing a group
    // go back and forth together.
    std::vector<std::uint64_t> logGroups_;
    std::vector<std::uint64_t> redoGroups_;
    std::uint64_t nextGroup_ = 1;
    std::uint64_t openGroup_ = 0;  ///< nonzero between begin/endEditGroup
    std::uint64_t redoing_ = 0;    ///< the group a redo is putting back
    std::atomic<bool> ocrCancel_{false};
    std::atomic<bool> phoneCancel_{false};
    void clearLog();

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
