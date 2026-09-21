// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"

#include "page_view.hpp"
#include "render_worker.hpp"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

MainWindow::MainWindow() {
    setWindowTitle(tr("Leht"));
    resize(1000, 800);

    view_ = new PageView(this);
    setCentralWidget(view_);

    // The worker lives on its own thread; everything MuPDF happens there.
    worker_ = new RenderWorker();
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);

    // GUI -> worker (queued across the thread boundary).
    connect(this, &MainWindow::requestOpen, worker_, &RenderWorker::open);
    connect(view_, &PageView::needRender, worker_, &RenderWorker::render);
    connect(view_, &PageView::generationChanged, worker_,
            [this](quint64 gen) { worker_->setGeneration(gen); });

    // worker -> GUI.
    connect(worker_, &RenderWorker::opened, this, &MainWindow::onOpened);
    connect(worker_, &RenderWorker::failed, this, &MainWindow::onFailed);
    connect(worker_, &RenderWorker::rendered, view_, &PageView::onRendered);

    connect(view_, &PageView::currentPageChanged, this,
            &MainWindow::onCurrentPageChanged);

    workerThread_.start();

    buildActions();

    pageLabel_ = new QLabel(this);
    zoomLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(pageLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);
    updateZoomLabel();
}

MainWindow::~MainWindow() {
    workerThread_.quit();
    workerThread_.wait();
}

void MainWindow::buildActions() {
    QToolBar* bar = addToolBar(tr("Main"));
    bar->setMovable(false);

    QAction* open = bar->addAction(tr("Open"));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::openDialog);

    bar->addSeparator();

    QAction* zoomIn = bar->addAction(tr("Zoom In"));
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(zoomIn, &QAction::triggered, this, [this] {
        view_->zoomBy(1.25);
        updateZoomLabel();
    });

    QAction* zoomOut = bar->addAction(tr("Zoom Out"));
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, this, [this] {
        view_->zoomBy(0.8);
        updateZoomLabel();
    });

    QAction* fit = bar->addAction(tr("Fit Width"));
    fit->setShortcut(Qt::CTRL | Qt::Key_0);
    connect(fit, &QAction::triggered, this, [this] {
        view_->fitWidth();
        updateZoomLabel();
    });

    QAction* quit = new QAction(tr("Quit"), this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);
    addAction(quit);
}

void MainWindow::openDialog() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open PDF"), QString(),
        tr("PDF documents (*.pdf);;All files (*)"));
    if (!path.isEmpty()) {
        openPath(path);
    }
}

void MainWindow::openPath(const QString& path) {
    currentTitle_ = QFileInfo(path).fileName();
    statusBar()->showMessage(tr("Opening %1…").arg(currentTitle_));
    view_->clear();
    emit requestOpen(path);
}

void MainWindow::onOpened(int pageCount, QVector<QSize> baseSizes) {
    pageCount_ = pageCount;
    setWindowTitle(tr("%1 — Leht").arg(currentTitle_));
    statusBar()->clearMessage();
    view_->setPages(baseSizes);
    view_->fitWidth();
    onCurrentPageChanged(view_->currentPage());
    updateZoomLabel();
}

void MainWindow::onFailed(const QString& message) {
    statusBar()->clearMessage();
    QMessageBox::warning(this, tr("Could not open document"), message);
}

void MainWindow::onCurrentPageChanged(int page) {
    if (pageCount_ > 0 && page >= 0) {
        pageLabel_->setText(tr("Page %1 / %2").arg(page + 1).arg(pageCount_));
    } else {
        pageLabel_->clear();
    }
}

void MainWindow::updateZoomLabel() {
    zoomLabel_->setText(tr("%1%").arg(qRound(view_->zoom() * 100.0)));
}
