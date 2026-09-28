// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"

#include "sign_dialog.hpp"
#include "leht/crypto/crypto.hpp"

#include <QCursor>
#include <QDateTime>
#include <QSet>
#include <QTreeWidgetItem>

#include "page_grid.hpp"
#include "page_view.hpp"
#include "page_dialogs.hpp"
#include "file_tools.hpp"
#include "file_tools_dialogs.hpp"
#include "properties_dialog.hpp"
#include "protect_dialog.hpp"
#include "signature_cards.hpp"
#ifdef LEHT_HAVE_OCR
#include "leht/ocr/ocr.hpp"
#endif
#include <QProgressDialog>
#include <memory>
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
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>
#include <QPushButton>

#include "actions.hpp"
#include "comments_panel.hpp"
#include "icons.hpp"
#include "mode_bar.hpp"
#include "outline_model.hpp"
#include "preferences.hpp"
#include "recent_files.hpp"
#include "sidebar.hpp"
#include "thumbnail_bar.hpp"
#include "welcome_view.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>

MainWindow::MainWindow() {
    // Types that cross the worker-thread boundary in queued signals.
    qRegisterMetaType<AnnotRow>();
    qRegisterMetaType<QVector<AnnotRow>>();
    qRegisterMetaType<FieldRow>();
    qRegisterMetaType<QVector<FieldRow>>();
    qRegisterMetaType<QVector<QPolygonF>>();
    qRegisterMetaType<QVector<int>>();

    setWindowTitle(tr("Leht"));
    resize(1180, 860);
    setAcceptDrops(true);  // files dropped anywhere on the window (see dropEvent)

    // Placed by buildLayout(); made first because everything below wires to it.
    view_ = new PageView(this);
    sidebar_ = new Sidebar(this);
    actions_ = new ActionRegistry(this);

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
                if (pageGrid_ != nullptr) {
                    pageGrid_->onThumbnail(page, img);
                }
            });

    connect(view_, &PageView::currentPageChanged, this,
            &MainWindow::onCurrentPageChanged);

    // Editing: the view's tools -> worker edits; worker changes -> view.
    // Before the view hears of the edit: its pictures are of the wrong pages.
    connect(worker_, &RenderWorker::pagesRearranged, this, [this] {
        redactionMarks_.clear();  // marks name pages that have moved
        view_->forgetPages();
        rearranged_ = true;
    });
    connect(worker_, &RenderWorker::documentEdited, view_, &PageView::onDocumentEdited);
    connect(worker_, &RenderWorker::documentEdited, this,
            [this](const QVector<int>& pages, bool allPages, const QVector<QSize>& sizes) {
        if (std::exchange(rearranged_, false) || sizes.size() != pageCount_) {
            // Pages came, went or moved: every thumbnail and count is new.
            pageCount_ = static_cast<int>(sizes.size());
            thumbnails_->setPageCount(pageCount_);
            pageGrid_->setPageCount(pageCount_);
            if (!gridSelectionAfterEdit_.isEmpty()) {
                pageGrid_->selectPages(std::exchange(gridSelectionAfterEdit_, {}));
            }
            pageSpin_->setMaximum(qMax(1, pageCount_));
            onCurrentPageChanged(view_->currentPage());
        } else {
            thumbnails_->invalidate(pages, allPages);
            if (allPages || !pages.isEmpty()) {
                pageGrid_->invalidate();
            }
        }
        onWorker([](RenderWorker* w) {
            w->listAnnotations();
            w->listFields();
        });
    });
    connect(worker_, &RenderWorker::annotationsReady, view_, &PageView::setAnnotations);
    connect(worker_, &RenderWorker::annotationsReady, this, [this](const QVector<AnnotRow>& rows) {
        if (comments_ != nullptr) {
            comments_->setAnnotations(rows);
        }
    });
    connect(worker_, &RenderWorker::fieldsReady, this, &MainWindow::onFieldsReady);
    connect(worker_, &RenderWorker::editStateChanged, this, &MainWindow::onEditStateChanged);
    connect(worker_, &RenderWorker::saved, this, &MainWindow::onSaved);
    // Every network contact says where it goes, as it goes.
    connect(worker_, &RenderWorker::networkUsed, this, [this](const QString& hosts) {
        statusBar()->showMessage(
            tr("Contacting %1 (certificate identifiers only, never the document)…").arg(hosts));
    });
    connect(worker_, &RenderWorker::longTermValidationAdded, this,
            [this](int certs, int ocsps, int crls, qint64 timestamp) {
                QString what = tr("Embedded %n certificate(s)", nullptr, certs) +
                               tr(", %n OCSP response(s)", nullptr, ocsps) +
                               tr(" and %n CRL(s)", nullptr, crls);
                if (timestamp != 0) {
                    what += tr(", then a document timestamp");
                }
                statusBar()->showMessage(what + QStringLiteral("."), 8000);
            });
    connect(worker_, &RenderWorker::trustedListUpdated, this,
            [this](int verified, int lists, int services, const QStringList& failed) {
                QString what = tr("EU trusted lists: %1 of %2 verified, %n qualified service(s).",
                                  nullptr, services)
                                   .arg(verified)
                                   .arg(lists);
                if (!failed.isEmpty()) {
                    what += QLatin1Char(' ') +
                            tr("Could not be verified: %1.").arg(failed.join(QStringLiteral(", ")));
                }
                statusBar()->showMessage(what, 12000);
            });
    connect(worker_, &RenderWorker::trustedListFailed, this, [this](const QString& why) {
        statusBar()->clearMessage();
        QMessageBox::warning(this, tr("EU trusted lists"),
                             tr("The trusted lists were not updated; the ones cached before are "
                                "still used.\n\n%1").arg(why));
    });
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
    // The Redact tool marks; Apply Redactions removes. Nothing leaves the
    // file until the marks have been looked over.
    connect(view_, &PageView::redactRequested, this, [this](int page, QRectF box) {
        auto marks = redactionMarks_;
        marks.push_back({page, box});
        setRedactionMarks(marks);
        statusBar()->showMessage(
            tr("%n area(s) marked for redaction. Review them, then choose Apply Redactions.", nullptr,
               static_cast<int>(marks.size())),
            8000);
    });
    connect(view_, &PageView::markupRequested, this,
            [this](int page, const QVector<QRectF>& boxes, bool strikeOut) {
                const QColor color = strikeOut ? QColor(200, 30, 30) : QColor(20, 90, 200);
                onWorker([=](RenderWorker* w) { w->addTextMarkup(page, boxes, strikeOut, color); });
            });
    connect(view_, &PageView::stampRequested, this, [this](int page, QPointF at) {
        // The standard stamps (ops::AnnotSpec::stamp), by what they say.
        const struct {
            const char* name;
            const char* label;
        } kStamps[] = {
            {"Approved", QT_TR_NOOP("Approved")}, {"NotApproved", QT_TR_NOOP("Not Approved")},
            {"Draft", QT_TR_NOOP("Draft")}, {"Final", QT_TR_NOOP("Final")},
            {"Confidential", QT_TR_NOOP("Confidential")}, {"ForComment", QT_TR_NOOP("For Comment")},
            {"ForPublicRelease", QT_TR_NOOP("For Public Release")},
            {"NotForPublicRelease", QT_TR_NOOP("Not For Public Release")},
            {"Experimental", QT_TR_NOOP("Experimental")}, {"Expired", QT_TR_NOOP("Expired")},
            {"AsIs", QT_TR_NOOP("As Is")}, {"Departmental", QT_TR_NOOP("Departmental")},
            {"Sold", QT_TR_NOOP("Sold")}, {"TopSecret", QT_TR_NOOP("Top Secret")},
        };
        QMenu menu(this);
        for (const auto& s : kStamps) {
            menu.addAction(tr(s.label))->setData(QLatin1String(s.name));
        }
        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == nullptr) {
            return;
        }
        // Centred on the click, and kept on the page.
        const QSizeF pageSize = view_->pageSizePoints(page);
        QRectF box(QPointF(0, 0), QSizeF(180, 50));
        box.moveCenter(at);
        box.moveLeft(std::clamp(box.left(), 0.0, std::max(0.0, pageSize.width() - box.width())));
        box.moveTop(std::clamp(box.top(), 0.0, std::max(0.0, pageSize.height() - box.height())));
        const QString name = chosen->data().toString();
        onWorker([=](RenderWorker* w) { w->addStamp(page, box, name); });
    });
    connect(view_, &PageView::signRequested, this, [this](int page, QRectF box) {
        startSigning(page, box);
    });
    connect(worker_, &RenderWorker::signaturesReady, this, &MainWindow::onSignaturesReady);
    connect(view_, &PageView::moveRequested, this, [this](int id, QRectF to) {
        onWorker([=](RenderWorker* w) { w->moveAnnotation(id, to); });
    });
    connect(view_, &PageView::freeTextRequested, this,
            [this](int page, QRectF box, QString text, double size, QColor color) {
        onWorker([=](RenderWorker* w) { w->addFreeText(page, box, text, size, color); });
    });
    connect(view_, &PageView::annotationTextRequested, this, [this](int id, QString text) {
        onWorker([=](RenderWorker* w) { w->setAnnotationText(id, text); });
    });
    connect(view_, &PageView::noteEditRequested, this, [this](int id, QString current) {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(this, tr("Note"), tr("Note text:"),
                                                            current, &ok);
        if (ok && text != current) {
            if (text.trimmed().isEmpty()) {
                onWorker([=](RenderWorker* w) { w->deleteAnnotation(id); });
            } else {
                onWorker([=](RenderWorker* w) { w->setAnnotationText(id, text); });
            }
        }
    });
    connect(view_, &PageView::cropBoxRequested, this, [this](int page, QRectF box) {
        const QString pages = askCropPages(this, page, view_->pageCount());
        if (!pages.isNull()) {
            onWorker([=](RenderWorker* w) { w->cropBox(pages, box); });
        }
    });
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
    buildFileTools();
    buildPageActions();
    buildMainToolbar();
    buildSignaturePanel();
    buildLayout();
    buildMenus();
    applyAppearance();
    showWelcome();
}

