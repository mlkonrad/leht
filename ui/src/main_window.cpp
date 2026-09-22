// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"

#include "sign_dialog.hpp"
#include "leht/crypto/crypto.hpp"

#include <QDateTime>
#include <QTreeWidgetItem>

#include "page_view.hpp"
#include "render_worker.hpp"

#include "leht/ops/forms.hpp"

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

#include <QActionGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QMenu>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>

#include "outline_model.hpp"
#include "thumbnail_bar.hpp"

MainWindow::MainWindow() {
    // Types that cross the worker-thread boundary in queued signals.
    qRegisterMetaType<AnnotRow>();
    qRegisterMetaType<QVector<AnnotRow>>();
    qRegisterMetaType<FieldRow>();
    qRegisterMetaType<QVector<FieldRow>>();
    qRegisterMetaType<QVector<QPolygonF>>();
    qRegisterMetaType<QVector<int>>();

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
    connect(worker_, &RenderWorker::pageFailed, view_, &PageView::markPageFailed);

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

    // Editing: the view's tools -> worker edits; worker changes -> view.
    connect(worker_, &RenderWorker::documentEdited, view_, &PageView::onDocumentEdited);
    connect(worker_, &RenderWorker::documentEdited, this,
            [this](const QVector<int>& pages, bool allPages, const QVector<QSize>&) {
        if (thumbnails_ != nullptr) {
            thumbnails_->invalidate(pages, allPages);
        }
        onWorker([](RenderWorker* w) {
            w->listAnnotations();
            w->listFields();
        });
    });
    connect(worker_, &RenderWorker::annotationsReady, view_, &PageView::setAnnotations);
    connect(worker_, &RenderWorker::fieldsReady, this, &MainWindow::onFieldsReady);
    connect(worker_, &RenderWorker::editStateChanged, this, &MainWindow::onEditStateChanged);
    connect(worker_, &RenderWorker::saved, this, &MainWindow::onSaved);
    connect(worker_, &RenderWorker::saveFailed, this, [this](const QString& why) {
        afterSave_ = nullptr;
        statusBar()->clearMessage();
        QMessageBox::warning(this, tr("Could not save"), why);
    });
    connect(worker_, &RenderWorker::editFailed, this, [this](const QString& why) {
        QMessageBox::warning(this, tr("Could not make that change"), why);
    });
    connect(worker_, &RenderWorker::redactionIncomplete, this, [this](const QStringList& where) {
        QMessageBox::warning(
            this, tr("Redaction incomplete"),
            tr("The text was removed from the pages, but it still appears here:\n\n• %1\n\n"
               "Leht does not change these on its own. Review them before sharing the file.")
                .arg(where.join(QStringLiteral("\n• "))));
    });
    connect(view_, &PageView::highlightRequested, this,
            [this](int page, const QVector<QRectF>& boxes) {
                onWorker([=](RenderWorker* w) { w->addHighlight(page, boxes, QColor(255, 220, 0)); });
            });
    connect(view_, &PageView::noteRequested, this, [this](int page, QPointF at) {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(this, tr("Add note"), tr("Note:"),
                                                            QString(), &ok);
        if (ok && !text.isEmpty()) {
            onWorker([=](RenderWorker* w) { w->addNote(page, at, text); });
        }
    });
    connect(view_, &PageView::inkRequested, this,
            [this](int page, const QVector<QPolygonF>& strokes) {
                onWorker([=](RenderWorker* w) { w->addInk(page, strokes, QColor(30, 60, 200)); });
            });
    connect(view_, &PageView::redactRequested, this, [this](int page, QRectF box) {
        if (!confirmBreakingSignatures(tr("A redaction"))) {
            return;
        }
        onWorker([=](RenderWorker* w) { w->redactArea(page, box); });
    });
    connect(view_, &PageView::signRequested, this, [this](int page, QRectF box) {
        startSigning(page, box);
    });
    connect(worker_, &RenderWorker::signaturesReady, this, &MainWindow::onSignaturesReady);
    connect(view_, &PageView::eraseRequested, this, [this](int id) {
        onWorker([=](RenderWorker* w) { w->deleteAnnotation(id); });
    });
    connect(view_, &PageView::toolRefused, this, [this](const QString& why) {
        statusBar()->showMessage(why, 5000);
        if (tools_ != nullptr && !tools_->actions().isEmpty()) {
            tools_->actions().first()->setChecked(true);  // Select
        }
    });

    workerThread_.start();

    buildActions();
    buildEditActions();
    buildSignaturePanel();

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

    // Form panel: one row per field, the value editable in place.
    auto* formDock = new QDockWidget(tr("Form"), this);
    formDock->setObjectName(QStringLiteral("formDock"));
    fields_ = new QTableWidget(0, 2, formDock);
    fields_->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
    fields_->horizontalHeader()->setStretchLastSection(true);
    fields_->verticalHeader()->hide();
    formDock->setWidget(fields_);
    addDockWidget(Qt::RightDockWidgetArea, formDock);
    formDock->hide();
    connect(fields_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (populatingFields_ || item->column() != 1) {
            return;
        }
        const QString name = fields_->item(item->row(), 0)->text();
        const QString value = item->text();
        onWorker([=](RenderWorker* w) { w->setFieldValue(name, value); });
    });
}

