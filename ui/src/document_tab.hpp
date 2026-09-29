// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QThread>
#include <QVector>
#include <QWidget>

#include <functional>

#include "edit_model.hpp"
#include "outline_model.hpp"

class CommentsPanel;
class FormPanel;
class PageGrid;
class PageView;
class QLabel;
class QPrinter;
class QSplitter;
class QStackedWidget;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class RenderWorker;
class Sidebar;
class SignatureCards;
class ThumbnailBar;

/// What a certification at `level` still allows, in words ("form filling and
/// signing allowed"); empty for 0.
QString certificationWords(int level);

/// One open document: a tab in a MainWindow. It owns the page view, the
/// render worker and its thread, the sidebar with its panels, the page grid
/// and the signature banner, and everything the window used to know about
/// the document (path, edits, signatures, redaction marks…).
///
/// It works in any window. It never reaches into one: what the window shows
/// (status bar, actions, mode bar) it hears through signals, and dialogs are
/// parented to window(), so a tab moved to another window takes nothing of
/// the old one with it.
class DocumentTab : public QWidget {
    Q_OBJECT

public:
    explicit DocumentTab(QWidget* parent = nullptr);
    ~DocumentTab() override;

    /// Opens `path` here, replacing what the tab shows (asks about unsaved
    /// edits first).
    void openPath(const QString& path);
    /// The page to go to once the document opens (0-based; restored tabs).
    void setStartPage(int page) { startPage_ = page; }
    /// A welcome task ("sign", "fill"…) waiting for the document to open;
    /// the window runs it when opened() comes.
    void setPendingTask(const QString& task);
    [[nodiscard]] QString takePendingTask() { return std::exchange(pendingTask_, QString()); }

    // What the window asks of the document.
    [[nodiscard]] bool isOpen() const { return pageCount_ > 0; }
    [[nodiscard]] int pageCount() const { return pageCount_; }
    [[nodiscard]] QString path() const { return currentPath_; }
    [[nodiscard]] QString title() const { return currentTitle_; }
    /// The tab's text: the file name, and " *" while there are unsaved edits.
    [[nodiscard]] QString tabText() const;
    [[nodiscard]] bool isModified() const { return modified_; }
    [[nodiscard]] bool canUndo() const { return canUndo_; }
    [[nodiscard]] bool canRedo() const { return canRedo_; }
    [[nodiscard]] int certLevel() const { return certLevel_; }
    /// Whether the certification level allows what needs `needs` (see
    /// ActionRegistry::Spec::certNeeds).
    [[nodiscard]] bool certAllows(int needs) const;
    [[nodiscard]] int signatureCount() const { return signatureCount_; }
    [[nodiscard]] bool isEncrypted() const { return documentEncrypted_; }
    [[nodiscard]] int pendingRedactions() const { return static_cast<int>(redactionMarks_.size()); }
    [[nodiscard]] int currentPage() const;
    [[nodiscard]] bool isShowingGrid() const;

    [[nodiscard]] PageView* view() const { return view_; }
    [[nodiscard]] RenderWorker* worker() const { return worker_; }
    [[nodiscard]] Sidebar* sidebar() const { return sidebar_; }
    [[nodiscard]] PageGrid* pageGrid() const { return pageGrid_; }
    [[nodiscard]] FormPanel* formPanel() const { return form_; }
    /// The page view or the page grid, whichever shows (F6 goes there).
    [[nodiscard]] QWidget* pageArea() const;

    /// The mode and tool this tab was left in; the window puts them back
    /// when the tab comes forward again.
    [[nodiscard]] QString mode() const { return mode_; }
    void setMode(const QString& mode) { mode_ = mode; }
    [[nodiscard]] QString tool() const { return tool_; }
    void setTool(const QString& toolId) { tool_ = toolId; }

    /// Runs `fn` on the worker thread, in order with every other request.
    void onWorker(std::function<void(RenderWorker*)> fn);