void MainWindow::recognizeText() {
#ifndef LEHT_HAVE_OCR
    QMessageBox::information(this, tr("Recognize text"),
                             tr("This build of Leht was made without OCR."));
#else
    QStringList installed;
    for (const std::string& code :
         leht::ocr::installed_languages(leht::ocr::default_datadir())) {
        installed << QString::fromStdString(code);
    }
    if (installed.isEmpty()) {
        QMessageBox::information(
            this, tr("Recognize text"),
            tr("No OCR languages are installed. Install Tesseract's language data -- on "
               "Fedora, tesseract-langpack-est and tesseract-langpack-eng."));
        return;
    }
    OcrDialog dialog(this, view_->pageCount(), installed);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    // Progress per page, with Cancel: the run is on the worker thread, and the
    // cancel reaches it directly, not through its (busy) event queue.
    auto* progress = new QProgressDialog(tr("Starting OCR…"), tr("Cancel"), 0, 0, this);
    progress->setWindowTitle(tr("Recognize text"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    RenderWorker* worker = worker_;
    connect(progress, &QProgressDialog::canceled, this, [worker] { worker->cancelRecognition(); });
    const auto step = connect(worker_, &RenderWorker::ocrProgress, progress,
                              [progress](int done, int total, int page) {
        progress->setMaximum(total);
        progress->setValue(done);
        if (page >= 0) {
            progress->setLabelText(tr("Reading page %1 (%2 of %3)…")
                                       .arg(page + 1).arg(done + 1).arg(total));
        }
    });
    auto finished = std::make_shared<QMetaObject::Connection>();
    *finished = connect(worker_, &RenderWorker::ocrFinished, this,
                        [this, progress, step, finished](int words, int pages, bool cancelled,
                                                         const QString& error) {
        disconnect(step);
        disconnect(*finished);
        progress->close();
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Recognize text"), error);
        }
        statusBar()->showMessage(
            cancelled ? tr("OCR cancelled: %n word(s) read", nullptr, words) + tr(" on %n page(s).", nullptr, pages)
                      : tr("OCR read %n word(s)", nullptr, words) + tr(" on %n page(s).", nullptr, pages),
            8000);
    });
    const QString pages = dialog.pages();
    const QString languages = dialog.languages();
    const int dpi = dialog.dpi();
    const bool skip = dialog.skipPagesWithText();
    onWorker([=](RenderWorker* w) { w->recognizeText(pages, languages, dpi, skip); });
#endif
}

void MainWindow::onWorker(std::function<void(RenderWorker*)> fn) {
    RenderWorker* w = worker_;
    QMetaObject::invokeMethod(w, [w, fn = std::move(fn)] { fn(w); }, Qt::QueuedConnection);
}

MainWindow::~MainWindow() {
    workerThread_.quit();
    workerThread_.wait();
    fileTools_->cancel();
    fileToolsThread_.quit();
    fileToolsThread_.wait();
}

bool MainWindow::certAllows(int needs) const {
    return needs == 0 || certLevel_ == 0 || certLevel_ >= needs;
}

void MainWindow::buildActions() {
    using Spec = ActionRegistry::Spec;
    const auto open = [this] { return pageCount_ > 0; };
    const auto add = [this](Spec spec, auto&& slot) {
        QAction* a = actions_->add(spec);
        connect(a, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return a;
    };
    const QString file = tr("File");
    const QString edit = tr("Edit");
    const QString view = tr("View");
    const QString help = tr("Help");

    // File.
    add({.id = QStringLiteral("open"), .text = tr("&Open…"), .icon = QStringLiteral("folder-open"),
         .themeIcon = QStringLiteral("document-open"), .shortcuts = {QKeySequence::Open},
         .tip = tr("Open a PDF"), .group = file},
        &MainWindow::openDialog);
    add({.id = QStringLiteral("close"), .text = tr("&Close"), .icon = QStringLiteral("x"),
         .themeIcon = QStringLiteral("window-close"), .shortcuts = {QKeySequence::Close},
         .tip = tr("Close the document"), .enabledWhen = open, .group = file},
        &MainWindow::closeDocument);
    saveAction_ = add({.id = QStringLiteral("save"), .text = tr("&Save"), .icon = QStringLiteral("save"),
                       .themeIcon = QStringLiteral("document-save"), .shortcuts = {QKeySequence::Save},
                       .tip = tr("Save your changes to this file"), .enabledWhen = open, .group = file},
                      [this] { (void)save(); });
    add({.id = QStringLiteral("saveAs"), .text = tr("Save &As…"), .icon = QStringLiteral("save-all"),
         .themeIcon = QStringLiteral("document-save-as"), .shortcuts = {QKeySequence::SaveAs},
         .tip = tr("Save a copy under another name"), .enabledWhen = open, .group = file},
        [this] { (void)saveAs(); });
    add({.id = QStringLiteral("properties"), .text = tr("Document P&roperties…"), .icon = QStringLiteral("file-text"),
         .themeIcon = QStringLiteral("document-properties"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_D)},
         .tip = tr("Title, author and keywords, and what the file is"), .enabledWhen = open, .group = file},
        [this] {
            // Asked of the worker; the dialog opens when the answer comes.
            auto once = std::make_shared<QMetaObject::Connection>();
            *once = connect(worker_, &RenderWorker::infoReady, this,
                            [this, once](const QStringList& keys, const QStringList& values) {
                disconnect(*once);
                QHash<QString, QString> info;
                for (int i = 0; i < keys.size() && i < values.size(); ++i) {
                    info.insert(keys[i], values[i]);
                }
                const bool editable = certAllows(2) && info.value(QStringLiteral("format")).startsWith(QLatin1String("PDF"));
                PropertiesDialog dialog(this, currentPath_, pageCount_, view_->pageSizePoints(0), info, editable);
                if (dialog.exec() != QDialog::Accepted) {
                    return;
                }
                const auto changes = dialog.changes();
                if (changes.isEmpty()) {
                    return;
                }
                // One Undo takes back the whole dialog.
                onWorker([changes](RenderWorker* w) {
                    w->beginEditGroup();
                    for (const auto& [key, value] : changes) {
                        w->setInfo(key, value);
                    }
                    w->endEditGroup();
                });
            });
            onWorker([](RenderWorker* w) { w->requestInfo(); });
        });
    add({.id = QStringLiteral("print"), .text = tr("&Print…"), .icon = QStringLiteral("printer"),
         .themeIcon = QStringLiteral("document-print"), .shortcuts = {QKeySequence::Print},
         .enabledWhen = open, .group = file},
        &MainWindow::printDialog);
    // Through close(), so unsaved edits are asked about.
    add({.id = QStringLiteral("quit"), .text = tr("&Quit"), .icon = QStringLiteral("log-out"),
         .themeIcon = QStringLiteral("application-exit"), .shortcuts = {QKeySequence::Quit}, .group = file},
        [this] { close(); });

    // Edit.
    undoAction_ = add({.id = QStringLiteral("undo"), .text = tr("&Undo"), .icon = QStringLiteral("undo-2"),
                       .themeIcon = QStringLiteral("edit-undo"), .shortcuts = {QKeySequence::Undo},
                       .group = edit},
                      [this] { onWorker([](RenderWorker* w) { w->undo(); }); });
    redoAction_ = add({.id = QStringLiteral("redo"), .text = tr("&Redo"), .icon = QStringLiteral("redo-2"),
                       .themeIcon = QStringLiteral("edit-redo"), .shortcuts = {QKeySequence::Redo},
                       .group = edit},
                      [this] { onWorker([](RenderWorker* w) { w->redo(); }); });
    undoAction_->setEnabled(false);
    redoAction_->setEnabled(false);
    add({.id = QStringLiteral("copy"), .text = tr("&Copy"), .icon = QStringLiteral("copy"),
         .themeIcon = QStringLiteral("edit-copy"), .shortcuts = {QKeySequence::Copy},
         .tip = tr("Copy the selected text"), .enabledWhen = open, .group = edit},
        [this] { view_->copySelection(); });
    add({.id = QStringLiteral("find"), .text = tr("&Find…"), .icon = QStringLiteral("search"),
         .themeIcon = QStringLiteral("edit-find"), .shortcuts = {QKeySequence::Find},
         .tip = tr("Find text in the document"), .enabledWhen = open, .group = edit},
        &MainWindow::showFindBar);
    add({.id = QStringLiteral("findNext"), .text = tr("Find &Next"), .icon = QStringLiteral("chevron-down"),
         .shortcuts = {QKeySequence::FindNext}, .enabledWhen = open, .group = edit},
        [this] { view_->nextMatch(); });
    add({.id = QStringLiteral("findPrevious"), .text = tr("Find Pre&vious"), .icon = QStringLiteral("chevron-up"),
         .shortcuts = {QKeySequence::FindPrevious}, .enabledWhen = open, .group = edit},
        [this] { view_->prevMatch(); });
    add({.id = QStringLiteral("preferences"), .text = tr("Pre&ferences…"), .icon = QStringLiteral("settings"),
         .themeIcon = QStringLiteral("preferences-system"),
         .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_Comma)}, .group = edit},
        [this] { openPreferences(); });

    // View.
    add({.id = QStringLiteral("zoomIn"), .text = tr("Zoom &In"), .icon = QStringLiteral("zoom-in"),
         .themeIcon = QStringLiteral("zoom-in"), .shortcuts = {QKeySequence::ZoomIn},
         .enabledWhen = open, .group = view},
        [this] {
            view_->zoomBy(1.25);
            updateZoomLabel();
        });
    add({.id = QStringLiteral("zoomOut"), .text = tr("Zoom &Out"), .icon = QStringLiteral("zoom-out"),
         .themeIcon = QStringLiteral("zoom-out"), .shortcuts = {QKeySequence::ZoomOut},
         .enabledWhen = open, .group = view},
        [this] {
            view_->zoomBy(0.8);
            updateZoomLabel();
        });
    add({.id = QStringLiteral("actualSize"), .text = tr("&Actual Size"), .icon = QStringLiteral("scan"),
         .themeIcon = QStringLiteral("zoom-original"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_1)},
         .enabledWhen = open, .group = view},
        [this] {
            view_->setZoom(1.0);
            updateZoomLabel();
        });
    add({.id = QStringLiteral("fitWidth"), .text = tr("Fit &Width"), .icon = QStringLiteral("arrow-left-right"),
         .themeIcon = QStringLiteral("zoom-fit-width"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_0)},
         .enabledWhen = open, .group = view},
        [this] {
            view_->fitWidth();
            updateZoomLabel();
        });
    add({.id = QStringLiteral("fitPage"), .text = tr("Fit &Page"), .icon = QStringLiteral("maximize"),
         .themeIcon = QStringLiteral("zoom-fit-best"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_9)},
         .enabledWhen = open, .group = view},
        [this] {
            view_->fitPage();
            updateZoomLabel();
        });
    add({.id = QStringLiteral("rotateView"), .text = tr("&Rotate View"), .icon = QStringLiteral("rotate-cw"),
         .themeIcon = QStringLiteral("object-rotate-right"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_R)},
         .tip = tr("Turn the pages on screen; the file is not changed"), .enabledWhen = open, .group = view},
        [this] { view_->rotateBy(90); });
    QAction* sidebar = add({.id = QStringLiteral("toggleSidebar"), .text = tr("&Sidebar"),
                            .icon = QStringLiteral("panel-left"), .shortcuts = {QKeySequence(Qt::Key_F9)},
                            .tip = tr("Show or hide the sidebar"), .checkable = true,
                            .enabledWhen = open, .group = view},
                           [this](bool on) { sidebar_->setExpanded(on); });
    sidebar->setChecked(true);
    connect(sidebar_, &Sidebar::expandedChanged, sidebar, &QAction::setChecked);
    add({.id = QStringLiteral("fullScreen"), .text = tr("F&ull Screen"), .icon = QStringLiteral("maximize"),
         .themeIcon = QStringLiteral("view-fullscreen"), .shortcuts = {QKeySequence::FullScreen},
         .checkable = true, .group = view},
        [this](bool on) { on ? showFullScreen() : showNormal(); });

    // Help.
    add({.id = QStringLiteral("shortcuts"), .text = tr("&Keyboard Shortcuts"), .icon = QStringLiteral("keyboard"),
         .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_Question), QKeySequence(Qt::Key_F1)}, .group = help},
        [this] { showShortcutSheet(this, *actions_); });
    add({.id = QStringLiteral("about"), .text = tr("&About Leht"), .icon = QStringLiteral("info"),
         .themeIcon = QStringLiteral("help-about"), .group = help},
        &MainWindow::showAbout);
}

