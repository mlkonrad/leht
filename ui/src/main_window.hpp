// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QThread>
#include <QVector>

#include <functional>

#include "edit_model.hpp"
#include "outline_model.hpp"

class PageView;
class QAction;
class QActionGroup;
class QCloseEvent;
class QTableWidget;
class RenderWorker;
class QLabel;
class QLineEdit;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class QSpinBox;
class ThumbnailBar;

/// The application window. Owns the render thread, wires it to the view, and
/// provides open / zoom / fit actions.
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

private:
    void buildActions();
    void buildEditActions();
    void buildSignaturePanel();
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
    int signatureCount_ = 0;
    bool populatingFields_ = false;
    std::function<void()> afterSave_;  ///< what an unsaved-changes prompt was waiting for
};