    // Saving.
    /// Saves to the file it came from (or asks, if it has none). Returns
    /// false if nothing was started.
    bool save();
    bool saveAs();
    /// Asks what to do with unsaved edits. True means carry on now; false
    /// means stop (cancelled, or a save was started and `then` will run when
    /// it succeeds).
    bool resolveUnsaved(std::function<void()> then);

    // Commands, as the window's actions ask for them.
    /// Tools > Apply Redactions: removes everything under the marks, as one
    /// edit group. Asks first when `confirm`. False if nothing was marked or
    /// the user called it off.
    bool applyRedactions(bool confirm = true);
    void clearRedactionMarks();
    void redactText();
    void addWatermark();
    void cropMargins();
    void flattenForm();
    void showProperties();
    /// More -> Recognize Text (OCR)...
    void recognizeText();
    /// Opens the Sign dialog for a box on `page` (an empty box signs
    /// invisibly), then signs -- which saves, so it asks where to when the
    /// document has no path yet.
    void startSigning(int page, QRectF rect);
    /// More -> Add Long-Term Validation...
    void addLongTermValidation();
    void checkRevocation();
    /// Checks the signatures again (the trusted certificates changed).
    void recheckSignatures();

    // Find.
    void search(const QString& needle);
    void cancelSearch();

    // Printing and export.
    void printDialog();
    /// Prints pages [fromPage, toPage] (1-based; 0,0 = all) to `printer`.
    /// Returns false if nothing was printed.
    bool printDocument(QPrinter& printer, int fromPage = 0, int toPage = 0);
    void exportImagesDialog();
    void exportTextDialog();
    /// File > Export > Pages as Images, without the dialogs: `pages` (a range
    /// spec, "" all) at `dpi`, each to `pattern` with %1 for the page number
    /// (or to `pattern` itself for one page). Returns the files written.
    QStringList exportImages(const QString& pages, int dpi, const QString& pattern);
    /// File > Export > Text, without the dialogs. Asynchronous: the file is
    /// written when the worker's text arrives.
    void exportText(const QString& pages, const QString& path);

    // Pages mode: turn, delete, move, insert (ops/organize.hpp).
    /// The pages a page command acts on: the grid's selection in the page
    /// grid, else the page in view. 0-based, ascending.
    [[nodiscard]] QVector<int> targetPages() const;
    /// Shows the page grid (true) or the reading view in the document area.
    void showPageGrid(bool grid);
    void rotatePages(int degrees);
    void deletePages();
    void insertFileDialog();
    void insertBlankPage();
    /// Inserts every page of each PDF in `paths`, in order, before page `at`.
    void insertFilesAt(const QStringList& paths, int at);
    /// True unless the user calls off a page change to a signed document.
    /// Unlike a redaction it is appended, so the signatures stay intact, but
    /// each will say the document changed after it.
    [[nodiscard]] bool confirmChangingSigned(const QString& what);

    /// The colour a comment tool draws in: the user's choice, else its own.
    [[nodiscard]] static QColor toolColor(const QString& toolId);

signals:
    /// For the window's status bar, while this tab is the current one. An
    /// empty text clears it.
    void statusMessage(const QString& text, int timeout);
    /// The file name or the modified mark changed.
    void titleChanged();
    /// Undo/redo, page count, marks, fields or signatures changed: what the
    /// window's actions allow may have too.
    void stateChanged();
    /// The certification level changed (see certLevel()).
    void certificationChanged();
    /// A document starts opening here: its mode and tool are back to Read
    /// and Select.
    void opening();
    /// The document's pages show.
    void opened();
    /// Nothing is open after all (it failed, or the password was not given):
    /// the window closes the tab.
    void closed();
    void pageChanged(int page);
    void zoomChanged();
    void matchNavigated(int index, int total);
    /// The view put its tool down (the reason went to statusMessage).
    void toolRefused();
    /// The tab asks the window's mode bar for `mode` (a page opened from
    /// the grid goes back to Read).
    void modeRequested(const QString& mode);
    /// The Sign dialog's "Place a box" choice: Fill & Sign mode, Sign tool.
    void placeSignatureRequested();
    /// The form panel's Flatten button.
    void flattenRequested();
    /// A signature card's "trust this signer" link: Preferences > Trust.
    void trustSettingsRequested();