void MainWindow::buildEditActions() {
    using Spec = ActionRegistry::Spec;
    const QString comment = tr("Comment");
    tools_ = new QActionGroup(this);
    tools_->setExclusive(true);
    // The certification level from which each tool is allowed (see
    // applyCertification): 2 signing, 3 annotations, 4 never.
    const struct {
        const char* id;
        const char* label;
        const char* icon;
        const char* tip;
        PageView::Tool tool;
        int certNeeds;
    } kTools[] = {
        {"toolSelect", QT_TR_NOOP("Select"), "text-cursor",
         QT_TR_NOOP("Select and copy text; double-click text you added to edit it"), PageView::Tool::Select, 0},
        {"toolHighlight", QT_TR_NOOP("Highlight"), "highlighter", QT_TR_NOOP("Drag across text to highlight it"),
         PageView::Tool::Highlight, 3},
        {"toolNote", QT_TR_NOOP("Note"), "sticky-note", QT_TR_NOOP("Click to add a sticky note"),
         PageView::Tool::Note, 3},
        {"toolText", QT_TR_NOOP("Text Box"), "type",
         QT_TR_NOOP("Drag a box (or click) and type; Ctrl+Enter or click away to finish, Esc to cancel"),
         PageView::Tool::Text, 3},
        {"toolUnderline", QT_TR_NOOP("Underline"), "underline", QT_TR_NOOP("Drag across text to underline it"),
         PageView::Tool::Underline, 3},
        {"toolStrike", QT_TR_NOOP("Strike Out"), "strikethrough",
         QT_TR_NOOP("Drag across text to strike it out"), PageView::Tool::StrikeOut, 3},
        {"toolDraw", QT_TR_NOOP("Draw"), "pen-line", QT_TR_NOOP("Draw freehand"), PageView::Tool::Ink, 3},
        {"toolStamp", QT_TR_NOOP("Stamp"), "stamp",
         QT_TR_NOOP("Click where a stamp such as Approved or Draft should go"), PageView::Tool::Stamp, 3},
        {"toolMove", QT_TR_NOOP("Move"), "move",
         QT_TR_NOOP("Click an annotation to select it: drag it to move, drag a handle to resize, arrows to "
                    "nudge, Delete to remove"),
         PageView::Tool::Move, 3},
        {"toolErase", QT_TR_NOOP("Erase"), "eraser", QT_TR_NOOP("Click an annotation to delete it"),
         PageView::Tool::Erase, 3},
        {"toolSign", QT_TR_NOOP("Sign"), "signature", QT_TR_NOOP("Drag a box to place a signature there"),
         PageView::Tool::Sign, 2},
        {"toolRedact", QT_TR_NOOP("Mark for Redaction"), "square-dashed",
         QT_TR_NOOP("Drag boxes over what must go; Apply Redactions then removes it from the file, "
                    "not just covers it"),
         PageView::Tool::Redact, 4},
        {"toolCrop", QT_TR_NOOP("Crop"), "crop",
         QT_TR_NOOP("Drag the box to keep: the rest of the page is hidden, not removed"), PageView::Tool::Crop, 4},
    };
    for (const auto& t : kTools) {
        const int needs = t.certNeeds;
        QAction* a = actions_->add({.id = QLatin1String(t.id), .text = tr(t.label), .icon = QLatin1String(t.icon),
                                    .tip = tr(t.tip), .certNeeds = needs, .checkable = true,
                                    .enabledWhen = [this, needs] { return pageCount_ > 0 && certAllows(needs); },
                                    .group = comment});
        a->setProperty("certNeeds", needs);  // also for Select (0), which applyCertification skips
        tools_->addAction(a);
        const PageView::Tool tool = t.tool;
        connect(a, &QAction::triggered, this, [this, tool] { (void)view_->setTool(tool); });
    }
    tools_->actions().first()->setChecked(true);

    const auto add = [this](Spec spec, auto&& slot) {
        const int needs = spec.certNeeds;
        if (!spec.enabledWhen) {
            spec.enabledWhen = [this, needs] { return pageCount_ > 0 && certAllows(needs); };
        }
        QAction* a = actions_->add(spec);
        connect(a, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return a;
    };
    const QString sign = tr("Sign");
    const QString tools = tr("Tools");
    add({.id = QStringLiteral("redactText"), .text = tr("Redact &Text…"), .icon = QStringLiteral("eye-off"),
         .tip = tr("Remove every occurrence of a word or phrase from the file"), .certNeeds = 4, .group = tools},
        [this] {
            bool ok = false;
            const QString needle = QInputDialog::getText(
                this, tr("Redact text"), tr("Remove every occurrence of (case-insensitive):"),
                QLineEdit::Normal, QString(), &ok);
            if (!ok || needle.isEmpty()) {
                return;
            }
            if (signatureCount_ == 0 &&
                QMessageBox::warning(this, tr("Redact text"),
                                     tr("Every occurrence of “%1” will be removed from the file, not just "
                                        "covered. Once saved, it cannot be brought back.\n\nRedact it?")
                                         .arg(needle),
                                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
                return;
            }
            if (confirmBreakingSignatures(tr("A redaction"))) {
                onWorker([=](RenderWorker* w) { w->redactText(needle); });
            }
        });
    add({.id = QStringLiteral("applyRedactions"), .text = tr("&Apply Redactions…"), .icon = QStringLiteral("check"),
         .tip = tr("Remove everything under the marked areas from the file, for good"), .certNeeds = 4,
         .enabledWhen = [this] { return pageCount_ > 0 && certAllows(4) && !redactionMarks_.isEmpty(); },
         .group = tools},
        [this] { (void)applyRedactions(); });
    add({.id = QStringLiteral("clearRedactionMarks"), .text = tr("C&lear Redaction Marks"),
         .icon = QStringLiteral("x"), .tip = tr("Forget the marked areas; nothing is removed"), .certNeeds = 4,
         .enabledWhen = [this] { return !redactionMarks_.isEmpty(); }, .group = tools},
        [this] { setRedactionMarks({}); });
    add({.id = QStringLiteral("watermark"), .text = tr("&Watermark…"), .icon = QStringLiteral("droplets"),
         .tip = tr("Put text such as DRAFT across the pages"), .certNeeds = 4, .group = tools},
        [this] {
            const int page = std::max(0, view_->currentPage());
            WatermarkDialog dialog(this, view_->pageImage(page), view_->pageSizePoints(page),
                                   view_->pageCount());
            if (dialog.exec() == QDialog::Accepted) {
                const QString pages = dialog.pages();
                const leht::ops::WatermarkOptions options = dialog.options();
                onWorker([=](RenderWorker* w) { w->addWatermark(pages, options); });
            }
        });
    add({.id = QStringLiteral("recognizeText"), .text = tr("&Recognize Text (OCR)…"),
         .icon = QStringLiteral("scan-text"), .tip = tr("Make scanned pages searchable"), .certNeeds = 4,
         .group = tools},
        &MainWindow::recognizeText);
    add({.id = QStringLiteral("cropMargins"), .text = tr("Crop &Margins…"), .icon = QStringLiteral("crop"),
         .tip = tr("Trim the same margins from many pages"), .certNeeds = 4, .group = tools},
        [this] {
            CropMarginsDialog dialog(this, view_->pageCount());
            if (dialog.exec() == QDialog::Accepted) {
                const QString pages = dialog.pages();
                const leht::ops::Margins margins = dialog.margins();
                onWorker([=](RenderWorker* w) { w->cropMargins(pages, margins); });
            }
        });
    add({.id = QStringLiteral("signInvisibly"), .text = tr("Sign &Invisibly…"), .icon = QStringLiteral("file-pen-line"),
         .tip = tr("Sign the document without marking a page"), .certNeeds = 2, .group = sign},
        [this] { startSigning(0, QRectF()); });
    add({.id = QStringLiteral("addLongTermValidation"), .text = tr("Add &Long-Term Validation…"),
         .icon = QStringLiteral("history"),
         .tip = tr("Embed what every signature needs to be checked after its certificates expire "
                   "(PAdES B-LT), and a document timestamp over it (B-LTA)"),
         .enabledWhen = [this] { return pageCount_ > 0 && signatureCount_ > 0; }, .group = sign},
        &MainWindow::addLongTermValidation);
    add({.id = QStringLiteral("checkRevocation"), .text = tr("Check &Revocation Online"),
         .icon = QStringLiteral("globe"),
         .tip = tr("Ask the certificates' revocation services now whether they were revoked. Only "
                   "certificate identifiers are sent, never the document."),
         .enabledWhen = [this] { return pageCount_ > 0 && signatureCount_ > 0; }, .group = sign},
        [this] {
            statusBar()->showMessage(tr("Checking revocation online…"));
            onWorker([](RenderWorker* w) { w->checkRevocationOnline(); });
        });
    add({.id = QStringLiteral("updateTrustedList"), .text = tr("Update EU Trusted &Lists…"),
         .icon = QStringLiteral("badge-check"),
         .tip = tr("Fetch and verify the EU trusted lists, which make qualified CAs and timestamp "
                   "authorities trusted and say whether a signature is qualified"),
         .enabledWhen = [] { return true; }, .group = sign},
        &MainWindow::updateTrustedList);
    add({.id = QStringLiteral("trustedCertificates"), .text = tr("&Trusted Certificates…"),
         .icon = QStringLiteral("key-round"), .tip = tr("Certificates you trust besides your system's"),
         .enabledWhen = [] { return true; }, .group = sign},
        [this] { openPreferences(static_cast<int>(PreferencesDialog::Page::Trust)); });
}

void MainWindow::buildSignaturePanel() {
    // A banner rather than a dialog: a document's signatures are a standing
    // fact about it, not an event, and the one thing a reader must not have to
    // go looking for. On a row of its own, under the main toolbar.
    addToolBarBreak(Qt::TopToolBarArea);
    signatureBanner_ = new QToolBar(tr("Signatures"), this);
    signatureBanner_->setObjectName(QStringLiteral("signatureBanner"));
    signatureBanner_->setMovable(false);
    signatureBanner_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    signatureBannerIcon_ = new QLabel(signatureBanner_);
    signatureBannerIcon_->setContentsMargins(6, 0, 2, 0);
    signatureBanner_->addWidget(signatureBannerIcon_);
    signatureBannerLabel_ = new QLabel(signatureBanner_);
    signatureBannerLabel_->setTextFormat(Qt::PlainText);
    signatureBanner_->addWidget(signatureBannerLabel_);
    auto* bannerSpacer = new QWidget(signatureBanner_);
    bannerSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    signatureBanner_->addWidget(bannerSpacer);
    QAction* details = signatureBanner_->addAction(icons::named(QStringLiteral("shield-check")),
                                                   tr("Details"));
    addToolBar(Qt::TopToolBarArea, signatureBanner_);
    signatureBanner_->hide();

    signaturePanel_ = new QWidget(this);
    signaturePanel_->setObjectName(QStringLiteral("signaturePanel"));
    auto* column = new QVBoxLayout(signaturePanel_);
    column->setContentsMargins(4, 0, 4, 4);
    auto* online = new QPushButton(icons::named(QStringLiteral("globe")), tr("Check Revocation Online"),
                                   signaturePanel_);
    online->setObjectName(QStringLiteral("checkRevocationOnline"));
    online->setToolTip(tr("Ask the certificates' revocation services (OCSP, CRL) now whether "
                          "they were revoked. Only certificate identifiers are sent, never the "
                          "document, and nothing is added to it."));
    connect(online, &QPushButton::clicked, actions_->find(QStringLiteral("checkRevocation")),
            &QAction::trigger);
    // Cards first, in words; the full technical record folds out below.
    signatureCards_ = new SignatureCards(signaturePanel_);
    auto* cardScroll = new QScrollArea(signaturePanel_);
    cardScroll->setWidgetResizable(true);
    cardScroll->setFrameShape(QFrame::NoFrame);
    cardScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // cards wrap to the width
    auto* cardHolder = new QWidget(cardScroll);
    auto* cardColumn = new QVBoxLayout(cardHolder);
    cardColumn->setContentsMargins(0, 0, 0, 0);
    cardColumn->addWidget(signatureCards_);
    cardColumn->addStretch(1);
    cardScroll->setWidget(cardHolder);
    connect(signatureCards_, &SignatureCards::showPage, this, [this](int page) { view_->goToPage(page); });
    connect(signatureCards_, &SignatureCards::trustRequested, this,
            [this] { openPreferences(static_cast<int>(PreferencesDialog::Page::Trust)); });
    auto* detailsToggle = new QPushButton(icons::named(QStringLiteral("chevron-down")), tr("Technical details"),
                                          signaturePanel_);
    detailsToggle->setObjectName(QStringLiteral("signatureDetailsToggle"));
    detailsToggle->setCheckable(true);
    detailsToggle->setFlat(true);
    signatures_ = new QTreeWidget(signaturePanel_);
    signatures_->setObjectName(QStringLiteral("signatureTree"));
    signatures_->setHeaderLabels({tr("Signature"), tr("Details")});
    signatures_->setColumnWidth(0, 150);
    signatures_->hide();
    connect(detailsToggle, &QPushButton::toggled, this, [this, detailsToggle, cardScroll](bool on) {
        signatures_->setVisible(on);
        cardScroll->setMaximumHeight(on ? cardScroll->sizeHint().height() : QWIDGETSIZE_MAX);
        detailsToggle->setIcon(icons::named(on ? QStringLiteral("chevron-up") : QStringLiteral("chevron-down")));
    });
    column->addWidget(cardScroll, 1);
    column->addWidget(detailsToggle, 0, Qt::AlignLeft);
    column->addWidget(signatures_, 1);
    column->addWidget(online);
    connect(details, &QAction::triggered, this, [this] { sidebar_->showPanel(QStringLiteral("signatures")); });

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
        case leht::crypto::Trust::Revoked:     return MainWindow::tr("certificate revoked");
    }
    return MainWindow::tr("not checked");
}

/// One line saying what this signature is worth, and the colour to say it in.
std::pair<QString, QColor> verdict(const SigRow& row) {
    if (!row.rangeOk || !row.intact) {
        return {MainWindow::tr("Broken"), QColor(170, 20, 20)};
    }
    if (row.changesJudged && !row.changesPermitted) {
        return {MainWindow::tr("Intact, but changed in a way it forbids"), QColor(170, 20, 20)};
    }
    if (static_cast<leht::crypto::Trust>(row.trust) == leht::crypto::Trust::Revoked) {
        return {MainWindow::tr("Intact, but the certificate was revoked"), QColor(170, 20, 20)};
    }
    const bool trusted = static_cast<leht::crypto::Trust>(row.trust) ==
                         leht::crypto::Trust::Trusted;
    if (row.changedAfterSigning && !row.laterSignatureCoversChanges &&
        !row.onlyValidationDataAfter) {
        return {MainWindow::tr("Intact, but the document was changed afterwards"),
                QColor(170, 110, 0)};
    }
    if (!trusted) {
        return {row.documentTimestamp ? MainWindow::tr("Intact, authority not trusted")
                                      : MainWindow::tr("Intact, signer not trusted"),
                QColor(170, 110, 0)};
    }
    return {MainWindow::tr("Valid"), QColor(20, 120, 40)};
}

/// crypto::QualifiedReport::Level::Qes, as SigRow::qualified carries it.
constexpr int kQes = 2;

/// A signature's "Qualified" line: the EU trusted lists' verdict, and why.
QString qualifiedWords(const SigRow& row, bool haveTrustedList) {
    QString words;
    switch (row.qualified) {
        case 0:
            return haveTrustedList
                       ? MainWindow::tr("not checked")
                       : MainWindow::tr("not checked: no EU trusted lists yet (Sign → Update EU "
                                        "Trusted Lists…)");
        case 1: words = MainWindow::tr("not qualified"); break;
        case kQes: words = MainWindow::tr("qualified electronic signature (QES)"); break;
        case 3: words = MainWindow::tr("qualified electronic seal"); break;
        case 4: words = MainWindow::tr("advanced, with a qualified certificate"); break;
        default: return {};
    }
    if (!row.qualifiedDetail.isEmpty()) {
        words = MainWindow::tr("%1: %2").arg(words, row.qualifiedDetail);
    }
    return words;
}

QString certificationWords(int level) {
    switch (level) {
        case 1: return MainWindow::tr("no changes allowed");
        case 2: return MainWindow::tr("form filling and signing allowed");
        case 3: return MainWindow::tr("form filling, signing and comments allowed");
        default: return {};
    }
}

QString localTime(qint64 unix_seconds) {
    return QDateTime::fromSecsSinceEpoch(unix_seconds).toString(Qt::ISODate);
}

}  // namespace