void MainWindow::onWorker(std::function<void(RenderWorker*)> fn) {
    RenderWorker* w = worker_;
    QMetaObject::invokeMethod(w, [w, fn = std::move(fn)] { fn(w); }, Qt::QueuedConnection);
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

void MainWindow::buildEditActions() {
    QToolBar* bar = addToolBar(tr("Edit"));
    bar->setObjectName(QStringLiteral("editBar"));
    bar->setMovable(false);

    saveAction_ = bar->addAction(tr("Save"));
    saveAction_->setShortcut(QKeySequence::Save);
    saveAction_->setEnabled(false);
    connect(saveAction_, &QAction::triggered, this, [this] { (void)save(); });

    auto* saveAsAction = new QAction(tr("Save As…"), this);
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, [this] { (void)saveAs(); });
    addAction(saveAsAction);

    undoAction_ = bar->addAction(tr("Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setEnabled(false);
    connect(undoAction_, &QAction::triggered, this,
            [this] { onWorker([](RenderWorker* w) { w->undo(); }); });
    redoAction_ = bar->addAction(tr("Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setEnabled(false);
    connect(redoAction_, &QAction::triggered, this,
            [this] { onWorker([](RenderWorker* w) { w->redo(); }); });

    bar->addSeparator();
    tools_ = new QActionGroup(this);
    tools_->setExclusive(true);
    const struct {
        const char* label;
        const char* tip;
        PageView::Tool tool;
    } kTools[] = {
        {"Select", "Select and copy text", PageView::Tool::Select},
        {"Highlight", "Drag across text to highlight it", PageView::Tool::Highlight},
        {"Note", "Click to add a sticky note", PageView::Tool::Note},
        {"Draw", "Draw freehand", PageView::Tool::Ink},
        {"Redact", "Drag a box: everything under it is removed from the file, "
                   "not just covered", PageView::Tool::Redact},
        {"Erase", "Click an annotation to delete it", PageView::Tool::Erase},
        {"Sign", "Drag a box to place a signature there", PageView::Tool::Sign},
    };
    for (const auto& t : kTools) {
        QAction* a = bar->addAction(tr(t.label));
        a->setToolTip(tr(t.tip));
        a->setCheckable(true);
        tools_->addAction(a);
        const PageView::Tool tool = t.tool;
        connect(a, &QAction::triggered, this, [this, tool] { (void)view_->setTool(tool); });
    }
    tools_->actions().first()->setChecked(true);

    auto* more = new QToolButton(bar);
    more->setText(tr("More"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->addAction(saveAsAction);
    menu->addSeparator();
    QAction* redactText = menu->addAction(tr("Redact Text…"));
    connect(redactText, &QAction::triggered, this, [this] {
        bool ok = false;
        const QString needle = QInputDialog::getText(
            this, tr("Redact text"),
            tr("Remove every occurrence of (case-insensitive):"), QLineEdit::Normal, QString(), &ok);
        if (ok && !needle.isEmpty() && confirmBreakingSignatures(tr("A redaction"))) {
            onWorker([=](RenderWorker* w) { w->redactText(needle); });
        }
    });
    QAction* watermark = menu->addAction(tr("Watermark…"));
    connect(watermark, &QAction::triggered, this, [this] {
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("Watermark"), tr("Text to stamp on every page:"),
                                                   QLineEdit::Normal, tr("DRAFT"), &ok);
        if (ok && !text.isEmpty()) {
            onWorker([=](RenderWorker* w) { w->addWatermark(text); });
        }
    });
    menu->addSeparator();
    QAction* signInvisibly = menu->addAction(tr("Sign Invisibly…"));
    signInvisibly->setToolTip(tr("Sign the document without marking a page"));
    connect(signInvisibly, &QAction::triggered, this, [this] { startSigning(0, QRectF()); });
    QAction* trustCert = menu->addAction(tr("Trust a Certificate…"));
    connect(trustCert, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Trust a certificate"), QString(),
            tr("Certificates (*.pem *.crt *.cer);;All files (*)"));
        if (!path.isEmpty()) {
            onWorker([=](RenderWorker* w) { w->addTrustedCertificate(path); });
        }
    });
    menu->addSeparator();
    QAction* crop = menu->addAction(tr("Crop Margins…"));
    connect(crop, &QAction::triggered, this, [this] {
        bool ok = false;
        const double points = QInputDialog::getDouble(
            this, tr("Crop margins"),
            tr("Points to trim from every edge of every page.\n"
               "Cropping hides content; it stays in the file. Use Redact to remove it."),
            36.0, 0.0, 1000.0, 1, &ok);
        if (ok && points > 0) {
            onWorker([=](RenderWorker* w) { w->cropMargins(points); });
        }
    });
    more->setMenu(menu);
    bar->addWidget(more);

    for (QAction* a : bar->actions()) {
        a->setEnabled(false);
    }
    more->setEnabled(false);
}

void MainWindow::buildSignaturePanel() {
    // A banner rather than a dialog: a document's signatures are a standing
    // fact about it, not an event, and the one thing a reader must not have to
    // go looking for.
    signatureBanner_ = new QToolBar(tr("Signatures"), this);
    signatureBanner_->setObjectName(QStringLiteral("signatureBanner"));
    signatureBanner_->setMovable(false);
    signatureBannerLabel_ = new QLabel(signatureBanner_);
    signatureBannerLabel_->setTextFormat(Qt::PlainText);
    signatureBanner_->addWidget(signatureBannerLabel_);
    QAction* details = signatureBanner_->addAction(tr("Details"));
    addToolBar(Qt::TopToolBarArea, signatureBanner_);
    signatureBanner_->hide();

    auto* dock = new QDockWidget(tr("Signatures"), this);
    dock->setObjectName(QStringLiteral("signatureDock"));
    signatures_ = new QTreeWidget(dock);
    signatures_->setHeaderLabels({tr("Signature"), tr("Details")});
    signatures_->setColumnWidth(0, 180);
    dock->setWidget(signatures_);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->hide();
    connect(details, &QAction::triggered, dock, &QWidget::show);

    // Clicking a signature goes to the page it is on.
    connect(signatures_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        const QVariant page = item->data(0, Qt::UserRole);
        if (page.isValid() && page.toInt() >= 0) {
            view_->goToPage(page.toInt());
        }
    });
}

namespace {

QString trustWord(int trust) {
    switch (static_cast<leht::crypto::Trust>(trust)) {
        case leht::crypto::Trust::Trusted:     return MainWindow::tr("trusted");
        case leht::crypto::Trust::Untrusted:   return MainWindow::tr("not trusted");
        case leht::crypto::Trust::Expired:     return MainWindow::tr("certificate expired");
        case leht::crypto::Trust::NotYetValid: return MainWindow::tr("certificate not yet valid");
        case leht::crypto::Trust::Unknown:     return MainWindow::tr("not checked");
    }
    return MainWindow::tr("not checked");
}

/// One line saying what this signature is worth, and the colour to say it in.
std::pair<QString, QColor> verdict(const SigRow& row) {
    if (!row.rangeOk || !row.intact) {
        return {MainWindow::tr("Broken"), QColor(170, 20, 20)};
    }
    const bool trusted = static_cast<leht::crypto::Trust>(row.trust) ==
                         leht::crypto::Trust::Trusted;
    if (row.changedAfterSigning && !row.laterSignatureCoversChanges) {
        return {MainWindow::tr("Intact, but the document was changed afterwards"),
                QColor(170, 110, 0)};
    }
    if (!trusted) {
        return {MainWindow::tr("Intact, signer not trusted"), QColor(170, 110, 0)};
    }
    return {MainWindow::tr("Valid"), QColor(20, 120, 40)};
}

QString localTime(qint64 unix_seconds) {
    return QDateTime::fromSecsSinceEpoch(unix_seconds).toString(Qt::ISODate);
}

}  // namespace

void MainWindow::onSignaturesReady(const QVector<SigRow>& rows) {
    signatureCount_ = static_cast<int>(rows.size());
    signatures_->clear();
    auto* dock = findChild<QDockWidget*>(QStringLiteral("signatureDock"));
    if (rows.isEmpty()) {
        signatureBanner_->hide();
        if (dock != nullptr) {
            dock->hide();
        }
        return;
    }

    int worst = 0;  // 0 valid, 1 a warning, 2 broken
    for (const SigRow& row : rows) {
        const auto [word, colour] = verdict(row);
        auto* item = new QTreeWidgetItem(signatures_);
        item->setText(0, row.signerCommonName.isEmpty() ? row.field : row.signerCommonName);
        item->setText(1, word);
        item->setForeground(1, colour);
        item->setData(0, Qt::UserRole, row.page);
        const auto add = [item](const QString& key, const QString& value) {
            if (!value.isEmpty()) {
                auto* child = new QTreeWidgetItem(item);
                child->setText(0, key);
                child->setText(1, value);
                child->setData(0, Qt::UserRole, -1);
            }
        };
        if (!row.rangeOk) {
            add(tr("Problem"), row.rangeProblem);
        } else if (!row.intact) {
            add(tr("Problem"), row.problem);
        }
        add(tr("Field"), row.field);
        add(tr("Signer"), row.signer);
        add(tr("Issuer"), row.issuer);
        add(tr("Trust"), row.trustDetail.isEmpty() ? trustWord(row.trust)
                                                   : tr("%1: %2").arg(trustWord(row.trust),
                                                                      row.trustDetail));
        if (row.notAfter != 0) {
            add(tr("Certificate valid until"), localTime(row.notAfter));
        }
        add(tr("Algorithm"), row.digest.isEmpty() ? row.subfilter
                                                  : tr("%1, %2").arg(row.digest, row.subfilter));
        add(tr("Claimed time"), row.claimedTime);
        if (row.hasTimestamp) {
            add(tr("Timestamp"),
                row.timestampValid
                    ? tr("%1, by %2 (%3)").arg(localTime(row.timestampTime), row.authority,
                                               trustWord(row.timestampTrust))
                    : tr("not valid: %1").arg(row.timestampProblem));
        } else if (row.intact) {
            add(tr("Timestamp"), tr("none: nothing proves when this was signed"));
        }
        add(tr("Reason"), row.reason);
        add(tr("Location"), row.location);
        if (row.changedAfterSigning) {
            add(tr("Changed"), row.laterSignatureCoversChanges
                                   ? tr("yes, and a later signature covers those changes")
                                   : tr("yes: the document was added to after this signature"));
        }
        add(tr("Certificate fingerprint"), row.fingerprint);

        if (!row.rangeOk || !row.intact) {
            worst = 2;
        } else if (worst < 1 && (row.changedAfterSigning && !row.laterSignatureCoversChanges)) {
            worst = 1;
        } else if (worst < 1 && static_cast<leht::crypto::Trust>(row.trust) !=
                                    leht::crypto::Trust::Trusted) {
            worst = 1;
        }
    }
    signatures_->expandAll();

    const QString summary =
        worst == 2 ? tr("⚠ This document has a broken signature.")
        : worst == 1 ? tr("This document is signed, with something worth checking.")
                     : tr("✓ Signed and verified.");
    signatureBannerLabel_->setText(tr(" %1  (%n signature(s)) ", nullptr, signatureCount_)
                                       .arg(summary));
    QPalette pal = signatureBannerLabel_->palette();
    pal.setColor(QPalette::WindowText, worst == 2 ? QColor(170, 20, 20)
                                       : worst == 1 ? QColor(140, 90, 0)
                                                    : QColor(20, 110, 40));
    signatureBannerLabel_->setPalette(pal);
    signatureBanner_->show();
    if (dock != nullptr && worst > 0) {
        dock->show();
    }
}

bool MainWindow::confirmBreakingSignatures(const QString& what) {
    if (signatureCount_ == 0) {
        return true;
    }
    const auto answer = QMessageBox::warning(
        this, tr("This document is signed"),
        tr("%1 cannot be added as a new revision: it rewrites the file, and the %n existing "
           "signature(s) will no longer verify.\n\nCarry on?", nullptr, signatureCount_)
            .arg(what),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Yes;
}

void MainWindow::startSigning(int page, QRectF rect) {
    if (pageCount_ == 0) {
        return;
    }
    SignDialog dialog(this, page, rect, QString());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    SignSpec spec = dialog.spec();
    // Signing writes a file, so it needs a path. A document opened read-only
    // from somewhere unwritable is signed with Save As.
    QString target = currentPath_;
    if (target.isEmpty()) {
        target = QFileDialog::getSaveFileName(this, tr("Save signed document as"), QString(),
                                              tr("PDF documents (*.pdf)"));
        if (target.isEmpty()) {
            return;
        }
    }
    statusBar()->showMessage(tr("Signing…"));
    onWorker([=](RenderWorker* w) { w->signDocument(target, spec); });
}

void MainWindow::onEditStateChanged(bool canUndo, bool canRedo, bool modified) {
    undoAction_->setEnabled(canUndo);
    redoAction_->setEnabled(canRedo);
    modified_ = modified;
    saveAction_->setEnabled(pageCount_ > 0);
    updateTitle();
}

void MainWindow::updateTitle() {
    if (currentTitle_.isEmpty()) {
        setWindowTitle(tr("Leht"));
        return;
    }
    setWindowTitle(tr("%1%2 — Leht").arg(currentTitle_, modified_ ? QStringLiteral(" *") : QString()));
}

void MainWindow::onFieldsReady(const QVector<FieldRow>& rows) {
    auto* dock = findChild<QDockWidget*>(QStringLiteral("formDock"));
    populatingFields_ = true;
    fields_->setRowCount(0);
    fields_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        const FieldRow& f = rows[i];
        auto* name = new QTableWidgetItem(f.name);
        name->setFlags(Qt::ItemIsEnabled);
        fields_->setItem(i, 0, name);
        auto* value = new QTableWidgetItem(f.value);
        if (f.readOnly) {
            value->setFlags(Qt::ItemIsEnabled);
            value->setToolTip(tr("Read-only field"));
        }
        fields_->setItem(i, 1, value);

        // Fields with a fixed set of values get a drop-down.
        const auto type = static_cast<leht::ops::FieldType>(f.type);
        const bool button =
            type == leht::ops::FieldType::Checkbox || type == leht::ops::FieldType::Radio;
        if ((button || type == leht::ops::FieldType::Choice) && !f.readOnly) {
            auto* combo = new QComboBox(fields_);
            QStringList choices = f.options;
            if (button) {
                choices.push_back(QStringLiteral("Off"));
            }
            combo->addItems(choices);
            combo->setCurrentText(f.value);
            const QString fieldName = f.name;
            connect(combo, &QComboBox::activated, this, [this, combo, fieldName] {
                const QString v = combo->currentText();
                onWorker([=](RenderWorker* w) { w->setFieldValue(fieldName, v); });
            });
            fields_->setCellWidget(i, 1, combo);
        }
    }
    populatingFields_ = false;
    if (dock != nullptr) {
        dock->setVisible(!rows.isEmpty());
    }
}

bool MainWindow::save() {
    if (pageCount_ <= 0) {
        return false;
    }
    if (currentPath_.isEmpty() || !currentPath_.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        return saveAs();  // an image or XPS opened for viewing is not written back as PDF in place
    }
    statusBar()->showMessage(tr("Saving…"));
    const QString path = currentPath_;
    onWorker([=](RenderWorker* w) { w->save(path); });
    return true;
}

bool MainWindow::saveAs() {
    if (pageCount_ <= 0) {
        return false;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Save PDF"), currentPath_,
                                                      tr("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        afterSave_ = nullptr;
        return false;
    }
    statusBar()->showMessage(tr("Saving…"));
    onWorker([=](RenderWorker* w) { w->save(path); });
    return true;
}

void MainWindow::onSaved(const QString& path) {
    currentPath_ = path;
    currentTitle_ = QFileInfo(path).fileName();
    modified_ = false;
    updateTitle();
    statusBar()->showMessage(tr("Saved %1").arg(currentTitle_), 3000);
    if (auto then = std::exchange(afterSave_, nullptr)) {
        then();
    }
}

bool MainWindow::resolveUnsaved(std::function<void()> then) {
    if (!modified_) {
        return true;
    }
    const auto choice = QMessageBox::question(
        this, tr("Unsaved changes"),
        tr("“%1” has changes that are not saved. Save them?").arg(currentTitle_),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Discard) {
        return true;
    }
    if (choice == QMessageBox::Save) {
        afterSave_ = std::move(then);
        if (!save()) {
            afterSave_ = nullptr;
        }
    }
    return false;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (resolveUnsaved([this] {
            modified_ = false;
            close();
        })) {
        event->accept();
    } else {
        event->ignore();
    }
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
    if (!resolveUnsaved([this, path] { openPath(path); })) {
        return;
    }
    modified_ = false;
    currentPath_ = QFileInfo(path).absoluteFilePath();
    currentTitle_ = QFileInfo(path).fileName();
    (void)view_->setTool(PageView::Tool::Select);
    if (tools_ != nullptr) {
        tools_->actions().first()->setChecked(true);
    }
    statusBar()->showMessage(tr("Opening %1…").arg(currentTitle_));
    view_->clear();
    if (thumbnails_ != nullptr) {
        thumbnails_->clearThumbnails();
    }
    emit requestOpen(path);
}

void MainWindow::onOpened(int pageCount, QVector<QSize> baseSizes) {
    pageCount_ = pageCount;
    updateTitle();
    if (auto* bar = findChild<QToolBar*>(QStringLiteral("editBar"))) {
        for (QAction* a : bar->actions()) {
            a->setEnabled(true);
        }
        for (QWidget* w : bar->findChildren<QToolButton*>()) {
            w->setEnabled(true);
        }
    }
    undoAction_->setEnabled(false);
    redoAction_->setEnabled(false);
    signatureCount_ = 0;
    onWorker([](RenderWorker* w) {
        w->listAnnotations();
        w->listFields();
        w->listSignatures();
    });
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
