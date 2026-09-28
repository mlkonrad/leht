// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QThread>
#include <QVector>

#include <functional>

#include "edit_model.hpp"
#include "outline_model.hpp"

class ActionRegistry;
class CommentsPanel;
class ModeBar;
class PageGrid;
class PageView;
class QAction;
class QMenu;
class QStackedWidget;
class SignatureCards;
class Sidebar;
class WelcomeView;
class QActionGroup;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QTableWidget;
class RenderWorker;
class QLabel;
class QLineEdit;
class QToolBar;
class QToolButton;
class QProgressDialog;
class FileTools;
class QTreeWidget;
class QTreeWidgetItem;
class QSpinBox;
class ThumbnailBar;

/// The application window. Owns the render thread and wires it to the view.
///
/// Every command is registered once in an ActionRegistry (actions.hpp), and
/// the menu bar, the toolbar and the mode bar are built from it. With no
/// document open the window shows the WelcomeView; with one, a mode bar over
/// the sidebar (pages, outline, comments, form, signatures) and the page.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    void openPath(const QString& path);

    // Accessors for the headless smoke test; the worker lives on another thread
    // and has no parent, so findChild cannot reach it.
    [[nodiscard]] PageView* view() const { return view_; }
    [[nodiscard]] RenderWorker* worker() const { return worker_; }
    [[nodiscard]] FileTools* fileTools() const { return fileTools_; }
    [[nodiscard]] ActionRegistry* actions() const { return actions_; }
    [[nodiscard]] Sidebar* sidebar() const { return sidebar_; }
    [[nodiscard]] ModeBar* modeBar() const { return modes_; }
    /// Whether the welcome view is showing (no document open).
    [[nodiscard]] bool isShowingWelcome() const;

    /// Opens one dropped file, or offers to combine several.
    void handleDroppedFiles(const QStringList& paths);
    /// A welcome-view task: "sign", "fill", "combine", "ocr", "reduce", "verify".
    void startTask(const QString& task);
    /// File > Close: back to the welcome view (asks about unsaved edits).
    void closeDocument();
    /// Areas marked with the Redact tool and not yet applied.
    [[nodiscard]] int pendingRedactions() const { return static_cast<int>(redactionMarks_.size()); }
    /// Tools > Apply Redactions: removes everything under the marks, as one
    /// edit group. Asks first when `confirm`. False if nothing was marked or
    /// the user called it off.
    bool applyRedactions(bool confirm = true);

private slots:
    void openDialog();
    void onOpened(int pageCount, QVector<QSize> baseSizes);
    void onFailed(const QString& message);
    void onCurrentPageChanged(int page);
    void updateZoomLabel();
    void showFindBar();
    void hideFindBar();
    void runSearch();
    void onMatchNavigated(int index, int total);
    void onOutlineReady(const QVector<OutlineRow>& rows);
    void onOutlineClicked(QTreeWidgetItem* item, int column);
    void goToPageFromSpin();
    void onPasswordRequired(bool retry);
    void printDialog();
    void onEditStateChanged(bool canUndo, bool canRedo, bool modified);
    void onFieldsReady(const QVector<FieldRow>& rows);
    void onSignaturesReady(const QVector<SigRow>& rows);
    void onSaved(const QString& path);

public:
    /// Saves to the file it came from (or asks, if it has none). Returns
    /// false if nothing was started.
    bool save();
    bool saveAs();
    /// Whether there are edits not yet saved.
    [[nodiscard]] bool isModified() const { return modified_; }
    /// Prints pages [fromPage, toPage] (1-based; 0,0 = all) to `printer`.
    /// Public so the headless test can print to a PDF. Returns false if nothing
    /// was printed.
    bool printDocument(class QPrinter& printer, int fromPage = 0, int toPage = 0);

signals:
    void requestOpen(const QString& path);
    void requestAuthenticate(const QString& password);
    void requestSearch(const QString& needle);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

public slots:
    /// More -> Recognize Text (OCR)...
    void recognizeText();
    /// Disables what a certification at `level` forbids (0: nothing).
    void applyCertification(int level);
    /// More -> Add Long-Term Validation...
    void addLongTermValidation();
    /// Sign -> Update EU Trusted Lists...
    void updateTrustedList();

