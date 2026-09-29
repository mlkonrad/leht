// SPDX-License-Identifier: AGPL-3.0-or-later
#include "document_tab.hpp"

#include "comments_panel.hpp"
#include "contrast.hpp"
#include "export_dialog.hpp"
#include "form_panel.hpp"
#include "icons.hpp"
#include "page_dialogs.hpp"
#include "page_grid.hpp"
#include "page_view.hpp"
#include "page_view_accessible.hpp"
#include "preferences.hpp"
#include "properties_dialog.hpp"
#include "render_worker.hpp"
#include "sidebar.hpp"
#include "sign_dialog.hpp"
#include "signature_cards.hpp"
#include "thumbnail_bar.hpp"
#include "leht/crypto/crypto.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/forms.hpp"
#ifdef LEHT_HAVE_OCR
#include "leht/ocr/ocr.hpp"
#endif

#include <QAccessible>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

DocumentTab::DocumentTab(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("documentTab"));
    view_ = new PageView(this);
    sidebar_ = new Sidebar(this);

    // The worker lives on its own thread; everything MuPDF happens there.
    worker_ = new RenderWorker();
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    wireWorker();
    workerThread_.start();

    buildSignaturePanel();
    buildPanels();
}

DocumentTab::~DocumentTab() {
    // Whatever the worker is waiting on stops first, or closing the tab
    // would wait with it: these reach it directly, not through its queue.
    worker_->cancelSearch();
    worker_->cancelRecognition();
    workerThread_.quit();
    workerThread_.wait();
}

void DocumentTab::onWorker(std::function<void(RenderWorker*)> fn) {
    RenderWorker* w = worker_;
    QMetaObject::invokeMethod(w, [w, fn = std::move(fn)] { fn(w); }, Qt::QueuedConnection);
}

bool DocumentTab::certAllows(int needs) const {
    return needs == 0 || certLevel_ == 0 || certLevel_ >= needs;
}

QString DocumentTab::tabText() const {
    return currentTitle_ + (modified_ ? QStringLiteral(" *") : QString());
}

int DocumentTab::currentPage() const { return view_->currentPage(); }

bool DocumentTab::isShowingGrid() const { return viewStack_->currentWidget() == pageGrid_; }

QWidget* DocumentTab::pageArea() const { return viewStack_->currentWidget(); }

QColor DocumentTab::toolColor(const QString& toolId) {
    static const QHash<QString, QColor> defaults = {
        {QStringLiteral("toolHighlight"), QColor(255, 220, 0)},
        {QStringLiteral("toolUnderline"), QColor(20, 90, 200)},
        {QStringLiteral("toolStrike"), QColor(200, 30, 30)},
        {QStringLiteral("toolDraw"), QColor(20, 90, 200)},
        {QStringLiteral("toolText"), QColor(0, 0, 0)},
    };
    const QColor chosen(QSettings().value(QStringLiteral("toolColors/") + toolId).toString());
    return chosen.isValid() ? chosen : defaults.value(toolId, QColor(0, 0, 0));
}

