// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"

#include "page_view.hpp"
#include "render_worker.hpp"

#include <algorithm>

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QDockWidget>
#include <QHeaderView>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>

#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QTreeWidget>

#include "outline_model.hpp"
#include "thumbnail_bar.hpp"

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
    // Context `this`, not worker_: setGeneration must run on the GUI thread the
    // moment the view moves. Queued onto the worker thread it would wait behind
    // the very renders it is meant to make stale.
    connect(view_, &PageView::generationChanged, this,
            [this](quint64 gen) { worker_->setGeneration(gen); });

    // worker -> GUI.
    connect(worker_, &RenderWorker::opened, this, &MainWindow::onOpened);
    connect(worker_, &RenderWorker::outlineReady, this,
            &MainWindow::onOutlineReady);
    connect(worker_, &RenderWorker::passwordRequired, this,
            &MainWindow::onPasswordRequired);
    connect(this, &MainWindow::requestAuthenticate, worker_,
            &RenderWorker::authenticate);
    connect(worker_, &RenderWorker::failed, this, &MainWindow::onFailed);
    connect(worker_, &RenderWorker::rendered, view_, &PageView::onRendered);

    // Find: GUI -> worker search, worker -> view highlights.
    connect(this, &MainWindow::requestSearch, worker_, &RenderWorker::search);
    connect(worker_, &RenderWorker::searchStarted, view_, &PageView::clearMatches);
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

    // Thumbnails: bar -> worker request, worker -> bar image, bar -> navigation.
    connect(worker_, &RenderWorker::thumbnailReady, this,
            [this](int page, const QImage& img) {
                if (thumbnails_ != nullptr) {
                    thumbnails_->onThumbnail(page, img);
                }
            });

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

    // Outline sidebar: a dockable tree, hidden until a document with an outline
    // is opened.
    auto* dock = new QDockWidget(tr("Outline"), this);
    dock->setObjectName(QStringLiteral("outlineDock"));
    outlineTree_ = new QTreeWidget(dock);
    outlineTree_->setHeaderHidden(true);
    outlineTree_->setColumnCount(1);
    dock->setWidget(outlineTree_);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    dock->hide();
    connect(outlineTree_, &QTreeWidget::itemClicked, this,
            &MainWindow::onOutlineClicked);

    // Thumbnail sidebar, tabbed with the outline on the left.
    auto* thumbDock = new QDockWidget(tr("Thumbnails"), this);
    thumbDock->setObjectName(QStringLiteral("thumbnailDock"));
    thumbnails_ = new ThumbnailBar(thumbDock);
    thumbDock->setWidget(thumbnails_);
    addDockWidget(Qt::LeftDockWidgetArea, thumbDock);
    tabifyDockWidget(dock, thumbDock);
    thumbDock->raise();  // thumbnails visible by default
    connect(thumbnails_, &ThumbnailBar::needThumbnail, worker_,
            &RenderWorker::renderThumbnail);
    connect(thumbnails_, &ThumbnailBar::pageChosen, this,
            [this](int page) { view_->goToPage(page); });

    // Go-to-page: a spin box in the status bar, kept in sync with the view.
    pageSpin_ = new QSpinBox(this);
    pageSpin_->setMinimum(1);
    pageSpin_->setMaximum(1);
    pageSpin_->setEnabled(false);
    pageSpin_->setKeyboardTracking(false);
    pageSpin_->setPrefix(tr("Page "));
    statusBar()->addPermanentWidget(pageSpin_);
    connect(pageSpin_, &QSpinBox::editingFinished, this,
            &MainWindow::goToPageFromSpin);
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

    QAction* print = bar->addAction(tr("Print"));
    print->setShortcut(QKeySequence::Print);
    connect(print, &QAction::triggered, this, &MainWindow::printDialog);

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

    QAction* fitPage = bar->addAction(tr("Fit Page"));
    fitPage->setShortcut(Qt::CTRL | Qt::Key_9);
    connect(fitPage, &QAction::triggered, this, [this] {
        view_->fitPage();
        updateZoomLabel();
    });

    QAction* rotate = bar->addAction(tr("Rotate"));
    rotate->setShortcut(Qt::CTRL | Qt::Key_R);
    connect(rotate, &QAction::triggered, this, [this] {
        view_->rotateBy(90);
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
    if (thumbnails_ != nullptr) {
        thumbnails_->clearThumbnails();
    }
    emit requestOpen(path);
}

void MainWindow::onOpened(int pageCount, QVector<QSize> baseSizes) {
    pageCount_ = pageCount;
    setWindowTitle(tr("%1 — Leht").arg(currentTitle_));
    statusBar()->clearMessage();
    view_->setPages(baseSizes);
    view_->fitWidth();
    pageSpin_->setMaximum(qMax(1, pageCount));
    pageSpin_->setEnabled(pageCount > 0);
    thumbnails_->setPageCount(pageCount);
    view_->setFocus();
    onCurrentPageChanged(view_->currentPage());
    updateZoomLabel();
}

void MainWindow::onFailed(const QString& message) {
    statusBar()->clearMessage();
    QMessageBox::warning(this, tr("Could not open document"), message);
}

void MainWindow::onCurrentPageChanged(int page) {
    if (pageCount_ > 0 && page >= 0) {
        pageLabel_->setText(tr("/ %1").arg(pageCount_));
        syncingSpin_ = true;
        pageSpin_->setValue(page + 1);
        syncingSpin_ = false;
        if (thumbnails_ != nullptr) {
            thumbnails_->setCurrentPageQuiet(page);
        }
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
    worker_->cancelSearch();
    findBar_->hide();
    view_->clearMatches();
    view_->setFocus();
}

void MainWindow::runSearch() {
    const QString needle = findEdit_->text();
    view_->clearMatches();
    findLabel_->setText(needle.isEmpty() ? QString() : tr("searching…"));
    worker_->cancelSearch();  // a new search never waits behind an old one
    emit requestSearch(needle);
}

void MainWindow::onMatchNavigated(int index, int total) {
    if (total <= 0) {
        findLabel_->setText(tr("no matches"));
    } else {
        findLabel_->setText(tr("%1 of %2").arg(index + 1).arg(total));
    }
}

void MainWindow::onOutlineReady(const QVector<OutlineRow>& rows) {
    outlineTree_->clear();
    auto* dock = findChild<QDockWidget*>(QStringLiteral("outlineDock"));

    if (rows.isEmpty()) {
        if (dock != nullptr) {
            dock->hide();
        }
        return;
    }

    // Rebuild the tree from the flat, depth-tagged rows. A running stack maps
    // each depth to its last item, so a child attaches under the right parent.
    QVector<QTreeWidgetItem*> stack;
    for (const OutlineRow& row : rows) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, row.title);
        item->setData(0, Qt::UserRole, row.page);
        item->setData(0, Qt::UserRole + 1, row.y);

        stack.resize(row.depth);
        if (row.depth == 0) {
            outlineTree_->addTopLevelItem(item);
        } else if (!stack.isEmpty() && stack.last() != nullptr) {
            stack.last()->addChild(item);
        } else {
            outlineTree_->addTopLevelItem(item);  // malformed depth; do not lose it
        }
        stack.push_back(item);
    }
    outlineTree_->expandToDepth(1);
    if (dock != nullptr) {
        dock->show();
    }
}