private:
    void buildActions();
    void buildEditActions();
    void buildMainToolbar();
    /// Pages mode: turn, delete, move, insert, extract (ops/organize.hpp).
    void buildPageActions();
    /// The pages a page command acts on: the grid's selection in the page
    /// grid, else the page in view. 0-based, ascending.
    [[nodiscard]] QVector<int> targetPages() const;
    /// Shows the page grid (true) or the reading view in the document area.
    void showPageGrid(bool grid);
    /// Inserts every page of each PDF in `paths`, in order, before page `at`.
    void insertFilesAt(const QStringList& paths, int at);
    /// True unless the user calls off a page change to a signed document.
    /// Unlike a redaction it is appended, so the signatures stay intact, but
    /// each will say the document changed after it.
    [[nodiscard]] bool confirmChangingSigned(const QString& what);
    void buildSignaturePanel();
    void buildLayout();
    void buildMenus();
    /// Toolbar text and icons, from Preferences.
    void applyAppearance();
    void openPreferences(int page = 0);
    void showAbout();
    void showWelcome();
    /// Asks for a file to open; empty if cancelled.
    [[nodiscard]] QString askOpenPath();
    /// Combine Files, starting with `initial` in the list.
    void combineFiles(const QStringList& initial);
    /// Runs what a welcome task asked for, once its document is open.
    void runPendingTask();
    /// Whether the current certification level allows what needs `needs`
    /// (see applyCertification).
    [[nodiscard]] bool certAllows(int needs) const;
    /// Opens the Sign dialog for a box on `page` (an empty box signs
    /// invisibly), then signs -- which saves, so it asks where to when the
    /// document has no path yet.
    void startSigning(int page, QRectF rect);
    /// True unless the user calls off an edit that would break signatures.
    /// A redaction cannot be appended: it rewrites the file, and every
    /// signature in it goes with the revisions it drops.
    [[nodiscard]] bool confirmBreakingSignatures(const QString& what);
    void updateTitle();
    /// Asks what to do with unsaved edits. True means carry on now; false
    /// means stop (cancelled, or a save was started and `then` will run when
    /// it succeeds).
    bool resolveUnsaved(std::function<void()> then);
    /// Runs `fn` on the worker thread, in order with every other request.
    void onWorker(std::function<void(RenderWorker*)> fn);

    PageView* view_ = nullptr;
    QThread workerThread_;
    RenderWorker* worker_ = nullptr;

    QLabel* pageLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QToolBar* findBar_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QLabel* findLabel_ = nullptr;
    QTreeWidget* outlineTree_ = nullptr;
    ThumbnailBar* thumbnails_ = nullptr;
    QSpinBox* pageSpin_ = nullptr;
    bool syncingSpin_ = false;
    int pageCount_ = 0;
    QString currentTitle_;

    // Editing.
    QString currentPath_;
    bool modified_ = false;
    QAction* saveAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QActionGroup* tools_ = nullptr;
    QTableWidget* fields_ = nullptr;
    QTreeWidget* signatures_ = nullptr;
    QToolBar* signatureBanner_ = nullptr;
    QLabel* signatureBannerLabel_ = nullptr;
    QLabel* signatureBannerIcon_ = nullptr;
    int signatureCount_ = 0;
    bool populatingFields_ = false;
    std::function<void()> afterSave_;  ///< what an unsaved-changes prompt was waiting for

    // Structure (see the class comment).
    ActionRegistry* actions_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    WelcomeView* welcome_ = nullptr;
    QWidget* documentPage_ = nullptr;
    ModeBar* modes_ = nullptr;
    QHash<QString, QList<QAction*>> modeTools_;
    Sidebar* sidebar_ = nullptr;
    CommentsPanel* comments_ = nullptr;
    QStackedWidget* viewStack_ = nullptr;  ///< the page view, or the page grid
    PageGrid* pageGrid_ = nullptr;
    QVector<QPair<int, QRectF>> redactionMarks_;
    void setRedactionMarks(QVector<QPair<int, QRectF>> marks);
    /// Before a save: false to stop, after asking about marks not yet applied.
    bool settlePendingRedactions();
    /// Save As without the questions save() and saveAs() have already asked.
    bool saveToChosenPath();
    bool rearranged_ = false;
    bool sidebarBeforeGrid_ = true;  ///< whether the sidebar was open before the grid showed  ///< the next documentEdited follows a page rearrangement
    QVector<int> gridSelectionAfterEdit_;  ///< what the grid selects once the edit lands
    QWidget* signaturePanel_ = nullptr;
    SignatureCards* signatureCards_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    int certLevel_ = 0;
    QString pendingTask_;   ///< a welcome task waiting for its document to open
    bool verifyPending_ = false;
    bool formShown_ = false;  ///< the Form tab was opened for this document already

    // File tools: Combine Files, Reduce File Size, Split Document. Jobs run on
    // their own thread, each in a worker of its own (see file_tools.hpp).
    void buildFileTools();
    /// Starts `job` on the file-tools thread with an empty password, showing
    /// `title` in a progress dialog. If the file turns out to be encrypted,
    /// asks for the password and runs `job` again with it.
    void runFileTool(const QString& title, std::function<void(FileTools*, QString)> job);
    void endFileTool();
    QThread fileToolsThread_;
    FileTools* fileTools_ = nullptr;
    bool fileToolBusy_ = false;  ///< one job at a time
    bool documentEncrypted_ = false;  ///< it asked for a password when opened
    QProgressDialog* fileToolsProgress_ = nullptr;
    QString fileToolTitle_;
    std::function<void(FileTools*, QString)> fileToolJob_;
};
