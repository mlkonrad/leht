// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QThread>

class PageView;
class RenderWorker;
class QLabel;

/// The application window. Owns the render thread, wires it to the view, and
/// provides open / zoom / fit actions.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    void openPath(const QString& path);

private slots:
    void openDialog();
    void onOpened(int pageCount, QVector<QSize> baseSizes);
    void onFailed(const QString& message);
    void onCurrentPageChanged(int page);
    void updateZoomLabel();

signals:
    void requestOpen(const QString& path);

private:
    void buildActions();

    PageView* view_ = nullptr;
    QThread workerThread_;
    RenderWorker* worker_ = nullptr;

    QLabel* pageLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    int pageCount_ = 0;
    QString currentTitle_;
};
