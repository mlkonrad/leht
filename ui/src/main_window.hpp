// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QThread>
#include <QVector>

#include "outline_model.hpp"

class PageView;
class RenderWorker;
class QLabel;
class QLineEdit;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class QSpinBox;

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

signals:
    void requestOpen(const QString& path);
    void requestSearch(const QString& needle);

private:
    void buildActions();

    PageView* view_ = nullptr;
    QThread workerThread_;
    RenderWorker* worker_ = nullptr;

    QLabel* pageLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QToolBar* findBar_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QLabel* findLabel_ = nullptr;
    QTreeWidget* outlineTree_ = nullptr;
    QSpinBox* pageSpin_ = nullptr;
    bool syncingSpin_ = false;
    int pageCount_ = 0;
    QString currentTitle_;
};