void MainWindow::onSignaturesReady(const QVector<SigRow>& rows) {
    signatureCount_ = static_cast<int>(rows.size());
    signatures_->clear();
    signatureCards_->setRows({}, true);
    sidebar_->setPanelAvailable(QStringLiteral("signatures"), !rows.isEmpty());
    const bool verifyAsked = std::exchange(verifyPending_, false);
    if (rows.isEmpty()) {
        applyCertification(0);  // also refreshes the actions that need a signature
        signatureBanner_->hide();
        if (verifyAsked) {
            QMessageBox::information(this, tr("Check signatures"),
                                     tr("“%1” is not signed.").arg(currentTitle_));
        }
        return;
    }

    const RenderWorker::TrustedListState lists = RenderWorker::trustedListState();
    const bool haveTrustedList = lists.present;
    signatureCards_->setRows(rows, haveTrustedList);
    int worst = 0;  // 0 valid, 1 a warning, 2 broken
    for (const SigRow& row : rows) {
        const auto [word, colour] = verdict(row);
        auto* item = new QTreeWidgetItem(signatures_);
        item->setText(0, row.documentTimestamp ? tr("Document timestamp")
                         : row.signerCommonName.isEmpty() ? row.field
                                                          : row.signerCommonName);
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
        if (row.intact && !row.documentTimestamp) {
            add(tr("Qualified"), qualifiedWords(row, haveTrustedList));
        }
        if (row.notAfter != 0) {
            add(tr("Certificate valid until"), localTime(row.notAfter));
        }
        add(tr("Algorithm"), row.digest.isEmpty() ? row.subfilter
                                                  : tr("%1, %2").arg(row.digest, row.subfilter));
        if (row.intact && !row.documentTimestamp) {
            if (row.revocation.isEmpty()) {
                add(tr("Revocation"), tr("not checked: nothing embedded (Check Revocation "
                                         "Online asks now)"));
            }
            for (const QString& line : row.revocation) {
                add(tr("Revocation"), line);
            }
        }
        add(tr("Claimed time"), row.claimedTime);
        if (row.documentTimestamp) {
            if (row.intact) {
                add(tr("Proves"), tr("the file up to here existed at %1")
                                      .arg(localTime(row.timestampTime)));
                add(tr("Authority"), tr("%1 (%2)").arg(row.authority,
                                                        trustWord(row.timestampTrust)));
                for (const QString& line : row.revocation) {
                    add(tr("Revocation"), line);
                }
                const bool unknown = std::any_of(
                    row.revocation.begin(), row.revocation.end(),
                    [](const QString& l) { return l.contains(QStringLiteral(": unknown")); });
                if (unknown && !row.changedAfterSigning) {
                    add(tr("Note"), tr("the newest document timestamp's own validation data "
                                       "comes with the next Add Long-Term Validation"));
                }
            }
        } else if (row.hasTimestamp) {
            add(tr("Timestamp"),
                row.timestampValid
                    ? tr("%1, by %2 (%3)").arg(localTime(row.timestampTime), row.authority,
                                               row.timestampQualified
                                                   ? tr("%1, qualified").arg(trustWord(row.timestampTrust))
                                                   : trustWord(row.timestampTrust))
                    : tr("not valid: %1").arg(row.timestampProblem));
            for (const QString& line : row.timestampRevocation) {
                add(tr("Authority revocation"), line);
            }
        } else if (row.intact) {
            add(tr("Timestamp"), tr("none: nothing proves when this was signed"));
        }
        add(tr("Reason"), row.reason);
        add(tr("Location"), row.location);
        if (row.certification != 0) {
            add(tr("Certifies"), certificationWords(row.certification));
        }
        add(tr("Locks"), row.locks);
        if (row.changesJudged) {
            add(tr("Later changes"), row.changesPermitted
                                         ? tr("all of them allowed")
                                         : row.changeProblems.join(QStringLiteral("; ")));
        }
        if (row.changedAfterSigning && row.onlyValidationDataAfter) {
            add(tr("Afterwards"), tr("validation data was added; it changes nothing signed"));
        } else if (row.changedAfterSigning) {
            add(tr("Changed"), row.laterSignatureCoversChanges
                                   ? tr("yes, and a later signature covers those changes")
                                   : tr("yes: the document was added to after this signature"));
        }
        add(tr("Certificate fingerprint"), row.fingerprint);

        if (!row.rangeOk || !row.intact || (row.changesJudged && !row.changesPermitted) ||
            static_cast<leht::crypto::Trust>(row.trust) == leht::crypto::Trust::Revoked) {
            worst = 2;
        } else if (worst < 1 && (row.changedAfterSigning && !row.laterSignatureCoversChanges &&
                                 !row.onlyValidationDataAfter)) {
            worst = 1;
        } else if (worst < 1 && static_cast<leht::crypto::Trust>(row.trust) !=
                                    leht::crypto::Trust::Trusted) {
            worst = 1;
        }
    }
    signatures_->expandAll();

    const QString summary =
        worst == 2 ? tr("This document has a broken signature.")
        : worst == 1 ? tr("This document is signed, with something worth checking.")
                     : tr("Signed and verified.");
    int certified = 0;
    for (const SigRow& row : rows) {
        certified = row.certification != 0 ? row.certification : certified;
    }
    // A document timestamp is counted apart: nobody signed it.
    const int stamps = static_cast<int>(std::count_if(
        rows.begin(), rows.end(), [](const SigRow& r) { return r.documentTimestamp; }));
    const QString counted =
        stamps == 0 ? tr("%n signature(s)", nullptr, signatureCount_)
                    : tr("%n signature(s)", nullptr, signatureCount_ - stamps) + QStringLiteral(", ") +
                          tr("%n document timestamp(s)", nullptr, stamps);
    // Qualified when every signature is a QES, as the EU trusted lists say.
    const bool allQes = std::all_of(rows.begin(), rows.end(), [](const SigRow& r) {
        return r.documentTimestamp || r.qualified == kQes;
    });
    signatureBannerLabel_->setText(
        tr(" %1  (%2) ").arg(summary, counted) +
        (allQes && stamps < signatureCount_ ? tr(" Qualified electronic signature. ") : QString()) +
        (certified != 0 ? tr(" Certified: %1. ").arg(certificationWords(certified)) : QString()) +
        (lists.overdue ? tr(" The EU trusted lists are overdue for an update. ") : QString()));
    applyCertification(certified);
    // The verdict in a coloured mark and a faint wash of the same colour; the
    // text keeps the palette's, so it reads in light and dark themes alike.
    const QColor colour = worst == 2 ? QColor(200, 30, 30) : worst == 1 ? QColor(210, 130, 0) : QColor(30, 150, 60);
    signatureBannerIcon_->setPixmap(icons::tinted(
        worst == 2 ? QStringLiteral("shield-x") : worst == 1 ? QStringLiteral("shield-alert")
                                                             : QStringLiteral("shield-check"),
        colour, 18, devicePixelRatioF()));
    signatureBanner_->setStyleSheet(
        QStringLiteral("QToolBar#signatureBanner { background: rgba(%1, %2, %3, 38); border: none;"
                       " border-bottom: 1px solid rgba(%1, %2, %3, 110); padding: 2px; }")
            .arg(colour.red()).arg(colour.green()).arg(colour.blue()));
    signatureBanner_->show();
    if (worst > 0 || verifyAsked) {
        sidebar_->showPanel(QStringLiteral("signatures"));
    }
}

