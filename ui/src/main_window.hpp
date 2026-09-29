// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QVector>

#include <functional>

class ActionRegistry;
class ColorSwatches;
class DocumentTab;
class FileTools;
class ModeBar;
class PageView;
class QAction;
class QActionGroup;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QPrinter;
class QProgressDialog;
class QSpinBox;
class QStackedWidget;
class QToolBar;
class RenderWorker;
class Sidebar;
class WelcomeView;

/// The application window: the frame around the open documents.
///
/// Every command is registered once in an ActionRegistry (actions.hpp), and
/// the menu bar, the toolbar and the mode bar are built from it. With no
/// document open the window shows the WelcomeView; with one, the mode bar
/// over the document (a DocumentTab: its sidebar, pages and render worker).
/// Every action works on the current document, through current().
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    /// Opens `path` in the current document's place, replacing what it shows
    /// (a new one if nothing is open).
    void openPath(const QString& path);
    /// Opens `path` as the user asked: the window already showing it comes
    /// forward; an empty window opens it; otherwise a new window does.
    void openDocument(const QString& path);
    /// File > New Window: another window, empty, deleted when closed.
    MainWindow* newWindow();

    /// The document the window shows; nullptr on the welcome view.
    [[nodiscard]] DocumentTab* current() const;
    /// Every document open in this window, in order.
    [[nodiscard]] QList<DocumentTab*> documents() const { return tabs_; }

    // Accessors for the headless smoke test. view(), worker() and sidebar()
    // are the current document's, or nullptr.
    [[nodiscard]] PageView* view() const;
    [[nodiscard]] RenderWorker* worker() const;
    [[nodiscard]] Sidebar* sidebar() const;
    [[nodiscard]] FileTools* fileTools() const { return fileTools_; }
    [[nodiscard]] ActionRegistry* actions() const { return actions_; }
    [[nodiscard]] ModeBar* modeBar() const { return modes_; }
    /// Whether the welcome view is showing (no document open).
    [[nodiscard]] bool isShowingWelcome() const;

    /// Opens one dropped file, or offers to combine several.
    void handleDroppedFiles(const QStringList& paths);
    /// A welcome-view task: "sign", "fill", "combine", "ocr", "reduce", "verify".
    void startTask(const QString& task);
    /// The start-screen tour, the first time Leht runs (main() calls it once
    /// the window shows). The document tour follows with the first document.
    void showFirstRunHints();
    /// The colour a comment tool draws in: the user's choice, else its own.
    [[nodiscard]] QColor toolColor(const QString& toolId) const;
    /// File > Close: closes the current document (asks about unsaved edits).
    void closeDocument();

    // The current document's, for the smoke test and the actions.
    [[nodiscard]] int pendingRedactions() const;
    bool applyRedactions(bool confirm = true);
    QStringList exportImages(const QString& pages, int dpi, const QString& pattern);
    void exportText(const QString& pages, const QString& path);
    bool save();
    bool saveAs();
    [[nodiscard]] bool isModified() const;
    bool printDocument(QPrinter& printer, int fromPage = 0, int toPage = 0);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

public slots:
    /// Sign -> Update EU Trusted Lists...
    void updateTrustedList();

private:
    void buildActions();
    void buildEditActions();
    void buildMainToolbar();
    /// Pages mode: turn, delete, move, insert, extract (ops/organize.hpp).
    void buildPageActions();
    void buildLayout();
    void buildMenus();
    /// Toolbar text and icons, from Preferences.
    void applyAppearance();
    void openPreferences(int page = 0);
    void showAbout();
    void showWelcome();
    /// Asks for a file to open; empty if cancelled.
    [[nodiscard]] QString askOpenPath();
    void openDialog();
    /// Combine Files, starting with `initial` in the list.
    void combineFiles(const QStringList& initial);
    /// Runs what a welcome task asked for, once `tab`'s document is open.
    void runPendingTask(DocumentTab* tab);

    // Documents.
    /// A new, empty document in this window, made current.
    DocumentTab* addDocument();
    /// Takes `tab` into this window: its signals, the context menus that
    /// name this window's actions. Makes it current.
    void adopt(DocumentTab* tab);
    /// Lets go of `tab` without deleting it: its signals no longer reach
    /// this window.
    void release(DocumentTab* tab);
    /// Closes `tab` (asks about unsaved edits when `ask`).
    void closeTab(DocumentTab* tab, bool ask = true);
    /// Makes `tab` the one shown, and the window's chrome follow it.
    void setCurrent(DocumentTab* tab);
    /// The mode bar, tools, certification, page and zoom controls, title and
    /// actions, all as the current document has them.
    void loadChrome();
    /// Disables what the current document's certification forbids.
    void applyCertification();
    void updateTitle();
    void updatePageControls();
    void updateZoomLabel();
    void showFindBar();
    void hideFindBar();
    void runSearch();
    void goToPageFromSpin();
    /// Moves focus to the next (+1) or previous (-1) part of the window:
    /// toolbar, mode bar, sidebar, page.
    void focusRegion(int step);

    QList<DocumentTab*> tabs_;
    DocumentTab* current_ = nullptr;
    /// What each document is connected to in this window, cut on release().
    QHash<DocumentTab*, QList<QMetaObject::Connection>> links_;
    bool loadingChrome_ = false;  ///< the mode bar follows a document, not the user
    /// Closing the window: the documents whose edits the user chose to lose.
    /// Kept while a save the close waits for runs (resumingClose_).
    QSet<DocumentTab*> discarded_;
    bool resumingClose_ = false;
    /// The colour swatches and the new-text colour, as the checked tool has them.
    void syncSwatches();

    QLabel* pageLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QToolBar* findBar_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QLabel* findLabel_ = nullptr;
    QSpinBox* pageSpin_ = nullptr;
    bool syncingSpin_ = false;

    QActionGroup* tools_ = nullptr;

    // Structure (see the class comment).
    ActionRegistry* actions_ = nullptr;
    QStackedWidget* stack_ = nullptr;  ///< the welcome view, or the documents
    WelcomeView* welcome_ = nullptr;
    QWidget* documentPage_ = nullptr;
    QStackedWidget* documentStack_ = nullptr;  ///< one DocumentTab per document
    ModeBar* modes_ = nullptr;
    QHash<QString, QList<QAction*>> modeTools_;
    ColorSwatches* swatches_ = nullptr;
    QMenu* recentMenu_ = nullptr;

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
    QProgressDialog* fileToolsProgress_ = nullptr;
    QString fileToolTitle_;
    QString fileToolDocument_;  ///< the file name a job's password is asked for
    std::function<void(FileTools*, QString)> fileToolJob_;
};
