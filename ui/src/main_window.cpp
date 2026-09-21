// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"

#include "page_view.hpp"
#include "render_worker.hpp"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
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

    // Find: GUI -> worker search, worker -> view highlights.
    connect(this, &MainWindow::requestSearch, worker_, &RenderWorker::search);
    connect(worker_, &RenderWorker::pageMatches, view_, &PageView::addMatches);
    connect(worker_, &RenderWorker::searchFinished, view_,
            &PageView::finishMatches);
    connect(view_, &PageView::matchNavigated, this,
            &MainWindow::onMatchNavigated);

    // Selection: view -> worker request, worker -> view highlight + text.
    connect(view_, &PageView::selectRequested, worker_,
            &RenderWorker::selectRegion);
    connect(worker_, &RenderWorker::selectionReady, view_,
            &PageView::setSelection);

    connect(view_, &PageView::currentPageChanged, this,
            &MainWindow::onCurrentPageChanged);

    workerThread_.start();

    buildActions();

    pageLabel_ = new QLabel(this);
    zoomLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(pageLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);
    updateZoomLabel();

    // Find bar: a hidden toolbar with a query field, match counter, and
    // next/prev. Shown by Ctrl+F, dismissed by Escape.
    findBar_ = new QToolBar(tr("Find"), this);
    findBar_->setMovable(false);
    findEdit_ = new QLineEdit(findBar_);
    findEdit_->setPlaceholderText(tr("Find in document"));
    findEdit_->setClearButtonEnabled(true);
    findEdit_->setMaximumWidth(280);
    findBar_->addWidget(findEdit_);
    QAction* prev = findBar_->addAction(tr("Previous"));
    prev->setShortcut(QKeySequence::FindPrevious);
    QAction* next = findBar_->addAction(tr("Next"));
    next->setShortcut(QKeySequence::FindNext);
    findLabel_ = new QLabel(findBar_);
    findLabel_->setMinimumWidth(90);
    findBar_->addWidget(findLabel_);
    addToolBar(Qt::BottomToolBarArea, findBar_);
    findBar_->hide();

    connect(findEdit_, &QLineEdit::returnPressed, this, &MainWindow::runSearch);
    connect(next, &QAction::triggered, view_, &PageView::nextMatch);
    connect(prev, &QAction::triggered, view_, &PageView::prevMatch);

    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &MainWindow::hideFindBar);
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

    bar->addSeparator();
    QAction* find = bar->addAction(tr("Find"));
    find->setShortcut(QKeySequence::Find);
    connect(find, &QAction::triggered, this, &MainWindow::showFindBar);

    QAction* copy = new QAction(tr("Copy"), this);
    copy->setShortcut(QKeySequence::Copy);
    connect(copy, &QAction::triggered, this,
            [this] { view_->copySelection(); });
    addAction(copy);

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

void MainWindow::showFindBar() {
    findBar_->show();
    findEdit_->setFocus();
    findEdit_->selectAll();
}

void MainWindow::hideFindBar() {
    findBar_->hide();
    view_->clearMatches();
    view_->setFocus();
}

void MainWindow::runSearch() {
    const QString needle = findEdit_->text();
    view_->clearMatches();
    findLabel_->setText(needle.isEmpty() ? QString() : tr("searching…"));
    emit requestSearch(needle);
}

void MainWindow::onMatchNavigated(int index, int total) {
    if (total <= 0) {
        findLabel_->setText(tr("no matches"));
    } else {
        findLabel_->setText(tr("%1 of %2").arg(index + 1).arg(total));
    }
}