void MainWindow::applyCertification(int level) {
    // What a certified document still lets one do: each tool and command
    // carries the level from which it is allowed (2 form filling and signing,
    // 3 annotations, 4 never -- page content is fixed at every level), and the
    // registry's enabledWhen asks certAllows(). Leht does not break a
    // certification by a click.
    certLevel_ = level;
    for (QAction* a : findChildren<QAction*>()) {
        const int needs = a->property("certNeeds").toInt();
        if (needs == 0) {
            continue;
        }
        const QString base = a->property("baseTip").toString();
        a->setToolTip(certAllows(needs) ? base
                                        : tr("%1\n\nNot available: the document is certified, %2.")
                                              .arg(base, certificationWords(level)));
    }
    actions_->refresh();
    // Filling fields is the one change level 2 and 3 allow and 1 does not.
    if (fields_ != nullptr) {
        fields_->setEnabled(level != 1);
    }
    if (comments_ != nullptr) {
        comments_->setEditable(certAllows(3));
    }
    if (modes_ != nullptr) {
        const QString why = tr("Not available: the document is certified, %1.").arg(certificationWords(level));
        modes_->setModeEnabled(QStringLiteral("comment"), certAllows(3), why);
        modes_->setModeEnabled(QStringLiteral("sign"), certAllows(2), why);
        modes_->setModeEnabled(QStringLiteral("pages"), certAllows(4), why);
        modes_->setModeEnabled(QStringLiteral("redact"), certAllows(4), why);
    }
    if (tools_ != nullptr) {
        QAction* current = tools_->checkedAction();
        if (current != nullptr && !current->isEnabled() && pageCount_ > 0) {
            tools_->actions().first()->trigger();  // back to Select
        }
    }
}