void MainWindow::onOutlineClicked(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }
    const int page = item->data(0, Qt::UserRole).toInt();
    const double y = item->data(0, Qt::UserRole + 1).toDouble();
    if (page >= 0) {
        view_->goToPage(page, y);
    }
}

void MainWindow::goToPageFromSpin() {
    if (syncingSpin_) {
        return;  // the change came from scrolling, not the user
    }
    view_->goToPage(pageSpin_->value() - 1);
}

void MainWindow::onPasswordRequired(bool retry) {
    statusBar()->clearMessage();
    bool ok = false;
    const QString prompt =
        retry ? tr("Wrong password. Try again for “%1”:").arg(currentTitle_)
              : tr("“%1” is password-protected. Enter its password:")
                    .arg(currentTitle_);
    const QString password = QInputDialog::getText(
        this, tr("Password required"), prompt, QLineEdit::Password, QString(), &ok);

    if (!ok) {
        // User cancelled: leave the viewer as it was.
        statusBar()->showMessage(tr("Opening cancelled."), 3000);
        return;
    }
    statusBar()->showMessage(tr("Unlocking…"));
    emit requestAuthenticate(password);
}

void MainWindow::printDialog() {
    if (pageCount_ <= 0) {
        return;
    }
    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(currentTitle_);
    printer.setFromTo(1, pageCount_);

    QPrintDialog dialog(&printer, this);
    dialog.setOption(QAbstractPrintDialog::PrintPageRange, true);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    statusBar()->showMessage(tr("Printing…"));
    printDocument(printer, printer.fromPage(), printer.toPage());
    statusBar()->showMessage(tr("Printed %1").arg(currentTitle_), 3000);
}

bool MainWindow::printDocument(QPrinter& printer, int fromPage, int toPage) {
    if (pageCount_ <= 0 || worker_ == nullptr) {
        return false;
    }
    const int first = fromPage > 0 ? fromPage : 1;
    const int last = toPage > 0 ? std::min(toPage, pageCount_) : pageCount_;
    if (first > last) {
        return false;
    }

    QPainter painter;
    if (!painter.begin(&printer)) {
        return false;
    }

    // Render each page at ~150 DPI (zoom = 150/72) -- good print quality without
    // enormous images -- then scale it to fill the printable area, preserving
    // aspect ratio. The render happens on the worker thread; a blocking queued
    // call fetches the image synchronously, which is fine for a print operation.
    constexpr double kPrintZoom = 150.0 / 72.0;
    bool first_page = true;
    for (int p = first; p <= last; ++p) {
        QImage image;
        QMetaObject::invokeMethod(worker_, "renderAt", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(QImage, image), Q_ARG(int, p - 1),
                                  Q_ARG(double, kPrintZoom));
        if (!first_page) {
            printer.newPage();
        }
        first_page = false;
        if (image.isNull()) {
            continue;  // a bad page prints blank rather than aborting the job
        }

        const QRectF target = printer.pageRect(QPrinter::DevicePixel);
        QSizeF drawn = QSizeF(image.size()).scaled(target.size(), Qt::KeepAspectRatio);
        const QRectF where(target.x() + (target.width() - drawn.width()) / 2,
                           target.y() + (target.height() - drawn.height()) / 2,
                           drawn.width(), drawn.height());
        painter.drawImage(where, image);
    }
    painter.end();
    return true;
}