void DocumentTab::wireWorker() {
    // GUI -> worker (queued across the thread boundary).
    connect(this, &DocumentTab::requestOpen, worker_, &RenderWorker::open);
    connect(view_, &PageView::needRender, worker_, &RenderWorker::render);
    // Context `this`, not worker_: setGeneration must run on the GUI thread the
    // moment the view moves. Queued onto the worker thread it would wait behind
    // the very renders it is meant to make stale.
    connect(view_, &PageView::generationChanged, this,
            [this](quint64 gen) { worker_->setGeneration(gen); });

    // worker -> GUI.
    connect(worker_, &RenderWorker::opened, this, &DocumentTab::onOpened);
    connect(worker_, &RenderWorker::outlineReady, this, &DocumentTab::onOutlineReady);
    connect(worker_, &RenderWorker::passwordRequired, this, &DocumentTab::onPasswordRequired);
    connect(this, &DocumentTab::requestAuthenticate, worker_, &RenderWorker::authenticate);
    connect(worker_, &RenderWorker::failed, this, &DocumentTab::onFailed);
    connect(worker_, &RenderWorker::rendered, view_, &PageView::onRendered);
    connect(worker_, &RenderWorker::pageFailed, view_, &PageView::markPageFailed);

    // Find: GUI -> worker search, worker -> view highlights.
    connect(this, &DocumentTab::requestSearch, worker_, &RenderWorker::search);
    connect(worker_, &RenderWorker::searchStarted, view_, &PageView::clearMatches);
    connect(worker_, &RenderWorker::pageMatches, view_, &PageView::addMatches);
    connect(worker_, &RenderWorker::searchFinished, view_, &PageView::finishMatches);
    connect(view_, &PageView::matchNavigated, this, &DocumentTab::matchNavigated);

    // Selection: view -> worker request, worker -> view highlight + text.
    connect(view_, &PageView::selectRequested, worker_, &RenderWorker::selectRegion);
    connect(worker_, &RenderWorker::selectionReady, view_, &PageView::setSelection);

    // Thumbnails: bar -> worker request, worker -> bar image, bar -> navigation.
    connect(worker_, &RenderWorker::thumbnailReady, this, [this](int page, const QImage& img) {
        if (thumbnails_ != nullptr) {
            thumbnails_->onThumbnail(page, img);
        }
        if (pageGrid_ != nullptr) {
            pageGrid_->onThumbnail(page, img);
        }
    });

    connect(view_, &PageView::currentPageChanged, this, [this](int page) {
        if (pageCount_ > 0 && page >= 0 && thumbnails_ != nullptr) {
            thumbnails_->setCurrentPageQuiet(page);
        }
        emit pageChanged(page);
    });
    // A screen reader reads the page shown; its words are fetched only while
    // one is listening.
    pagereading::install();
    const auto readCurrentPage = [this] {
        const int page = view_->currentPage();
        if (QAccessible::isActive() && page >= 0) {
            onWorker([page](RenderWorker* w) { w->readPageText(page); });
        }
    };
    connect(view_, &PageView::currentPageChanged, this, readCurrentPage);
    connect(worker_, &RenderWorker::documentEdited, this, readCurrentPage);
    connect(worker_, &RenderWorker::pageTextRead, this,
            [this](int page, const QString& text) { pagereading::setPageText(view_, page, text); });

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
            emit stateChanged();
            emit pageChanged(view_->currentPage());
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
    connect(worker_, &RenderWorker::fieldsReady, this, &DocumentTab::onFieldsReady);
    connect(worker_, &RenderWorker::editStateChanged, this, &DocumentTab::onEditStateChanged);
    connect(worker_, &RenderWorker::saved, this, &DocumentTab::onSaved);
    // Every network contact says where it goes, as it goes.
    connect(worker_, &RenderWorker::networkUsed, this, [this](const QString& hosts) {
        say(tr("Contacting %1 (certificate identifiers only, never the document)…").arg(hosts));
    });
    connect(worker_, &RenderWorker::longTermValidationAdded, this,
            [this](int certs, int ocsps, int crls, qint64 timestamp) {
                QString what = tr("Embedded %n certificate(s)", nullptr, certs) +
                               tr(", %n OCSP response(s)", nullptr, ocsps) +
                               tr(" and %n CRL(s)", nullptr, crls);
                if (timestamp != 0) {
                    what += tr(", then a document timestamp");
                }
                say(what + QStringLiteral("."), 8000);
            });
    connect(worker_, &RenderWorker::saveFailed, this, [this](const QString& why) {
        afterSave_ = nullptr;
        say({});
        QMessageBox::warning(window(), tr("Could not save"), why);
    });
    connect(worker_, &RenderWorker::editFailed, this, [this](const QString& why) {
        QMessageBox::warning(window(), tr("Could not make that change"), why);
    });
    connect(worker_, &RenderWorker::redactionIncomplete, this, [this](const QStringList& where) {
        QMessageBox::warning(
            window(), tr("Redaction incomplete"),
            tr("The text was removed from the pages, but it still appears here:\n\n• %1\n\n"
               "Leht does not change these on its own. Review them before sharing the file.")
                .arg(where.join(QStringLiteral("\n• "))));
    });
    connect(view_, &PageView::highlightRequested, this,
            [this](int page, const QVector<QRectF>& boxes) {
                const QColor color = toolColor(QStringLiteral("toolHighlight"));
                onWorker([=](RenderWorker* w) { w->addHighlight(page, boxes, color); });
            });
    connect(view_, &PageView::noteRequested, this, [this](int page, QPointF at) {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(window(), tr("Add note"), tr("Note:"),
                                                            QString(), &ok);
        if (ok && !text.isEmpty()) {
            onWorker([=](RenderWorker* w) { w->addNote(page, at, text); });
        }
    });
    connect(view_, &PageView::inkRequested, this,
            [this](int page, const QVector<QPolygonF>& strokes) {
                const QColor color = toolColor(QStringLiteral("toolDraw"));
                onWorker([=](RenderWorker* w) { w->addInk(page, strokes, color); });
            });
    // The Redact tool marks; Apply Redactions removes. Nothing leaves the
    // file until the marks have been looked over.
    connect(view_, &PageView::redactRequested, this, [this](int page, QRectF box) {
        auto marks = redactionMarks_;
        marks.push_back({page, box});
        setRedactionMarks(marks);
        say(tr("%n area(s) marked for redaction. Review them, then choose Apply Redactions.", nullptr,
               static_cast<int>(marks.size())),
            8000);
    });
    connect(view_, &PageView::markupRequested, this,
            [this](int page, const QVector<QRectF>& boxes, bool strikeOut) {
                const QColor color = toolColor(strikeOut ? QStringLiteral("toolStrike") : QStringLiteral("toolUnderline"));
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
    connect(worker_, &RenderWorker::signaturesReady, this, &DocumentTab::onSignaturesReady);
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
    connect(view_, &PageView::cropBoxRequested, this, [this](int page, QRectF box) {
        const QString pages = askCropPages(window(), page, view_->pageCount());
        if (!pages.isNull()) {
            onWorker([=](RenderWorker* w) { w->cropBox(pages, box); });
        }
    });
    connect(view_, &PageView::eraseRequested, this, [this](int id) {
        onWorker([=](RenderWorker* w) { w->deleteAnnotation(id); });
    });
    connect(view_, &PageView::toolRefused, this, [this](const QString& why) {
        say(why, 5000);
        tool_ = QStringLiteral("toolSelect");
        emit toolRefused();
    });
}

void DocumentTab::buildSignaturePanel() {
    // A banner rather than a dialog: a document's signatures are a standing
    // fact about it, not an event, and the one thing a reader must not have to
    // go looking for. On a row of its own, over the pages.
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
    connect(online, &QPushButton::clicked, this, &DocumentTab::checkRevocation);
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
    connect(signatureCards_, &SignatureCards::trustRequested, this, &DocumentTab::trustSettingsRequested);
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

void DocumentTab::buildPanels() {
    // The sidebar's panels.
    thumbnails_ = new ThumbnailBar(sidebar_);
    connect(thumbnails_, &ThumbnailBar::needThumbnail, worker_, &RenderWorker::renderThumbnail);
    connect(thumbnails_, &ThumbnailBar::pageChosen, this, [this](int page) { view_->goToPage(page); });

    outlineTree_ = new QTreeWidget(sidebar_);
    outlineTree_->setObjectName(QStringLiteral("outlineTree"));
    outlineTree_->setHeaderHidden(true);
    outlineTree_->setColumnCount(1);
    connect(outlineTree_, &QTreeWidget::itemClicked, this, &DocumentTab::onOutlineClicked);

    comments_ = new CommentsPanel(sidebar_);
    connect(comments_, &CommentsPanel::showRequested, this,
            [this](int page, QRectF rect) { view_->goToPage(page, std::max(0.0, rect.top() - 36)); });
    connect(comments_, &CommentsPanel::deleteRequested, this,
            [this](int id) { onWorker([=](RenderWorker* w) { w->deleteAnnotation(id); }); });
    connect(comments_, &CommentsPanel::openRequested, this,
            [this](int id, bool edit) { (void)view_->showComment(id, edit); });
    connect(comments_, &CommentsPanel::styleRequested, this,
            [this](int id, const QColor& color, double opacity, double lineWidth, double fontSize,
                   const QString& author) {
                onWorker([=](RenderWorker* w) {
                    w->setAnnotationStyle(id, color, opacity, lineWidth, fontSize, author);
                });
            });

    // Form panel: one labelled editor per field, in reading order. Fields
    // are filled in on the page too; the panel then shows where the field
    // is, and the editor being typed in (either) frames it on the page.
    form_ = new FormPanel(sidebar_);
    const auto setField = [this](const QString& name, const QString& value) {
        onWorker([=](RenderWorker* w) { w->setFieldValue(name, value); });
    };
    connect(form_, &FormPanel::valueEdited, this, setField);
    connect(view_, &PageView::fieldValueRequested, this, setField);
    connect(form_, &FormPanel::currentFieldChanged, view_, &PageView::setCurrentField);
    connect(form_, &FormPanel::highlightChanged, view_, &PageView::setFieldsShown);
    connect(form_, &FormPanel::flattenRequested, this, &DocumentTab::flattenRequested);
    connect(view_, &PageView::fieldClicked, this, [this](const QString& name, bool inPlace) {
        if (inPlace) {
            form_->revealField(name);  // shown or not; the keyboard stays on the page
            return;
        }
        sidebar_->showPanel(QStringLiteral("form"));
        form_->focusField(name);
    });

    sidebar_->addPanel(QStringLiteral("pages"), QStringLiteral("files"), tr("Pages"), thumbnails_);
    sidebar_->addPanel(QStringLiteral("outline"), QStringLiteral("list-tree"), tr("Outline"), outlineTree_);
    sidebar_->addPanel(QStringLiteral("comments"), QStringLiteral("message-square"), tr("Comments"), comments_);
    sidebar_->addPanel(QStringLiteral("form"), QStringLiteral("text-cursor-input"), tr("Form"), form_);
    sidebar_->addPanel(QStringLiteral("signatures"), QStringLiteral("signature"), tr("Signatures"),
                       signaturePanel_);
    for (const char* panel : {"outline", "form", "signatures"}) {
        sidebar_->setPanelAvailable(QLatin1String(panel), false);  // until the document has one
    }

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("documentSplitter"));
    pageGrid_ = new PageGrid(this);
    connect(pageGrid_, &PageGrid::needThumbnail, worker_, &RenderWorker::renderThumbnail);
    connect(pageGrid_, &PageGrid::openPage, this, [this](int page) {
        emit modeRequested(QStringLiteral("read"));
        showPageGrid(false);
        view_->goToPage(page);
    });
    connect(pageGrid_, &PageGrid::moveRequested, this, [this](const QVector<int>& pages, int before) {
        if (pages.isEmpty() || !certAllows(4) || !confirmChangingSigned(tr("Moving pages"))) {
            return;
        }
        // Where the block lands: before `before`, less the moved pages ahead of it.
        const int ahead = static_cast<int>(std::count_if(pages.begin(), pages.end(), [&](int p) { return p < before; }));
        QVector<int> landed;
        for (int i = 0; i < pages.size(); ++i) {
            landed.push_back(before - ahead + i);
        }
        gridSelectionAfterEdit_ = landed;
        const QString spec = PageGrid::rangeSpec(pages);
        onWorker([=](RenderWorker* w) { w->movePages(spec, before); });
    });
    connect(pageGrid_, &PageGrid::deletePressed, this, [this] {
        if (pageCount_ > 0 && certAllows(4)) {
            deletePages();
        }
    });
    connect(pageGrid_, &PageGrid::filesDropped, this,
            [this](const QStringList& paths, int before) { insertFilesAt(paths, before); });
    viewStack_ = new QStackedWidget(this);
    viewStack_->addWidget(view_);
    viewStack_->addWidget(pageGrid_);

    splitter_->addWidget(sidebar_);
    splitter_->addWidget(viewStack_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setCollapsible(0, false);
    splitter_->setCollapsible(1, false);
    splitter_->setSizes({250, 900});
    // Folded, the sidebar is only its rail: the page takes the width it gave
    // up, and gets it back to the same size when it unfolds.
    connect(sidebar_, &Sidebar::expandedChanged, splitter_, [this](bool expanded) {
        const QList<int> sizes = splitter_->sizes();
        const int total = sizes.value(0) + sizes.value(1);
        if (!expanded) {
            sidebarWidth_ = std::max(sizes.value(0), 150);
            splitter_->setSizes({sidebar_->maximumWidth(), total - sidebar_->maximumWidth()});
        } else if (sidebarWidth_ > 0) {
            splitter_->setSizes({sidebarWidth_, std::max(0, total - sidebarWidth_)});
        }
    });

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(signatureBanner_);
    column->addWidget(splitter_, 1);
}

// --- Opening -------------------------------------------------------------------

void DocumentTab::setPendingTask(const QString& task) {
    pendingTask_ = task;
    verifyPending_ = task == QLatin1String("verify");
}

void DocumentTab::openPath(const QString& path) {
    if (!resolveUnsaved([this, path] { openPath(path); })) {
        return;
    }
    modified_ = false;
    canUndo_ = false;  // the worker has no log for the new document yet
    canRedo_ = false;
    redactionMarks_.clear();  // the view forgets its copy in clear()
    pageCount_ = 0;  // nothing to act on until it opens
    signatureCount_ = 0;
    documentEncrypted_ = false;
    formShown_ = false;
    certLevel_ = 0;
    currentPath_ = QFileInfo(path).absoluteFilePath();
    currentTitle_ = QFileInfo(path).fileName();
    (void)view_->setTool(PageView::Tool::Select);
    tool_ = QStringLiteral("toolSelect");
    if (pendingTask_.isEmpty()) {
        mode_ = QStringLiteral("read");
    }
    showPageGrid(false);
    say(tr("Opening %1…").arg(currentTitle_));
    view_->clear();
    thumbnails_->clearThumbnails();
    outlineTree_->clear();
    comments_->setAnnotations({});
    for (const char* panel : {"outline", "form", "signatures"}) {
        sidebar_->setPanelAvailable(QLatin1String(panel), false);
    }
    signatureBanner_->hide();
    emit titleChanged();
    emit opening();
    emit stateChanged();
    emit requestOpen(currentPath_);
}

void DocumentTab::onOpened(int pageCount, QVector<QSize> baseSizes) {
    pageCount_ = pageCount;
    canUndo_ = false;
    canRedo_ = false;
    signatureCount_ = 0;
    applyCertification(0);
    onWorker([](RenderWorker* w) {
        w->listAnnotations();
        w->listFields();
        w->listSignatures();
    });
    say({});
    view_->setPages(baseSizes);
    const QString zoom = QSettings().value(QLatin1String(prefs::kDefaultZoom), QStringLiteral("width")).toString();
    if (zoom == QLatin1String("page")) {
        view_->fitPage();
    } else if (zoom == QLatin1String("actual")) {
        view_->setZoom(1.0);
    } else {
        view_->fitWidth();
    }
    thumbnails_->setPageCount(pageCount);
    if (startPage_ > 0 && startPage_ < pageCount) {
        view_->goToPage(startPage_);
    }
    startPage_ = -1;
    view_->setFocus();
    emit titleChanged();
    emit stateChanged();
    emit pageChanged(view_->currentPage());
    emit zoomChanged();
    emit opened();
}

void DocumentTab::onFailed(const QString& message) {
    say({});
    pendingTask_.clear();
    verifyPending_ = false;
    QMessageBox::warning(window(), tr("Could not open document"), message);
    if (pageCount_ == 0) {
        currentPath_.clear();
        currentTitle_.clear();
        emit closed();
    }
}

void DocumentTab::onPasswordRequired(bool retry) {
    say({});
    documentEncrypted_ = true;
    bool ok = false;
    const QString prompt =
        retry ? tr("Wrong password. Try again for “%1”:").arg(currentTitle_)
              : tr("“%1” is password-protected. Enter its password:").arg(currentTitle_);
    const QString password = QInputDialog::getText(
        window(), tr("Password required"), prompt, QLineEdit::Password, QString(), &ok);

    if (!ok) {
        // User cancelled: nothing is open, so the tab goes.
        say(tr("Opening cancelled."), 3000);
        pendingTask_.clear();
        currentPath_.clear();
        currentTitle_.clear();
        emit closed();
        return;
    }
    say(tr("Unlocking…"));
    emit requestAuthenticate(password);
}

void DocumentTab::onOutlineReady(const QVector<OutlineRow>& rows) {
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

void DocumentTab::onOutlineClicked(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }
    const int page = item->data(0, Qt::UserRole).toInt();
    const double y = item->data(0, Qt::UserRole + 1).toDouble();
    if (page >= 0) {
        view_->goToPage(page, y);
    }
}

// --- Editing and saving --------------------------------------------------------

void DocumentTab::onEditStateChanged(bool canUndo, bool canRedo, bool modified) {
    canUndo_ = canUndo;
    canRedo_ = canRedo;
    const bool titleChange = modified != modified_;
    modified_ = modified;
    emit stateChanged();
    if (titleChange) {
        emit titleChanged();
    }
}

void DocumentTab::onFieldsReady(const QVector<FieldRow>& rows) {
    fieldRows_ = rows;
    form_->setFields(rows);
    view_->setFormFields(rows);
    emit stateChanged();  // Flatten Form follows whether there are fields
    sidebar_->setPanelAvailable(QStringLiteral("form"), !rows.isEmpty());
    // A form opens on its fields, once: after that the sidebar is the user's.
    if (!rows.isEmpty() && !std::exchange(formShown_, true)) {
        sidebar_->showPanel(QStringLiteral("form"));
    }
}

void DocumentTab::setRedactionMarks(QVector<QPair<int, QRectF>> marks) {
    redactionMarks_ = std::move(marks);
    view_->setRedactionMarks(redactionMarks_);
    emit stateChanged();
}

void DocumentTab::clearRedactionMarks() { setRedactionMarks({}); }

bool DocumentTab::applyRedactions(bool confirm) {
    if (redactionMarks_.isEmpty() || pageCount_ <= 0) {
        return false;
    }
    if (confirm) {
        QSet<int> pages;
        for (const auto& mark : redactionMarks_) {
            pages.insert(mark.first);
        }
        const auto answer = QMessageBox::warning(
            window(), tr("Apply redactions"),
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
    emit stateChanged();
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

bool DocumentTab::confirmBreakingSignatures(const QString& what) {
    if (signatureCount_ == 0) {
        return true;
    }
    const auto answer = QMessageBox::warning(
        window(), tr("This document is signed"),
        tr("%1 cannot be added as a new revision: it rewrites the file, and the %n existing "
           "signature(s) will no longer verify.\n\nCarry on?", nullptr, signatureCount_)
            .arg(what),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Yes;
}

bool DocumentTab::confirmChangingSigned(const QString& what) {
    if (signatureCount_ == 0) {
        return true;
    }
    const auto answer = QMessageBox::question(
        window(), tr("This document is signed"),
        tr("%1 is saved as a new revision after the %n signature(s). They stay intact, but each will "
           "say that the document was changed after it was signed.\n\nCarry on?",
           nullptr, signatureCount_)
            .arg(what),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Yes;
}

void DocumentTab::redactText() {
    bool ok = false;
    const QString needle = QInputDialog::getText(
        window(), tr("Redact text"), tr("Remove every occurrence of (case-insensitive):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || needle.isEmpty()) {
        return;
    }
    if (signatureCount_ == 0 &&
        QMessageBox::warning(window(), tr("Redact text"),
                             tr("Every occurrence of “%1” will be removed from the file, not just "
                                "covered. Once saved, it cannot be brought back.\n\nRedact it?")
                                 .arg(needle),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    if (confirmBreakingSignatures(tr("A redaction"))) {
        onWorker([=](RenderWorker* w) { w->redactText(needle); });
    }
}

void DocumentTab::addWatermark() {
    const int page = std::max(0, view_->currentPage());
    WatermarkDialog dialog(window(), view_->pageImage(page), view_->pageSizePoints(page), view_->pageCount());
    if (dialog.exec() == QDialog::Accepted) {
        const QString pages = dialog.pages();
        const leht::ops::WatermarkOptions options = dialog.options();
        onWorker([=](RenderWorker* w) { w->addWatermark(pages, options); });
    }
}

void DocumentTab::cropMargins() {
    CropMarginsDialog dialog(window(), view_->pageCount());
    if (dialog.exec() == QDialog::Accepted) {
        const QString pages = dialog.pages();
        const leht::ops::Margins margins = dialog.margins();
        onWorker([=](RenderWorker* w) { w->cropMargins(pages, margins); });
    }
}

void DocumentTab::flattenForm() {
    QString text = tr("The form's fields become part of the pages: what they show now stays, "
                      "but nobody can change it any more, in Leht or elsewhere.\n\nUndo brings "
                      "the fields back until the document is closed.");
    if (const QStringList empty = form_->emptyRequired(); !empty.isEmpty()) {
        QStringList labels;
        for (const QString& name : empty) {
            labels << FormPanel::fieldLabel(name);
        }
        text += QStringLiteral("\n\n") +
                tr("Still empty, though required: %1.").arg(QLocale().createSeparatedList(labels));
    }
    if (QMessageBox::question(window(), tr("Flatten the form?"), text,
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) !=
        QMessageBox::Yes) {
        return;
    }
    if (confirmBreakingSignatures(tr("Flattening the form"))) {
        onWorker([](RenderWorker* w) { w->flattenForm(); });
    }
}

void DocumentTab::showProperties() {
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
        PropertiesDialog dialog(window(), currentPath_, pageCount_, view_->pageSizePoints(0), info, editable);
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
}

void DocumentTab::recognizeText() {
#ifndef LEHT_HAVE_OCR
    QMessageBox::information(window(), tr("Recognize text"),
                             tr("This build of Leht was made without OCR."));
#else
    QStringList installed;
    for (const std::string& code :
         leht::ocr::installed_languages(leht::ocr::default_datadir())) {
        installed << QString::fromStdString(code);
    }
    if (installed.isEmpty()) {
        QMessageBox::information(
            window(), tr("Recognize text"),
            tr("No OCR languages are installed. Install Tesseract's language data -- on "
               "Fedora, tesseract-langpack-est and tesseract-langpack-eng."));
        return;
    }
    OcrDialog dialog(window(), view_->pageCount(), installed);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    // Progress per page, with Cancel: the run is on the worker thread, and the
    // cancel reaches it directly, not through its (busy) event queue.
    auto* progress = new QProgressDialog(tr("Starting OCR…"), tr("Cancel"), 0, 0, window());
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
            QMessageBox::warning(window(), tr("Recognize text"), error);
        }
        say(cancelled ? tr("OCR cancelled: %n word(s) read", nullptr, words) + tr(" on %n page(s).", nullptr, pages)
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

void DocumentTab::startSigning(int page, QRectF rect) {
    if (pageCount_ == 0) {
        return;
    }
    // The form's unsigned signature fields are offered as places to sign.
    QVector<FieldRow> emptyFields;
    for (const FieldRow& f : fieldRows_) {
        if (static_cast<leht::ops::FieldType>(f.type) == leht::ops::FieldType::Signature &&
            !f.readOnly && !signedFields_.contains(f.name)) {
            emptyFields.push_back(f);
        }
    }
    SignDialog dialog(window(), page, rect, QString(), signatureCount_ == 0, emptyFields);
    const int answer = dialog.exec();
    if (answer == SignDialog::PlaceBox) {
        emit placeSignatureRequested();
        return;
    }
    if (answer != QDialog::Accepted) {
        return;
    }
    SignSpec spec = dialog.spec();
    // Signing writes a file, so it needs a path. A document opened read-only
    // from somewhere unwritable is signed with Save As.
    QString target = currentPath_;
    if (target.isEmpty()) {
        target = QFileDialog::getSaveFileName(window(), tr("Save signed document as"), QString(),
                                              tr("PDF documents (*.pdf)"));
        if (target.isEmpty()) {
            return;
        }
    }
    say(tr("Signing…"));
    onWorker([=](RenderWorker* w) { w->signDocument(target, spec); });
}

void DocumentTab::addLongTermValidation() {
    if (pageCount_ == 0 || signatureCount_ == 0) {
        return;
    }
    if (modified_) {
        QMessageBox::information(
            window(), tr("Add Long-Term Validation"),
            tr("Save or undo your changes first. Validation data goes in a revision of its "
               "own, which must add nothing else."));
        return;
    }
    QSettings settings;
    bool ok = false;
    const QString tsa = QInputDialog::getText(
        window(), tr("Add Long-Term Validation"),
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
    say(tr("Adding long-term validation data…"));
    onWorker([=](RenderWorker* w) { w->addLongTermValidation(target, tsa); });
}

void DocumentTab::checkRevocation() {
    if (pageCount_ <= 0 || signatureCount_ <= 0) {
        return;
    }
    say(tr("Checking revocation online…"));
    onWorker([](RenderWorker* w) { w->checkRevocationOnline(); });
}

void DocumentTab::recheckSignatures() {
    if (pageCount_ > 0) {
        onWorker([](RenderWorker* w) { w->listSignatures(); });  // judged again, by the new list
    }
}

bool DocumentTab::settlePendingRedactions() {
    if (redactionMarks_.isEmpty()) {
        return true;
    }
    // Saving with marks still pending would write a file that looks redacted
    // and is not: ask, every time.
    QMessageBox box(QMessageBox::Warning, tr("Redactions not applied"),
                    tr("%n area(s) are marked for redaction but not yet removed. Saving now keeps "
                       "everything under them in the file.",
                       nullptr, static_cast<int>(redactionMarks_.size())),
                    QMessageBox::Cancel, window());
    QPushButton* apply = box.addButton(tr("Apply and Save"), QMessageBox::AcceptRole);
    QPushButton* without = box.addButton(tr("Save Without Applying"), QMessageBox::DestructiveRole);
    box.setDefaultButton(apply);
    box.exec();
    if (box.clickedButton() == apply) {
        return applyRedactions(/*confirm=*/false);  // queued before the save, so saved with it
    }
    return box.clickedButton() == without;
}

bool DocumentTab::save() {
    view_->finishFieldEditing();  // queued before the save, so saved with it
    if (pageCount_ <= 0 || !settlePendingRedactions()) {
        return false;
    }
    if (currentPath_.isEmpty() || !currentPath_.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        return saveToChosenPath();  // an image or XPS opened for viewing is not written back as PDF in place
    }
    say(tr("Saving…"));
    const QString path = currentPath_;
    onWorker([=](RenderWorker* w) { w->save(path); });
    return true;
}

bool DocumentTab::saveAs() {
    view_->finishFieldEditing();
    if (pageCount_ <= 0 || !settlePendingRedactions()) {
        return false;
    }
    return saveToChosenPath();
}

bool DocumentTab::saveToChosenPath() {
    const QString path = QFileDialog::getSaveFileName(window(), tr("Save PDF"), currentPath_,
                                                      tr("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        afterSave_ = nullptr;
        return false;
    }
    say(tr("Saving…"));
    onWorker([=](RenderWorker* w) { w->save(path); });
    return true;
}

void DocumentTab::onSaved(const QString& path) {
    currentPath_ = path;
    currentTitle_ = QFileInfo(path).fileName();
    modified_ = false;
    emit titleChanged();
    say(tr("Saved %1").arg(currentTitle_), 3000);
    if (auto then = std::exchange(afterSave_, nullptr)) {
        then();
    }
}

bool DocumentTab::resolveUnsaved(std::function<void()> then) {
    if (!modified_) {
        return true;
    }
    const auto choice = QMessageBox::question(
        window(), tr("Unsaved changes"),
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

// --- Find ----------------------------------------------------------------------

void DocumentTab::search(const QString& needle) {
    view_->clearMatches();
    worker_->cancelSearch();  // a new search never waits behind an old one
    emit requestSearch(needle);
}

void DocumentTab::cancelSearch() {
    worker_->cancelSearch();
    view_->clearMatches();
}

// --- Signatures ----------------------------------------------------------------

namespace {

QString trustWord(int trust) {
    switch (static_cast<leht::crypto::Trust>(trust)) {
        case leht::crypto::Trust::Trusted:     return DocumentTab::tr("trusted");
        case leht::crypto::Trust::Untrusted:   return DocumentTab::tr("not trusted");
        case leht::crypto::Trust::Expired:     return DocumentTab::tr("certificate expired");
        case leht::crypto::Trust::NotYetValid: return DocumentTab::tr("certificate not yet valid");
        case leht::crypto::Trust::Unknown:     return DocumentTab::tr("not checked");
        case leht::crypto::Trust::Revoked:     return DocumentTab::tr("certificate revoked");
    }
    return DocumentTab::tr("not checked");
}

/// One line saying what this signature is worth, and the colour to say it in.
std::pair<QString, QColor> verdict(const SigRow& row) {
    if (!row.rangeOk || !row.intact) {
        return {DocumentTab::tr("Broken"), QColor(170, 20, 20)};
    }
    if (row.changesJudged && !row.changesPermitted) {
        return {DocumentTab::tr("Intact, but changed in a way it forbids"), QColor(170, 20, 20)};
    }
    if (static_cast<leht::crypto::Trust>(row.trust) == leht::crypto::Trust::Revoked) {
        return {DocumentTab::tr("Intact, but the certificate was revoked"), QColor(170, 20, 20)};
    }
    const bool trusted = static_cast<leht::crypto::Trust>(row.trust) ==
                         leht::crypto::Trust::Trusted;
    if (row.changedAfterSigning && !row.laterSignatureCoversChanges &&
        !row.onlyValidationDataAfter) {
        return {DocumentTab::tr("Intact, but the document was changed afterwards"),
                QColor(170, 110, 0)};
    }
    if (!trusted) {
        return {row.documentTimestamp ? DocumentTab::tr("Intact, authority not trusted")
                                      : DocumentTab::tr("Intact, signer not trusted"),
                QColor(170, 110, 0)};
    }
    return {DocumentTab::tr("Valid"), QColor(20, 120, 40)};
}

/// crypto::QualifiedReport::Level::Qes, as SigRow::qualified carries it.
constexpr int kQes = 2;

/// A signature's "Qualified" line: the EU trusted lists' verdict, and why.
QString qualifiedWords(const SigRow& row, bool haveTrustedList) {
    QString words;
    switch (row.qualified) {
        case 0:
            return haveTrustedList
                       ? DocumentTab::tr("not checked")
                       : DocumentTab::tr("not checked: no EU trusted lists yet (Sign → Update EU "
                                         "Trusted Lists…)");
        case 1: words = DocumentTab::tr("not qualified"); break;
        case kQes: words = DocumentTab::tr("qualified electronic signature (QES)"); break;
        case 3: words = DocumentTab::tr("qualified electronic seal"); break;
        case 4: words = DocumentTab::tr("advanced, with a qualified certificate"); break;
        default: return {};
    }
    if (!row.qualifiedDetail.isEmpty()) {
        words = DocumentTab::tr("%1: %2").arg(words, row.qualifiedDetail);
    }
    return words;
}

QString localTime(qint64 unix_seconds) {
    return QDateTime::fromSecsSinceEpoch(unix_seconds).toString(Qt::ISODate);
}

}  // namespace

QString certificationWords(int level) {
    switch (level) {
        case 1: return DocumentTab::tr("no changes allowed");
        case 2: return DocumentTab::tr("form filling and signing allowed");
        case 3: return DocumentTab::tr("form filling, signing and comments allowed");
        default: return {};
    }
}

void DocumentTab::onSignaturesReady(const QVector<SigRow>& rows) {
    signatureCount_ = static_cast<int>(rows.size());
    signedFields_.clear();
    for (const SigRow& r : rows) {
        signedFields_.push_back(r.field);
    }
    signatures_->clear();
    signatureCards_->setRows({}, true);
    sidebar_->setPanelAvailable(QStringLiteral("signatures"), !rows.isEmpty());
    const bool verifyAsked = std::exchange(verifyPending_, false);
    if (rows.isEmpty()) {
        applyCertification(0);  // also refreshes the actions that need a signature
        signatureBanner_->hide();
        if (verifyAsked) {
            QMessageBox::information(window(), tr("Check signatures"),
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
        item->setForeground(1, contrast::readableOn(colour, signatures_->palette().color(QPalette::Base)));
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

void DocumentTab::applyCertification(int level) {
    // What a certified document still lets one do. The window disables the
    // tools and commands (see MainWindow::applyCertification); the panels
    // here are this tab's own. Leht does not break a certification by a click.
    certLevel_ = level;
    // Filling fields is the one change level 2 and 3 allow and 1 does not.
    form_->setEditable(level != 1);
    view_->setFieldsEditable(level != 1);
    comments_->setEditable(certAllows(3));
    view_->setCommentsEditable(certAllows(3));
    emit certificationChanged();
    emit stateChanged();
}

// --- Printing and export -------------------------------------------------------

void DocumentTab::printDialog() {
    if (pageCount_ <= 0) {
        return;
    }
    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(currentTitle_);
    printer.setFromTo(1, pageCount_);

    QPrintDialog dialog(&printer, window());
    dialog.setOption(QAbstractPrintDialog::PrintPageRange, true);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    say(tr("Printing…"));
    printDocument(printer, printer.fromPage(), printer.toPage());
    say(tr("Printed %1").arg(currentTitle_), 3000);
}

bool DocumentTab::printDocument(QPrinter& printer, int fromPage, int toPage) {
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

void DocumentTab::exportImagesDialog() {
    ExportDialog dialog(window(), ExportDialog::Kind::Images, pageCount_, std::max(0, view_->currentPage()));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString spec = dialog.pages();
    const QFileInfo doc(currentPath_);
    const bool one = !spec.isEmpty() && !spec.contains(QLatin1Char(',')) && !spec.contains(QLatin1Char('-'));
    QString pattern;
    if (one) {
        pattern = QFileDialog::getSaveFileName(
            window(), tr("Export page as"), doc.dir().filePath(tr("%1, page %2.png").arg(doc.completeBaseName(), spec)),
            tr("PNG pictures (*.png)"));
    } else {
        const QString folder = QFileDialog::getExistingDirectory(window(), tr("Export pages into"), doc.absolutePath());
        if (!folder.isEmpty()) {
            pattern = QDir(folder).filePath(doc.completeBaseName() + QStringLiteral(", page %1.png"));
        }
    }
    if (pattern.isEmpty()) {
        return;
    }
    const QStringList written = exportImages(spec, dialog.dpi(), pattern);
    say(tr("Exported %n picture(s).", nullptr, static_cast<int>(written.size())), 6000);
}

void DocumentTab::exportTextDialog() {
    ExportDialog dialog(window(), ExportDialog::Kind::Text, pageCount_, std::max(0, view_->currentPage()));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QFileInfo doc(currentPath_);
    const QString path = QFileDialog::getSaveFileName(window(), tr("Export text as"),
                                                      doc.dir().filePath(doc.completeBaseName() + QStringLiteral(".txt")),
                                                      tr("Text files (*.txt)"));
    if (!path.isEmpty()) {
        exportText(dialog.pages(), path);
    }
}

QStringList DocumentTab::exportImages(const QString& pages, int dpi, const QString& pattern) {
    QStringList written;
    std::vector<int> chosen;
    try {
        chosen = leht::page_set(pages.toStdString(), pageCount_);
    } catch (const leht::Error&) {
        return written;
    }
    auto* progress = new QProgressDialog(tr("Exporting pages…"), tr("Cancel"), 0,
                                         static_cast<int>(chosen.size()), window());
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(500);
    const double zoom = dpi / 72.0;
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        progress->setValue(static_cast<int>(i));
        if (progress->wasCanceled()) {
            break;
        }
        const int page = chosen[i];
        QImage image;
        // As printing does: the worker renders, this thread waits for it.
        QMetaObject::invokeMethod(worker_, "renderAt", Qt::BlockingQueuedConnection, Q_RETURN_ARG(QImage, image),
                                  Q_ARG(int, page), Q_ARG(double, zoom));
        if (image.isNull()) {
            continue;
        }
        image.setDotsPerMeterX(static_cast<int>(std::lround(dpi / 0.0254)));
        image.setDotsPerMeterY(static_cast<int>(std::lround(dpi / 0.0254)));
        const QString path = chosen.size() == 1 && !pattern.contains(QLatin1String("%1"))
                                 ? pattern
                                 : pattern.arg(page + 1, 3, 10, QLatin1Char('0'));
        if (image.save(path, "PNG")) {
            written << path;
        }
    }
    progress->close();
    progress->deleteLater();
    return written;
}

void DocumentTab::exportText(const QString& pages, const QString& path) {
    auto once = std::make_shared<QMetaObject::Connection>();
    *once = connect(worker_, &RenderWorker::textReady, this,
                    [this, once, path](const QVector<int>& numbers, const QStringList& texts) {
        disconnect(*once);
        QFile file(path);
        if (numbers.isEmpty() || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(window(), tr("Export text"), tr("Could not write “%1”.").arg(path));
            return;
        }
        // Pages apart by a form feed, as pdftotext does: a plain-text page break.
        file.write(texts.join(QLatin1Char('\f')).toUtf8());
        file.close();
        say(tr("Exported the text of %n page(s).", nullptr, static_cast<int>(numbers.size())), 6000);
    });
    onWorker([pages](RenderWorker* w) { w->extractText(pages); });
}

// --- Pages mode: organising pages ----------------------------------------------

QVector<int> DocumentTab::targetPages() const {
    if (pageCount_ <= 0) {
        return {};
    }
    if (viewStack_->currentWidget() == pageGrid_) {
        return pageGrid_->selectedPages();
    }
    return {std::max(0, view_->currentPage())};
}

void DocumentTab::showPageGrid(bool grid) {
    const bool wasGrid = viewStack_->currentWidget() == pageGrid_;
    viewStack_->setCurrentWidget(grid ? static_cast<QWidget*>(pageGrid_) : view_);
    // The grid is the pages at a glance; the sidebar's thumbnails would only
    // repeat it. Folded while it shows, as it was after.
    if (grid && !wasGrid) {
        sidebarBeforeGrid_ = sidebar_->isExpanded();
        sidebar_->setExpanded(false);
    } else if (!grid && wasGrid && sidebarBeforeGrid_) {
        sidebar_->setExpanded(true);
    }
    if (grid) {
        if (pageGrid_->count() != pageCount_) {
            pageGrid_->setPageCount(pageCount_);
        }
        pageGrid_->selectPages({std::max(0, view_->currentPage())});
        pageGrid_->setFocus();
    } else {
        view_->setFocus();
    }
}

void DocumentTab::rotatePages(int degrees) {
    const QVector<int> pages = targetPages();
    if (pages.isEmpty() || !confirmChangingSigned(tr("Turning pages"))) {
        return;
    }
    gridSelectionAfterEdit_ = pages;
    const QString spec = PageGrid::rangeSpec(pages);
    onWorker([=](RenderWorker* w) { w->rotatePages(spec, degrees); });
}

void DocumentTab::deletePages() {
    const QVector<int> pages = targetPages();
    if (pages.isEmpty()) {
        return;
    }
    if (pages.size() >= pageCount_) {
        QMessageBox::information(window(), tr("Delete pages"), tr("A document must keep at least one page."));
        return;
    }
    if (!confirmChangingSigned(tr("Deleting pages"))) {
        return;
    }
    gridSelectionAfterEdit_ = {std::min(pages.first(), pageCount_ - static_cast<int>(pages.size()) - 1)};
    const QString spec = PageGrid::rangeSpec(pages);
    onWorker([=](RenderWorker* w) { w->deletePages(spec); });
    say(tr("Deleted %n page(s). Undo brings them back.", nullptr, static_cast<int>(pages.size())), 6000);
}

void DocumentTab::insertFileDialog() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        window(), tr("Insert pages from"), QFileInfo(currentPath_).absolutePath(), tr("PDF documents (*.pdf)"));
    if (!paths.isEmpty()) {
        insertFilesAt(paths, pageGrid_->isVisible() ? pageGrid_->insertionPoint()
                                                    : std::max(0, view_->currentPage()) + 1);
    }
}

void DocumentTab::insertBlankPage() {
    if (!confirmChangingSigned(tr("Inserting a page"))) {
        return;
    }
    const QVector<int> pages = targetPages();
    const int at = pages.isEmpty() ? pageCount_ : pages.last() + 1;
    const QSize like = view_->pageSizePoints(std::max(0, at - 1)).toSize();
    const QSizeF size = like.isEmpty() ? QSizeF(595, 842) : QSizeF(like);  // A4 if in doubt
    gridSelectionAfterEdit_ = {at};
    onWorker([=](RenderWorker* w) { w->insertBlankPage(at, size); });
}

void DocumentTab::insertFilesAt(const QStringList& paths, int at) {
    if (paths.isEmpty() || pageCount_ <= 0 || !certAllows(4) || !confirmChangingSigned(tr("Inserting pages"))) {
        return;
    }
    // The viewer only reads the bytes; the worker parses them, in its sandbox.
    constexpr qint64 kMaxBytes = qint64{128} << 20;
    const int where = std::clamp(at, 0, pageCount_);
    // Each file goes in at the same place, so the last goes first: the pages
    // then read in the order the files were given.
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        const QString& path = *it;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxBytes) {
            QMessageBox::warning(window(), tr("Insert pages"),
                                 file.size() > kMaxBytes
                                     ? tr("“%1” is larger than 128 MB; combine it with Combine Files instead.")
                                           .arg(QFileInfo(path).fileName())
                                     : tr("“%1” could not be read.").arg(QFileInfo(path).fileName()));
            return;
        }
        const QByteArray data = file.readAll();
        onWorker([=](RenderWorker* w) { w->insertPages(where, data, QString()); });
    }
    say(tr("Inserting %n file(s)…", nullptr, static_cast<int>(paths.size())), 4000);
}