void MainWindow::setRedactionMarks(QVector<QPair<int, QRectF>> marks) {
    redactionMarks_ = std::move(marks);
    view_->setRedactionMarks(redactionMarks_);
    actions_->refresh();
}

bool MainWindow::applyRedactions(bool confirm) {
    if (redactionMarks_.isEmpty() || pageCount_ <= 0) {
        return false;
    }
    if (confirm) {
        QSet<int> pages;
        for (const auto& mark : redactionMarks_) {
            pages.insert(mark.first);
        }
        const auto answer = QMessageBox::warning(
            this, tr("Apply redactions"),
            tr("Everything under %n marked area(s)", nullptr, static_cast<int>(redactionMarks_.size())) +
                tr(" on %n page(s) will be removed from the file: text, pictures and drawing, not just "
                   "covered.\n\nUndo can bring it back until you save; after that it is gone.",
                   nullptr, static_cast<int>(pages.size())),
            QMessageBox::Apply | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Apply || !confirmBreakingSignatures(tr("A redaction"))) {
            return false;
        }
    }
    const auto marks = std::exchange(redactionMarks_, {});
    view_->setRedactionMarks({});
    actions_->refresh();
    // One edit group: one Undo takes back the lot.
    onWorker([marks](RenderWorker* w) {
        w->beginEditGroup();
        for (const auto& [page, box] : marks) {
            w->redactArea(page, box);
        }
        w->endEditGroup();
    });
    return true;
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
    SignDialog dialog(this, page, rect, QString(), signatureCount_ == 0);
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

void MainWindow::updateTrustedList() {
    const auto answer = QMessageBox::question(
        this, tr("Update EU Trusted Lists"),
        tr("Leht will download the European Commission's List of Trusted Lists and the national "
           "lists it points to — about 30 servers, some 28 MB — and verify each by its "
           "signature. Nothing about you or your documents is sent.\n\nUpdate now?"));
    if (answer != QMessageBox::Yes) {
        return;
    }
    statusBar()->showMessage(tr("Updating the EU trusted lists…"));
    onWorker([](RenderWorker* w) { w->updateTrustedList(); });
}

void MainWindow::addLongTermValidation() {
    if (pageCount_ == 0 || signatureCount_ == 0) {
        return;
    }
    if (modified_) {
        QMessageBox::information(
            this, tr("Add Long-Term Validation"),
            tr("Save or undo your changes first. Validation data goes in a revision of its "
               "own, which must add nothing else."));
        return;
    }
    QSettings settings;
    bool ok = false;
    const QString tsa = QInputDialog::getText(
        this, tr("Add Long-Term Validation"),
        tr("Leht will ask each signing certificate's revocation services (OCSP, CRL) over the "
           "network whether it was valid, and embed the answers. Only certificate identifiers "
           "are sent, never the document.\n\nA timestamp authority, to add a document "
           "timestamp over it all (leave empty to skip):"),
        QLineEdit::Normal, settings.value(QStringLiteral("signing/tsa")).toString(), &ok);
    if (!ok) {
        return;
    }
    if (!tsa.trimmed().isEmpty()) {
        settings.setValue(QStringLiteral("signing/tsa"), tsa.trimmed());
    }
    const QString target = currentPath_;
    if (target.isEmpty()) {
        return;
    }
    statusBar()->showMessage(tr("Adding long-term validation data…"));
    onWorker([=](RenderWorker* w) { w->addLongTermValidation(target, tsa); });
}

void MainWindow::onEditStateChanged(bool canUndo, bool canRedo, bool modified) {
    undoAction_->setEnabled(canUndo);
    redoAction_->setEnabled(canRedo);
    modified_ = modified;
    actions_->refresh();
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
    sidebar_->setPanelAvailable(QStringLiteral("form"), !rows.isEmpty());
    // A form opens on its fields, once: after that the sidebar is the user's.
    if (!rows.isEmpty() && !std::exchange(formShown_, true)) {
        sidebar_->showPanel(QStringLiteral("form"));
    }
}

bool MainWindow::settlePendingRedactions() {
    if (redactionMarks_.isEmpty()) {
        return true;
    }
    // Saving with marks still pending would write a file that looks redacted
    // and is not: ask, every time.
    QMessageBox box(QMessageBox::Warning, tr("Redactions not applied"),
                    tr("%n area(s) are marked for redaction but not yet removed. Saving now keeps "
                       "everything under them in the file.",
                       nullptr, static_cast<int>(redactionMarks_.size())),
                    QMessageBox::Cancel, this);
    QPushButton* apply = box.addButton(tr("Apply and Save"), QMessageBox::AcceptRole);
    QPushButton* without = box.addButton(tr("Save Without Applying"), QMessageBox::DestructiveRole);
    box.setDefaultButton(apply);
    box.exec();
    if (box.clickedButton() == apply) {
        return applyRedactions(/*confirm=*/false);  // queued before the save, so saved with it
    }
    return box.clickedButton() == without;
}

bool MainWindow::save() {
    if (pageCount_ <= 0 || !settlePendingRedactions()) {
        return false;
    }
    if (currentPath_.isEmpty() || !currentPath_.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        return saveToChosenPath();  // an image or XPS opened for viewing is not written back as PDF in place
    }
    statusBar()->showMessage(tr("Saving…"));
    const QString path = currentPath_;
    onWorker([=](RenderWorker* w) { w->save(path); });
    return true;
}

bool MainWindow::saveAs() {
    if (pageCount_ <= 0 || !settlePendingRedactions()) {
        return false;
    }
    return saveToChosenPath();
}

bool MainWindow::saveToChosenPath() {
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

QString MainWindow::askOpenPath() {
    return QFileDialog::getOpenFileName(this, tr("Open PDF"),
                                        currentPath_.isEmpty() ? QString() : QFileInfo(currentPath_).absolutePath(),
                                        tr("PDF documents (*.pdf);;All files (*)"));
}

void MainWindow::openDialog() {
    const QString path = askOpenPath();
    if (!path.isEmpty()) {
        openPath(path);
    }
}

void MainWindow::openPath(const QString& path) {
    if (!resolveUnsaved([this, path] { openPath(path); })) {
        return;
    }
    modified_ = false;
    redactionMarks_.clear();  // the view forgets its copy in clear()
    pageCount_ = 0;  // nothing to act on until it opens
    signatureCount_ = 0;
    documentEncrypted_ = false;
    formShown_ = false;
    currentPath_ = QFileInfo(path).absoluteFilePath();
    currentTitle_ = QFileInfo(path).fileName();
    (void)view_->setTool(PageView::Tool::Select);
    if (tools_ != nullptr) {
        tools_->actions().first()->setChecked(true);
    }
    if (modes_ != nullptr && pendingTask_.isEmpty()) {
        modes_->setMode(QStringLiteral("read"));
    }
    statusBar()->showMessage(tr("Opening %1…").arg(currentTitle_));
    view_->clear();
    if (thumbnails_ != nullptr) {
        thumbnails_->clearThumbnails();
    }
    outlineTree_->clear();
    comments_->setAnnotations({});
    for (const char* panel : {"outline", "form", "signatures"}) {
        sidebar_->setPanelAvailable(QLatin1String(panel), false);
    }
    signatureBanner_->hide();
    certLevel_ = 0;
    undoAction_->setEnabled(false);  // the worker has no log for the new document yet
    redoAction_->setEnabled(false);
    actions_->refresh();
    stack_->setCurrentWidget(documentPage_);
    emit requestOpen(path);
}

void MainWindow::onOpened(int pageCount, QVector<QSize> baseSizes) {
    pageCount_ = pageCount;
    updateTitle();
    undoAction_->setEnabled(false);
    redoAction_->setEnabled(false);
    signatureCount_ = 0;
    applyCertification(0);  // refreshes every action for the open document
    onWorker([](RenderWorker* w) {
        w->listAnnotations();
        w->listFields();
        w->listSignatures();
    });
    statusBar()->clearMessage();
    stack_->setCurrentWidget(documentPage_);
    view_->setPages(baseSizes);
    const QString zoom = QSettings().value(QLatin1String(prefs::kDefaultZoom), QStringLiteral("width")).toString();
    if (zoom == QLatin1String("page")) {
        view_->fitPage();
    } else if (zoom == QLatin1String("actual")) {
        view_->setZoom(1.0);
    } else {
        view_->fitWidth();
    }
    pageSpin_->setMaximum(qMax(1, pageCount));
    pageSpin_->setEnabled(pageCount > 0);
    thumbnails_->setPageCount(pageCount);
    view_->setFocus();
    onCurrentPageChanged(view_->currentPage());
    updateZoomLabel();
    if (!currentPath_.isEmpty()) {
        recent::add(currentPath_);
    }
    if (!pendingTask_.isEmpty()) {
        QTimer::singleShot(0, this, &MainWindow::runPendingTask);
    }
}

void MainWindow::onFailed(const QString& message) {
    statusBar()->clearMessage();
    pendingTask_.clear();
    verifyPending_ = false;
    QMessageBox::warning(this, tr("Could not open document"), message);
    if (pageCount_ == 0) {
        currentPath_.clear();
        currentTitle_.clear();
        updateTitle();
        showWelcome();
    }
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
    sidebar_->setPanelAvailable(QStringLiteral("outline"), !rows.isEmpty());
    if (rows.isEmpty()) {
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
    documentEncrypted_ = true;
    bool ok = false;
    const QString prompt =
        retry ? tr("Wrong password. Try again for “%1”:").arg(currentTitle_)
              : tr("“%1” is password-protected. Enter its password:")
                    .arg(currentTitle_);
    const QString password = QInputDialog::getText(
        this, tr("Password required"), prompt, QLineEdit::Password, QString(), &ok);

    if (!ok) {
        // User cancelled: nothing is open, so back to the start.
        statusBar()->showMessage(tr("Opening cancelled."), 3000);
        pendingTask_.clear();
        currentPath_.clear();
        currentTitle_.clear();
        updateTitle();
        showWelcome();
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

// --- File tools: Combine Files, Reduce File Size, Split Document ----------------

void MainWindow::buildFileTools() {
    fileTools_ = new FileTools();
    fileTools_->moveToThread(&fileToolsThread_);
    connect(&fileToolsThread_, &QThread::finished, fileTools_, &QObject::deleteLater);
    fileToolsThread_.start();

    // One job at a time: all three wait while one runs.
    const QString file = tr("File");
    QAction* combine = actions_->add({.id = QStringLiteral("combineFiles"), .text = tr("Co&mbine Files…"),
                                      .icon = QStringLiteral("combine"),
                                      .tip = tr("Put PDFs and images together into one new PDF"),
                                      .enabledWhen = [this] { return !fileToolBusy_; }, .group = file});
    QAction* reduce = actions_->add({.id = QStringLiteral("reduceFileSize"), .text = tr("Re&duce File Size…"),
                                     .icon = QStringLiteral("minimize-2"),
                                     .tip = tr("Make a smaller copy of this document"),
                                     .enabledWhen = [this] { return pageCount_ > 0 && !fileToolBusy_; },
                                     .group = file});
    QAction* split = actions_->add({.id = QStringLiteral("splitDocument"), .text = tr("Sp&lit Document…"),
                                    .icon = QStringLiteral("scissors"),
                                    .tip = tr("Write this document's pages into separate files"),
                                    .enabledWhen = [this] { return pageCount_ > 0 && !fileToolBusy_; },
                                    .group = file});

    connect(combine, &QAction::triggered, this, [this] {
        combineFiles(pageCount_ > 0 ? QStringList{currentPath_} : QStringList{});
    });

    // Password protection: a new file, by default beside this one. Like
    // Reduce and Split it reads the file on disk, so edits are settled first.
    const auto isPdf = [this] { return currentPath_.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive); };
    QAction* protect = actions_->add({.id = QStringLiteral("protect"), .text = tr("Password &Protect…"),
                                      .icon = QStringLiteral("lock"),
                                      .tip = tr("Save a copy that asks for a password to open"),
                                      .enabledWhen = [this, isPdf] { return pageCount_ > 0 && isPdf() && !fileToolBusy_; },
                                      .group = file});
    QAction* unprotect = actions_->add({.id = QStringLiteral("unprotect"), .text = tr("Remove Pass&word…"),
                                        .icon = QStringLiteral("lock-open"),
                                        .tip = tr("Save a copy that opens without a password"),
                                        .enabledWhen = [this, isPdf] {
                                            return pageCount_ > 0 && isPdf() && documentEncrypted_ && !fileToolBusy_;
                                        },
                                        .group = file});
    const auto target = [this](const QString& suffix) {
        const QFileInfo info(currentPath_);
        return QFileDialog::getSaveFileName(
            this, tr("Save as"), info.dir().filePath(tr("%1 (%2).pdf").arg(info.completeBaseName(), suffix)),
            tr("PDF documents (*.pdf)"));
    };
    connect(protect, &QAction::triggered, this, [this, protect, target] {
        if (pageCount_ <= 0 || !resolveUnsaved([protect] { protect->trigger(); })) {
            return;
        }
        ProtectDialog dialog(this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString output = target(tr("protected"));
        if (output.isEmpty()) {
            return;
        }
        const QString input = currentPath_;
        const QString user = dialog.openPassword();
        const QString owner = dialog.permissionsPassword();
        const int method = dialog.method();
        const int permissions = dialog.permissions();
        runFileTool(tr("Protecting…"), [=](FileTools* t, const QString& password) {
            t->protect(input, password, output, true, user, owner, method, permissions);
        });
    });
    connect(unprotect, &QAction::triggered, this, [this, unprotect, target] {
        if (pageCount_ <= 0 || !resolveUnsaved([unprotect] { unprotect->trigger(); })) {
            return;
        }
        const QString output = target(tr("no password"));
        if (output.isEmpty()) {
            return;
        }
        const QString input = currentPath_;
        // The job asks for the current password itself: Leht does not keep it.
        runFileTool(tr("Removing the password…"), [=](FileTools* t, const QString& password) {
            t->protect(input, password, output, false, {}, {}, 2, 0x7F);
        });
    });

    // Reduce and Split read the file on disk, so unsaved edits are settled
    // first; the dialogs then describe the file that will actually be used.
    connect(reduce, &QAction::triggered, this, [this, reduce] {
        if (pageCount_ <= 0 || !resolveUnsaved([reduce] { reduce->trigger(); })) {
            return;
        }
        ReduceDialog dialog(this, currentPath_, signatureCount_ > 0);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString input = currentPath_;
        const QString output = dialog.output();
        const int preset = dialog.preset();
        runFileTool(tr("Reducing file size…"),
                    [input, output, preset](FileTools* t, const QString& password) {
                        t->compress(input, password, output, preset, 0, false);
                    });
    });

    connect(split, &QAction::triggered, this, [this, split] {
        if (pageCount_ <= 0 || !resolveUnsaved([split] { split->trigger(); })) {
            return;
        }
        SplitDialog dialog(this, currentPath_, pageCount_, signatureCount_ > 0);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString input = currentPath_;
        const QStringList ranges = dialog.ranges();
        const QStringList outputs = dialog.outputs();
        runFileTool(tr("Splitting document…"),
                    [input, ranges, outputs](FileTools* t, const QString& password) {
                        t->split(input, password, ranges, outputs);
                    });
    });

    connect(fileTools_, &FileTools::progress, this, [this](int done, int total, const QString& what) {
        if (fileToolsProgress_ != nullptr) {
            fileToolsProgress_->setMaximum(total);
            fileToolsProgress_->setValue(done);
            fileToolsProgress_->setLabelText(what);
        }
    });
    connect(fileTools_, &FileTools::failed, this, [this](const QString& message) {
        const QString title = fileToolTitle_;
        endFileTool();
        QMessageBox::warning(this, title, message);
    });
    connect(fileTools_, &FileTools::finished, this,
            [this](const QString& summary, const QStringList& written) {
                const QString title = fileToolTitle_;
                endFileTool();
                statusBar()->showMessage(summary, 8000);
                QMessageBox box(QMessageBox::Information, title, summary, QMessageBox::Close, this);
                QPushButton* open = nullptr;
                if (written.size() == 1) {
                    open = box.addButton(tr("Open It"), QMessageBox::AcceptRole);
                }
                box.exec();
                if (open != nullptr && box.clickedButton() == open) {
                    openPath(written.first());
                }
            });
    connect(fileTools_, &FileTools::passwordRequired, this, [this](bool wrong) {
        if (fileToolsProgress_ != nullptr) {
            fileToolsProgress_->hide();
        }
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, tr("Password required"),
            wrong ? tr("Wrong password. Try again for “%1”:").arg(currentTitle_)
                  : tr("“%1” is password-protected. Enter its password:").arg(currentTitle_),
            QLineEdit::Password, QString(), &ok);
        if (!ok || !fileToolJob_) {
            endFileTool();
            return;
        }
        if (fileToolsProgress_ != nullptr) {
            fileToolsProgress_->show();
        }
        FileTools* tools = fileTools_;
        QMetaObject::invokeMethod(
            tools, [tools, job = fileToolJob_, password] { job(tools, password); },
            Qt::QueuedConnection);
    });
}

void MainWindow::runFileTool(const QString& title, std::function<void(FileTools*, QString)> job) {
    fileToolTitle_ = QString(title).remove(QStringLiteral("…"));
    fileToolJob_ = std::move(job);
    fileToolBusy_ = true;
    actions_->refresh();

    fileToolsProgress_ = new QProgressDialog(title, tr("Cancel"), 0, 0, this);
    fileToolsProgress_->setObjectName(QStringLiteral("fileToolsProgress"));
    fileToolsProgress_->setWindowTitle(fileToolTitle_);
    fileToolsProgress_->setWindowModality(Qt::WindowModal);
    fileToolsProgress_->setMinimumDuration(400);
    fileToolsProgress_->setAutoClose(false);
    fileToolsProgress_->setAutoReset(false);
    connect(fileToolsProgress_, &QProgressDialog::canceled, this, [this] {
        fileTools_->cancel();  // thread-safe; the job stops after its current step
        if (fileToolsProgress_ != nullptr) {
            fileToolsProgress_->setLabelText(tr("Stopping…"));
        }
    });

    FileTools* tools = fileTools_;
    QMetaObject::invokeMethod(
        tools, [tools, job = fileToolJob_] { job(tools, QString()); }, Qt::QueuedConnection);
}

void MainWindow::endFileTool() {
    if (fileToolsProgress_ != nullptr) {
        fileToolsProgress_->deleteLater();
        fileToolsProgress_ = nullptr;
    }
    fileToolJob_ = nullptr;
    fileToolBusy_ = false;
    actions_->refresh();
}
