// SPDX-License-Identifier: AGPL-3.0-or-later
// MainWindow's structure: the toolbar, the sidebar and its panels, the mode
// bar, the welcome view, the menus -- and what the welcome view and dropped
// files ask for. The document logic stays in main_window.cpp.
#include "main_window.hpp"

#include "actions.hpp"
#include "comments_panel.hpp"
#include "file_tools.hpp"
#include "file_tools_dialogs.hpp"
#include "icons.hpp"
#include "mode_bar.hpp"
#include "page_view.hpp"
#include "preferences.hpp"
#include "recent_files.hpp"
#include "render_worker.hpp"
#include "sidebar.hpp"
#include "thumbnail_bar.hpp"
#include "welcome_view.hpp"
#ifdef LEHT_HAVE_OCR
#include "leht/ocr/ocr.hpp"
#endif

#include <QActionGroup>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

void MainWindow::buildMainToolbar() {
    QToolBar* bar = addToolBar(tr("Main"));
    bar->setObjectName(QStringLiteral("mainBar"));
    bar->setMovable(false);
    bar->setIconSize(QSize(20, 20));
    actions_->populate(bar, {QStringLiteral("toggleSidebar"), QStringLiteral("-"), QStringLiteral("open"),
                             QStringLiteral("save"), QStringLiteral("print"), QStringLiteral("-"),
                             QStringLiteral("undo"), QStringLiteral("redo")});
    bar->addSeparator();

    // Go to page: the number, editable, and the count beside it.
    pageSpin_ = new QSpinBox(bar);
    pageSpin_->setObjectName(QStringLiteral("pageNumber"));
    pageSpin_->setMinimum(1);
    pageSpin_->setMaximum(1);
    pageSpin_->setEnabled(false);
    pageSpin_->setKeyboardTracking(false);
    pageSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    pageSpin_->setAlignment(Qt::AlignRight);
    pageSpin_->setMinimumWidth(48);
    pageSpin_->setToolTip(tr("Go to page"));
    pageSpin_->setAccessibleName(tr("Page number"));
    bar->addWidget(pageSpin_);
    pageLabel_ = new QLabel(bar);
    pageLabel_->setContentsMargins(4, 0, 8, 0);
    bar->addWidget(pageLabel_);
    connect(pageSpin_, &QSpinBox::editingFinished, this, &MainWindow::goToPageFromSpin);
    bar->addSeparator();

    actions_->populate(bar, {QStringLiteral("zoomOut")});
    zoomLabel_ = new QLabel(bar);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    zoomLabel_->setMinimumWidth(48);
    zoomLabel_->setToolTip(tr("Zoom"));
    bar->addWidget(zoomLabel_);
    actions_->populate(bar, {QStringLiteral("zoomIn"), QStringLiteral("fitWidth"), QStringLiteral("fitPage")});

    auto* spacer = new QWidget(bar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    actions_->populate(bar, {QStringLiteral("find")});
}

void MainWindow::buildLayout() {
    // Find bar: a hidden toolbar with a query field, match counter, and
    // next/prev. Shown by Ctrl+F, dismissed by Escape.
    findBar_ = new QToolBar(tr("Find"), this);
    findBar_->setObjectName(QStringLiteral("findBar"));
    findBar_->setMovable(false);
    findEdit_ = new QLineEdit(findBar_);
    findEdit_->setPlaceholderText(tr("Find in document"));
    findEdit_->setClearButtonEnabled(true);
    findEdit_->setMaximumWidth(280);
    findBar_->addWidget(findEdit_);
    actions_->populate(findBar_, {QStringLiteral("findPrevious"), QStringLiteral("findNext")});
    findLabel_ = new QLabel(findBar_);
    findLabel_->setMinimumWidth(90);
    findBar_->addWidget(findLabel_);
    addToolBar(Qt::BottomToolBarArea, findBar_);
    findBar_->hide();
    connect(findEdit_, &QLineEdit::returnPressed, this, &MainWindow::runSearch);
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &MainWindow::hideFindBar);

    // The sidebar's panels.
    thumbnails_ = new ThumbnailBar(sidebar_);
    connect(thumbnails_, &ThumbnailBar::needThumbnail, worker_, &RenderWorker::renderThumbnail);
    connect(thumbnails_, &ThumbnailBar::pageChosen, this, [this](int page) { view_->goToPage(page); });

    outlineTree_ = new QTreeWidget(sidebar_);
    outlineTree_->setObjectName(QStringLiteral("outlineTree"));
    outlineTree_->setHeaderHidden(true);
    outlineTree_->setColumnCount(1);
    connect(outlineTree_, &QTreeWidget::itemClicked, this, &MainWindow::onOutlineClicked);

    comments_ = new CommentsPanel(sidebar_);
    connect(comments_, &CommentsPanel::showRequested, this,
            [this](int page, QRectF rect) { view_->goToPage(page, std::max(0.0, rect.top() - 36)); });
    connect(comments_, &CommentsPanel::deleteRequested, this,
            [this](int id) { onWorker([=](RenderWorker* w) { w->deleteAnnotation(id); }); });
    connect(comments_, &CommentsPanel::editRequested, this, [this](int id, const QString& current) {
        bool ok = false;
        const QString text =
            QInputDialog::getMultiLineText(this, tr("Edit comment"), tr("Text:"), current, &ok);
        if (!ok || text == current) {
            return;
        }
        if (text.trimmed().isEmpty()) {
            onWorker([=](RenderWorker* w) { w->deleteAnnotation(id); });
        } else {
            onWorker([=](RenderWorker* w) { w->setAnnotationText(id, text); });
        }
    });

    // Form panel: one row per field, the value editable in place.
    fields_ = new QTableWidget(0, 2, sidebar_);
    fields_->setObjectName(QStringLiteral("formFields"));
    fields_->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
    fields_->horizontalHeader()->setStretchLastSection(true);
    fields_->verticalHeader()->hide();
    connect(fields_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (populatingFields_ || item->column() != 1) {
            return;
        }
        const QString name = fields_->item(item->row(), 0)->text();
        const QString value = item->text();
        onWorker([=](RenderWorker* w) { w->setFieldValue(name, value); });
    });

    sidebar_->addPanel(QStringLiteral("pages"), QStringLiteral("files"), tr("Pages"), thumbnails_);
    sidebar_->addPanel(QStringLiteral("outline"), QStringLiteral("list-tree"), tr("Outline"), outlineTree_);
    sidebar_->addPanel(QStringLiteral("comments"), QStringLiteral("message-square"), tr("Comments"), comments_);
    sidebar_->addPanel(QStringLiteral("form"), QStringLiteral("text-cursor-input"), tr("Form"), fields_);
    sidebar_->addPanel(QStringLiteral("signatures"), QStringLiteral("signature"), tr("Signatures"),
                       signaturePanel_);
    for (const char* panel : {"outline", "form", "signatures"}) {
        sidebar_->setPanelAvailable(QLatin1String(panel), false);  // until the document has one
    }

    // Modes: each shows Select and its own tools.
    const auto separator = [this] {
        auto* a = new QAction(this);
        a->setSeparator(true);
        return a;
    };
    const auto tools = [this, &separator](std::initializer_list<const char*> ids) {
        QList<QAction*> list{actions_->find(QStringLiteral("toolSelect")), separator()};
        for (const char* id : ids) {
            if (QLatin1String(id) == QLatin1String("-")) {
                list << separator();
            } else if (QAction* a = actions_->find(QLatin1String(id))) {
                list << a;
            }
        }
        return list;
    };
    modeTools_.insert(QStringLiteral("read"), tools({"copy", "find"}));
    modeTools_.insert(QStringLiteral("comment"),
                      tools({"toolHighlight", "toolNote", "toolText", "toolDraw", "-", "toolMove", "toolErase"}));
    modeTools_.insert(QStringLiteral("sign"),
                      tools({"toolSign", "signInvisibly", "-", "addLongTermValidation", "checkRevocation"}));
    modeTools_.insert(QStringLiteral("pages"),
                      tools({"toolCrop", "cropMargins", "watermark", "-", "recognizeText", "-", "splitDocument"}));
    modeTools_.insert(QStringLiteral("redact"), tools({"toolRedact", "redactText"}));
    modes_ = new ModeBar(this);
    modes_->addMode(QStringLiteral("read"), QStringLiteral("book-open"), tr("Read"),
                    tr("Read, search and copy text"), modeTools_.value(QStringLiteral("read")));
    modes_->addMode(QStringLiteral("comment"), QStringLiteral("message-square"), tr("Comment"),
                    tr("Highlight, add notes and text, draw"), modeTools_.value(QStringLiteral("comment")));
    modes_->addMode(QStringLiteral("sign"), QStringLiteral("signature"), tr("Fill && Sign"),
                    tr("Fill in forms and sign"), modeTools_.value(QStringLiteral("sign")));
    modes_->addMode(QStringLiteral("pages"), QStringLiteral("layout-grid"), tr("Pages"),
                    tr("Crop, watermark, recognize text, split"), modeTools_.value(QStringLiteral("pages")));
    modes_->addMode(QStringLiteral("redact"), QStringLiteral("eye-off"), tr("Redact"),
                    tr("Remove content from the file for good"), modeTools_.value(QStringLiteral("redact")));
    connect(modes_, &ModeBar::modeChanged, this, [this](const QString& mode) {
        // A tool the new mode does not show is put down.
        if (!modeTools_.value(mode).contains(tools_->checkedAction())) {
            tools_->actions().first()->trigger();
        }
        // The panel that goes with the mode, if the sidebar is open.
        if (sidebar_->isExpanded()) {
            if (mode == QLatin1String("comment")) {
                sidebar_->showPanel(QStringLiteral("comments"));
            } else if (mode == QLatin1String("sign") && sidebar_->isPanelAvailable(QStringLiteral("form"))) {
                sidebar_->showPanel(QStringLiteral("form"));
            }
        }
    });
    // A tool picked from a menu brings its mode along.
    connect(tools_, &QActionGroup::triggered, this, [this](QAction* tool) {
        if (modeTools_.value(modes_->mode()).contains(tool)) {
            return;
        }
        for (const QString mode : {QStringLiteral("comment"), QStringLiteral("sign"), QStringLiteral("pages"),
                                   QStringLiteral("redact")}) {
            if (modeTools_.value(mode).contains(tool)) {
                modes_->setMode(mode);
                return;
            }
        }
    });

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("documentSplitter"));
    splitter->addWidget(sidebar_);
    splitter->addWidget(view_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    splitter->setSizes({250, 900});

    documentPage_ = new QWidget(this);
    documentPage_->setObjectName(QStringLiteral("documentPage"));
    auto* column = new QVBoxLayout(documentPage_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(modes_);
    column->addWidget(splitter, 1);

    welcome_ = new WelcomeView(this);
    connect(welcome_, &WelcomeView::openRequested, this, &MainWindow::openDialog);
    connect(welcome_, &WelcomeView::openPath, this, &MainWindow::openPath);
    connect(welcome_, &WelcomeView::taskChosen, this, &MainWindow::startTask);
    connect(welcome_, &WelcomeView::filesDropped, this, &MainWindow::handleDroppedFiles);

    stack_ = new QStackedWidget(this);
    stack_->addWidget(welcome_);
    stack_->addWidget(documentPage_);
    setCentralWidget(stack_);
    updateZoomLabel();
}

void MainWindow::buildMenus() {
    recentMenu_ = new QMenu(tr("Open &Recent"), this);
    recentMenu_->menuAction()->setObjectName(QStringLiteral("openRecent"));
    recentMenu_->setIcon(icons::named(QStringLiteral("clock")));
    connect(recentMenu_, &QMenu::aboutToShow, this, [this] {
        recentMenu_->clear();
        const QStringList files = recent::files();
        for (const QString& path : files) {
            QAction* a = recentMenu_->addAction(QFileInfo(path).fileName());
            a->setToolTip(path);
            a->setStatusTip(path);
            connect(a, &QAction::triggered, this, [this, path] { openPath(path); });
        }
        if (files.isEmpty()) {
            recentMenu_->addAction(tr("No recent files"))->setEnabled(false);
        } else {
            recentMenu_->addSeparator();
            recentMenu_->addAction(tr("Clear List"), this, [] { recent::clear(); });
        }
    });

    using Item = ActionRegistry::Item;
    const auto ids = [](std::initializer_list<const char*> list) {
        std::vector<Item> items;
        for (const char* id : list) {
            items.emplace_back(id);
        }
        return items;
    };
    actions_->populate(menuBar(), {
        {tr("&File"), ids({"open", "openRecent", "close", "-", "save", "saveAs", "-", "combineFiles",
                           "reduceFileSize", "splitDocument", "-", "print", "-", "quit"})},
        {tr("&Edit"), ids({"undo", "redo", "-", "copy", "-", "find", "findNext", "findPrevious", "-",
                           "preferences"})},
        {tr("&View"), ids({"zoomIn", "zoomOut", "actualSize", "fitWidth", "fitPage", "-", "rotateView", "-",
                           "toggleSidebar", "fullScreen"})},
        {tr("&Pages"), ids({"toolCrop", "cropMargins", "-", "watermark"})},
        {tr("&Comment"), ids({"toolHighlight", "toolNote", "toolText", "toolDraw", "-", "toolMove",
                              "toolErase"})},
        {tr("&Sign"), ids({"toolSign", "signInvisibly", "-", "addLongTermValidation", "checkRevocation", "-",
                           "trustedCertificates"})},
        {tr("&Tools"), ids({"recognizeText", "-", "toolRedact", "redactText"})},
        {tr("&Help"), ids({"shortcuts", "-", "about"})},
    });
}

void MainWindow::applyAppearance() {
    // Without the SVG plugin there are no icons: words, then, not blank buttons.
    const bool text = QSettings().value(QLatin1String(prefs::kToolbarText), false).toBool() || !icons::available();
    const Qt::ToolButtonStyle style = text ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly;
    if (auto* bar = findChild<QToolBar*>(QStringLiteral("mainBar"))) {
        bar->setToolButtonStyle(style);
    }
    modes_->setToolButtonStyle(text ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    actions_->reloadIcons();
}

void MainWindow::openPreferences(int page) {
    QStringList languages;
#ifdef LEHT_HAVE_OCR
    for (const std::string& code : leht::ocr::installed_languages(leht::ocr::default_datadir())) {
        languages << QString::fromStdString(code);
    }
#endif
    PreferencesDialog dialog(this, languages, static_cast<PreferencesDialog::Page>(page));
    connect(&dialog, &PreferencesDialog::appearanceChanged, this, &MainWindow::applyAppearance);
    connect(&dialog, &PreferencesDialog::trustChanged, this, [this] {
        if (pageCount_ > 0) {
            onWorker([](RenderWorker* w) { w->listSignatures(); });  // judged again, by the new list
        }
    });
    dialog.exec();
}

void MainWindow::showAbout() {
    QMessageBox box(this);
    box.setWindowTitle(tr("About Leht"));
    box.setIconPixmap(QApplication::windowIcon().pixmap(64, 64));
    box.setTextFormat(Qt::RichText);
    box.setText(tr("<h3>Leht %1</h3><p>Every PDF, one page at a time.</p>").arg(QStringLiteral(LEHT_VERSION)));
    box.setInformativeText(
        tr("<p>Documents are opened in a sandboxed process, and never leave this computer.</p>"
           "<p>Free software under the GNU Affero General Public License, version 3 or later.<br>"
           "Icons: Lucide (ISC licence).</p>"));
    box.exec();
}

bool MainWindow::isShowingWelcome() const {
    return stack_ != nullptr && stack_->currentWidget() == welcome_;
}

void MainWindow::showWelcome() {
    welcome_->refresh();
    stack_->setCurrentWidget(welcome_);
    signatureBanner_->hide();
    hideFindBar();
    actions_->refresh();
    welcome_->setFocus();
}

void MainWindow::closeDocument() {
    if (pageCount_ <= 0) {
        return;
    }
    if (!resolveUnsaved([this] {
            modified_ = false;
            closeDocument();
        })) {
        return;
    }
    pageCount_ = 0;
    signatureCount_ = 0;
    modified_ = false;
    currentPath_.clear();
    currentTitle_.clear();
    view_->clear();
    thumbnails_->clearThumbnails();
    outlineTree_->clear();
    comments_->setAnnotations({});
    signatures_->clear();
    populatingFields_ = true;
    fields_->setRowCount(0);
    populatingFields_ = false;
    for (const char* panel : {"outline", "form", "signatures"}) {
        sidebar_->setPanelAvailable(QLatin1String(panel), false);
    }
    pageSpin_->setEnabled(false);
    pageLabel_->clear();
    undoAction_->setEnabled(false);
    redoAction_->setEnabled(false);
    applyCertification(0);
    updateTitle();
    showWelcome();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (!droppedFiles(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QStringList paths = droppedFiles(event->mimeData());
    if (!paths.isEmpty()) {
        event->acceptProposedAction();
        // After the drop has finished: a dialog inside a drop event holds the
        // drag source waiting.
        QTimer::singleShot(0, this, [this, paths] { handleDroppedFiles(paths); });
    }
}

void MainWindow::handleDroppedFiles(const QStringList& paths) {
    if (paths.isEmpty()) {
        return;
    }
    if (paths.size() == 1) {
        openPath(paths.first());
        return;
    }
    QMessageBox box(QMessageBox::Question, tr("Several files"),
                    tr("You dropped %n file(s). Combine them into one PDF?", nullptr,
                       static_cast<int>(paths.size())),
                    QMessageBox::Cancel, this);
    QPushButton* combine = box.addButton(tr("Combine…"), QMessageBox::AcceptRole);
    QPushButton* first = box.addButton(tr("Open the First"), QMessageBox::ActionRole);
    box.setDefaultButton(combine);
    box.exec();
    if (box.clickedButton() == combine) {
        combineFiles(paths);
    } else if (box.clickedButton() == first) {
        openPath(paths.first());
    }
}

void MainWindow::combineFiles(const QStringList& initial) {
    const bool startsWithOpen = pageCount_ > 0 && initial.value(0) == currentPath_;
    CombineDialog dialog(this, startsWithOpen ? currentPath_ : QString());
    dialog.addPaths(startsWithOpen ? initial.mid(1) : initial);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QStringList inputs = dialog.inputs();
    const QString output = dialog.output();
    const auto go = [this, inputs, output] {
        runFileTool(tr("Combining files…"), [inputs, output](FileTools* t, const QString&) {
            t->combine(inputs, output, false);
        });
    };
    // The open document goes in as it is on disk.
    if (currentPath_.isEmpty() || !inputs.contains(currentPath_) || resolveUnsaved(go)) {
        go();
    }
}

void MainWindow::startTask(const QString& task) {
    if (task == QLatin1String(WelcomeView::kCombine)) {
        combineFiles({});
        return;
    }
    const QString path = askOpenPath();
    if (path.isEmpty()) {
        return;
    }
    pendingTask_ = task;
    verifyPending_ = task == QLatin1String(WelcomeView::kVerify);
    openPath(path);
}

void MainWindow::runPendingTask() {
    const QString task = std::exchange(pendingTask_, QString());
    if (pageCount_ <= 0) {
        return;
    }
    if (task == QLatin1String(WelcomeView::kSign)) {
        modes_->setMode(QStringLiteral("sign"));
        if (QAction* sign = actions_->find(QStringLiteral("toolSign")); sign != nullptr && sign->isEnabled()) {
            sign->trigger();
            statusBar()->showMessage(
                tr("Drag a box where the signature should go — or choose Sign Invisibly."), 12000);
        }
    } else if (task == QLatin1String(WelcomeView::kFill)) {
        modes_->setMode(QStringLiteral("sign"));
        if (sidebar_->isPanelAvailable(QStringLiteral("form"))) {
            sidebar_->showPanel(QStringLiteral("form"));
        } else {
            // The fields arrive after the pages; say so only if there are none.
            QTimer::singleShot(1500, this, [this] {
                if (pageCount_ > 0 && !sidebar_->isPanelAvailable(QStringLiteral("form"))) {
                    statusBar()->showMessage(tr("This document has no form fields to fill in."), 8000);
                }
            });
        }
    } else if (task == QLatin1String(WelcomeView::kOcr)) {
        recognizeText();
    } else if (task == QLatin1String(WelcomeView::kReduce)) {
        if (QAction* reduce = actions_->find(QStringLiteral("reduceFileSize"))) {
            reduce->trigger();
        }
    }
    // kVerify is answered by onSignaturesReady (verifyPending_).
}