    // To the worker (queued across the thread boundary).
    void requestOpen(const QString& path);
    void requestAuthenticate(const QString& password);
    void requestSearch(const QString& needle);

private:
    void wireWorker();
    void buildPanels();
    void buildSignaturePanel();
    void onOpened(int pageCount, QVector<QSize> baseSizes);
    void onFailed(const QString& message);
    void onPasswordRequired(bool retry);
    void onOutlineReady(const QVector<OutlineRow>& rows);
    void onOutlineClicked(QTreeWidgetItem* item, int column);
    void onEditStateChanged(bool canUndo, bool canRedo, bool modified);
    void onFieldsReady(const QVector<FieldRow>& rows);
    void onSignaturesReady(const QVector<SigRow>& rows);
    void onSaved(const QString& path);
    void applyCertification(int level);
    void setRedactionMarks(QVector<QPair<int, QRectF>> marks);
    /// Before a save: false to stop, after asking about marks not yet applied.
    bool settlePendingRedactions();
    /// Save As without the questions save() and saveAs() have already asked.
    bool saveToChosenPath();
    /// True unless the user calls off an edit that would break signatures.
    /// A redaction cannot be appended: it rewrites the file, and every
    /// signature in it goes with the revisions it drops.
    [[nodiscard]] bool confirmBreakingSignatures(const QString& what);
    void say(const QString& text, int timeout = 0) { emit statusMessage(text, timeout); }

    PageView* view_ = nullptr;
    QThread workerThread_;
    RenderWorker* worker_ = nullptr;

    Sidebar* sidebar_ = nullptr;
    ThumbnailBar* thumbnails_ = nullptr;
    QTreeWidget* outlineTree_ = nullptr;
    CommentsPanel* comments_ = nullptr;
    FormPanel* form_ = nullptr;
    QWidget* signaturePanel_ = nullptr;
    SignatureCards* signatureCards_ = nullptr;
    QTreeWidget* signatures_ = nullptr;
    QToolBar* signatureBanner_ = nullptr;
    QLabel* signatureBannerLabel_ = nullptr;
    QLabel* signatureBannerIcon_ = nullptr;
    PageGrid* pageGrid_ = nullptr;
    QStackedWidget* viewStack_ = nullptr;  ///< the page view, or the page grid
    QSplitter* splitter_ = nullptr;

    QString currentPath_;
    QString currentTitle_;
    bool modified_ = false;
    bool canUndo_ = false;
    bool canRedo_ = false;
    int pageCount_ = 0;
    int certLevel_ = 0;
    int signatureCount_ = 0;
    int startPage_ = -1;
    QVector<FieldRow> fieldRows_;  ///< the latest field list, for the Sign dialog's Where step
    QStringList signedFields_;     ///< the fields that hold a signature already
    QVector<QPair<int, QRectF>> redactionMarks_;
    bool rearranged_ = false;  ///< the next documentEdited follows a page rearrangement
    QVector<int> gridSelectionAfterEdit_;  ///< what the grid selects once the edit lands
    bool formShown_ = false;  ///< the Form tab was opened for this document already
    bool documentEncrypted_ = false;  ///< it asked for a password when opened
    std::function<void()> afterSave_;  ///< what an unsaved-changes prompt was waiting for
    QString pendingTask_;   ///< a welcome task waiting for its document to open
    bool verifyPending_ = false;
    int sidebarWidth_ = 0;  ///< its width before it last folded
    bool sidebarBeforeGrid_ = true;  ///< whether the sidebar was open before the grid showed
    QString mode_ = QStringLiteral("read");
    QString tool_ = QStringLiteral("toolSelect");
};
