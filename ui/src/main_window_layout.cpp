// SPDX-License-Identifier: AGPL-3.0-or-later
// MainWindow's structure: the toolbar, the sidebar and its panels, the mode
// bar, the welcome view, the menus -- and what the welcome view and dropped
// files ask for. The document logic stays in main_window.cpp.
#include "main_window.hpp"

#include "actions.hpp"
#include "color_swatches.hpp"
#include "comments_panel.hpp"
#include "first_run_hints.hpp"
#include "export_dialog.hpp"
#include "file_tools.hpp"
#include "file_tools_dialogs.hpp"
#include "icons.hpp"
#include "mode_bar.hpp"
#include "page_grid.hpp"
#include "page_view.hpp"
#include "preferences.hpp"
#include "recent_files.hpp"
#include "render_worker.hpp"
#include "sidebar.hpp"
#include "thumbnail_bar.hpp"
#include "welcome_view.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#ifdef LEHT_HAVE_OCR
#include "leht/ocr/ocr.hpp"
#endif

#include <QActionGroup>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFile>
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
#include <QProgressDialog>
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

#include <algorithm>
#include <cmath>
#include <memory>

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
                      tools({"toolHighlight", "toolUnderline", "toolStrike", "toolNote", "toolText", "toolDraw",
                             "toolStamp", "-", "toolMove", "toolErase"}));
    modeTools_.insert(QStringLiteral("sign"),
                      tools({"toolSign", "signInvisibly", "-", "addLongTermValidation", "checkRevocation"}));
    modeTools_.insert(QStringLiteral("pages"),
                      tools({"pageGrid", "-", "pageRotateLeft", "pageRotateRight", "-", "pageInsertFile",
                             "pageInsertBlank", "pageExtract", "pageDelete", "-", "toolCrop", "cropMargins",
                             "watermark", "recognizeText"}));
    modeTools_.insert(QStringLiteral("redact"),
                      tools({"toolRedact", "-", "applyRedactions", "clearRedactionMarks", "-", "redactText"}));
    modes_ = new ModeBar(this);
    modes_->addMode(QStringLiteral("read"), QStringLiteral("book-open"), tr("Read"),
                    tr("Read, search and copy text"), modeTools_.value(QStringLiteral("read")));
    modes_->addMode(QStringLiteral("comment"), QStringLiteral("message-square"), tr("Comment"),
                    tr("Highlight, add notes and text, draw"), modeTools_.value(QStringLiteral("comment")));
    modes_->addMode(QStringLiteral("sign"), QStringLiteral("signature"), tr("Fill && Sign"),
                    tr("Fill in forms and sign"), modeTools_.value(QStringLiteral("sign")));
    modes_->addMode(QStringLiteral("pages"), QStringLiteral("layout-grid"), tr("Pages"),
                    tr("Turn, reorder, insert and delete pages; crop, watermark, recognize text"),
                    modeTools_.value(QStringLiteral("pages")));
    modes_->addMode(QStringLiteral("redact"), QStringLiteral("eye-off"), tr("Redact"),
                    tr("Remove content from the file for good"), modeTools_.value(QStringLiteral("redact")));
    connect(modes_, &ModeBar::modeChanged, this, [this](const QString& mode) {
        // Pages mode opens on the page grid; every other mode reads.
        showPageGrid(mode == QLatin1String("pages"));
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
    // The comment tools' colour, at the end of their row; it follows the tool.
    swatches_ = new ColorSwatches(this);
    swatches_->setToolTip(tr("The colour the chosen tool draws in"));
    modes_->setModeExtra(QStringLiteral("comment"), swatches_);
    const auto colourable = [](const QString& id) {
        return id == QLatin1String("toolHighlight") || id == QLatin1String("toolUnderline") ||
               id == QLatin1String("toolStrike") || id == QLatin1String("toolDraw") ||
               id == QLatin1String("toolText");
    };
    const auto syncSwatches = [this, colourable] {
        QAction* tool = tools_->checkedAction();
        const QString id = tool != nullptr ? tool->objectName() : QString();
        swatches_->setEnabled(colourable(id));
        if (colourable(id)) {
            swatches_->setColor(toolColor(id));
        }
        view_->setNewTextColor(toolColor(QStringLiteral("toolText")));
    };
    connect(tools_, &QActionGroup::triggered, this, syncSwatches);
    connect(swatches_, &ColorSwatches::colorChosen, this, [this, colourable, syncSwatches](const QColor& colour) {
        QAction* tool = tools_->checkedAction();
        const QString id = tool != nullptr ? tool->objectName() : QString();
        if (colourable(id)) {
            QSettings().setValue(QStringLiteral("toolColors/") + id, colour.name());
        }
        syncSwatches();
    });
    syncSwatches();

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
    pageGrid_ = new PageGrid(this);
    connect(pageGrid_, &PageGrid::needThumbnail, worker_, &RenderWorker::renderThumbnail);
    connect(pageGrid_, &PageGrid::openPage, this, [this](int page) {
        modes_->setMode(QStringLiteral("read"));
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
    connect(pageGrid_, &PageGrid::deletePressed, actions_->find(QStringLiteral("pageDelete")),
            &QAction::trigger);
    connect(pageGrid_, &PageGrid::filesDropped, this,
            [this](const QStringList& paths, int before) { insertFilesAt(paths, before); });
    pageGrid_->setContextActions({actions_->find(QStringLiteral("pageRotateLeft")),
                                  actions_->find(QStringLiteral("pageRotateRight")), nullptr,
                                  actions_->find(QStringLiteral("pageInsertFile")),
                                  actions_->find(QStringLiteral("pageInsertBlank")),
                                  actions_->find(QStringLiteral("pageExtract")), nullptr,
                                  actions_->find(QStringLiteral("pageDelete"))});
    viewStack_ = new QStackedWidget(this);
    viewStack_->addWidget(view_);
    viewStack_->addWidget(pageGrid_);

    splitter->addWidget(sidebar_);
    splitter->addWidget(viewStack_);
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
    connect(welcome_, &WelcomeView::openPath, this, &MainWindow::openDocument);
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
            connect(a, &QAction::triggered, this, [this, path] { openDocument(path); });
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
    std::vector<Item> fileMenu = ids({"newWindow", "open", "openRecent", "close", "closeWindow", "-", "save",
                                      "saveAs", "-", "combineFiles", "reduceFileSize", "splitDocument", "-",
                                      "protect", "unprotect", "-", "properties"});
    fileMenu.emplace_back(tr("&Export"), ids({"exportImages", "exportText"}));
    for (Item& item : ids({"print", "-", "quit"})) {
        fileMenu.push_back(std::move(item));
    }
    actions_->populate(menuBar(), {
        {tr("&File"), fileMenu},
        {tr("&Edit"), ids({"undo", "redo", "-", "copy", "-", "find", "findNext", "findPrevious", "-",
                           "preferences"})},
        {tr("&View"), ids({"zoomIn", "zoomOut", "actualSize", "fitWidth", "fitPage", "-", "rotateView", "-",
                           "toggleSidebar", "fullScreen", "-", "focusNextRegion", "focusPreviousRegion"})},
        {tr("&Pages"), ids({"pageRotateLeft", "pageRotateRight", "-", "pageInsertFile", "pageInsertBlank",
                            "pageExtract", "pageDelete", "-", "toolCrop", "cropMargins", "-", "watermark"})},
        {tr("&Comment"), ids({"toolHighlight", "toolUnderline", "toolStrike", "toolNote", "toolText",
                              "toolDraw", "toolStamp", "-", "toolMove", "toolErase"})},
        {tr("&Sign"), ids({"toolSign", "signInvisibly", "-", "addLongTermValidation", "checkRevocation", "-",
                           "updateTrustedList", "trustedCertificates"})},
        {tr("&Tools"), ids({"recognizeText", "-", "toolRedact", "applyRedactions", "clearRedactionMarks",
                            "redactText"})},
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
    setRedactionMarks({});
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
        openDocument(paths.first());
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
        openDocument(paths.first());
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

// --- Pages mode: organising pages ------------------------------------------

void MainWindow::buildPageActions() {
    {
        // File > Export: not a page edit, but it lives with the page commands.
        const QString file = tr("File");
        const auto open = [this] { return pageCount_ > 0; };
        QAction* images = actions_->add({.id = QStringLiteral("exportImages"), .text = tr("Pages as &Images…"),
                                         .icon = QStringLiteral("image"),
                                         .tip = tr("Save pages as PNG pictures"), .enabledWhen = open, .group = file});
        connect(images, &QAction::triggered, this, [this] {
            ExportDialog dialog(this, ExportDialog::Kind::Images, pageCount_, std::max(0, view_->currentPage()));
            if (dialog.exec() != QDialog::Accepted) {
                return;
            }
            const QString spec = dialog.pages();
            const QFileInfo doc(currentPath_);
            const bool one = !spec.isEmpty() && !spec.contains(QLatin1Char(',')) && !spec.contains(QLatin1Char('-'));
            QString pattern;
            if (one) {
                pattern = QFileDialog::getSaveFileName(
                    this, tr("Export page as"), doc.dir().filePath(tr("%1, page %2.png").arg(doc.completeBaseName(), spec)),
                    tr("PNG pictures (*.png)"));
            } else {
                const QString folder = QFileDialog::getExistingDirectory(this, tr("Export pages into"), doc.absolutePath());
                if (!folder.isEmpty()) {
                    pattern = QDir(folder).filePath(doc.completeBaseName() + QStringLiteral(", page %1.png"));
                }
            }
            if (pattern.isEmpty()) {
                return;
            }
            const QStringList written = exportImages(spec, dialog.dpi(), pattern);
            statusBar()->showMessage(tr("Exported %n picture(s).", nullptr, static_cast<int>(written.size())), 6000);
        });
        QAction* text = actions_->add({.id = QStringLiteral("exportText"), .text = tr("&Text…"),
                                       .icon = QStringLiteral("file-text"),
                                       .tip = tr("Save the document's text as a plain text file"),
                                       .enabledWhen = open, .group = file});
        connect(text, &QAction::triggered, this, [this] {
            ExportDialog dialog(this, ExportDialog::Kind::Text, pageCount_, std::max(0, view_->currentPage()));
            if (dialog.exec() != QDialog::Accepted) {
                return;
            }
            const QFileInfo doc(currentPath_);
            const QString path = QFileDialog::getSaveFileName(this, tr("Export text as"),
                                                              doc.dir().filePath(doc.completeBaseName() + QStringLiteral(".txt")),
                                                              tr("Text files (*.txt)"));
            if (!path.isEmpty()) {
                exportText(dialog.pages(), path);
            }
        });
    }
    const QString group = tr("Pages");
    const auto open = [this] { return pageCount_ > 0 && certAllows(4); };
    const auto add = [&](ActionRegistry::Spec spec, auto&& slot) {
        spec.certNeeds = 4;  // never in a certified document: its pages are fixed
        if (!spec.enabledWhen) {
            spec.enabledWhen = open;
        }
        spec.group = group;
        QAction* a = actions_->add(spec);
        connect(a, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return a;
    };
    QAction* grid = add({.id = QStringLiteral("pageGrid"), .text = tr("Page &Grid"),
                         .icon = QStringLiteral("layout-grid"),
                         .tip = tr("Show every page, to select, drag and drop; off to see the pages full size"),
                         .checkable = true,
                         .enabledWhen = [this] { return pageCount_ > 0; }},
                        [this](bool on) { showPageGrid(on); });
    grid->setProperty("certNeeds", 0);  // looking is always allowed
    grid->setProperty("certMenu", false);

    const auto turn = [this](int degrees) {
        const QVector<int> pages = targetPages();
        if (pages.isEmpty() || !confirmChangingSigned(tr("Turning pages"))) {
            return;
        }
        gridSelectionAfterEdit_ = pages;
        const QString spec = PageGrid::rangeSpec(pages);
        onWorker([=](RenderWorker* w) { w->rotatePages(spec, degrees); });
    };
    add({.id = QStringLiteral("pageRotateLeft"), .text = tr("Rotate &Left"), .icon = QStringLiteral("rotate-ccw"),
         .tip = tr("Turn the selected pages a quarter turn counter-clockwise, in the file")},
        [turn] { turn(-90); });
    add({.id = QStringLiteral("pageRotateRight"), .text = tr("Rotate &Right"), .icon = QStringLiteral("rotate-cw"),
         .tip = tr("Turn the selected pages a quarter turn clockwise, in the file")},
        [turn] { turn(90); });
    add({.id = QStringLiteral("pageDelete"), .text = tr("&Delete Pages"), .icon = QStringLiteral("trash-2"),
         .tip = tr("Remove the selected pages (Undo brings them back)")},
        [this] {
            const QVector<int> pages = targetPages();
            if (pages.isEmpty()) {
                return;
            }
            if (pages.size() >= pageCount_) {
                QMessageBox::information(this, tr("Delete pages"), tr("A document must keep at least one page."));
                return;
            }
            if (!confirmChangingSigned(tr("Deleting pages"))) {
                return;
            }
            gridSelectionAfterEdit_ = {std::min(pages.first(), pageCount_ - static_cast<int>(pages.size()) - 1)};
            const QString spec = PageGrid::rangeSpec(pages);
            onWorker([=](RenderWorker* w) { w->deletePages(spec); });
            statusBar()->showMessage(tr("Deleted %n page(s). Undo brings them back.", nullptr,
                                        static_cast<int>(pages.size())), 6000);
        });
    add({.id = QStringLiteral("pageInsertFile"), .text = tr("Insert Pages from &File…"),
         .icon = QStringLiteral("file-plus"), .tip = tr("Put the pages of another PDF after the selected page")},
        [this] {
            const QStringList paths = QFileDialog::getOpenFileNames(
                this, tr("Insert pages from"), QFileInfo(currentPath_).absolutePath(),
                tr("PDF documents (*.pdf)"));
            if (!paths.isEmpty()) {
                insertFilesAt(paths, pageGrid_->isVisible() ? pageGrid_->insertionPoint()
                                                            : std::max(0, view_->currentPage()) + 1);
            }
        });
    add({.id = QStringLiteral("pageInsertBlank"), .text = tr("Insert &Blank Page"), .icon = QStringLiteral("file"),
         .tip = tr("Add an empty page, the size of the one before it, after the selected page")},
        [this] {
            if (!confirmChangingSigned(tr("Inserting a page"))) {
                return;
            }
            const QVector<int> pages = targetPages();
            const int at = pages.isEmpty() ? pageCount_ : pages.last() + 1;
            const QSize like = view_->pageSizePoints(std::max(0, at - 1)).toSize();
            const QSizeF size = like.isEmpty() ? QSizeF(595, 842) : QSizeF(like);  // A4 if in doubt
            gridSelectionAfterEdit_ = {at};
            onWorker([=](RenderWorker* w) { w->insertBlankPage(at, size); });
        });
    add({.id = QStringLiteral("pageExtract"), .text = tr("E&xtract Pages…"), .icon = QStringLiteral("file-output"),
         .tip = tr("Save the selected pages as a new PDF; this document is not changed"),
         .enabledWhen = [this] { return pageCount_ > 0 && !fileToolBusy_; }},
        [this] {
            const QVector<int> pages = targetPages();
            // Extract reads the file on disk, so edits are settled first.
            if (pages.isEmpty() ||
                !resolveUnsaved([this] { actions_->find(QStringLiteral("pageExtract"))->trigger(); })) {
                return;
            }
            const QString spec = PageGrid::rangeSpec(pages);
            const QString suggested = QFileInfo(currentPath_).dir().filePath(
                tr("%1 (pages %2).pdf").arg(QFileInfo(currentPath_).completeBaseName(), spec));
            const QString output = QFileDialog::getSaveFileName(this, tr("Extract pages to"), suggested,
                                                                tr("PDF documents (*.pdf)"));
            if (output.isEmpty()) {
                return;
            }
            const QString input = currentPath_;
            runFileTool(tr("Extracting pages…"), [input, spec, output](FileTools* t, const QString& password) {
                t->split(input, password, {spec}, {output});
            });
        });
}

QVector<int> MainWindow::targetPages() const {
    if (pageCount_ <= 0) {
        return {};
    }
    if (viewStack_ != nullptr && viewStack_->currentWidget() == pageGrid_) {
        return pageGrid_->selectedPages();
    }
    return {std::max(0, view_->currentPage())};
}

void MainWindow::showPageGrid(bool grid) {
    if (viewStack_ == nullptr) {
        return;
    }
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
    if (QAction* toggle = actions_->find(QStringLiteral("pageGrid"))) {
        toggle->setChecked(grid);
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

bool MainWindow::confirmChangingSigned(const QString& what) {
    if (signatureCount_ == 0) {
        return true;
    }
    const auto answer = QMessageBox::question(
        this, tr("This document is signed"),
        tr("%1 is saved as a new revision after the %n signature(s). They stay intact, but each will "
           "say that the document was changed after it was signed.\n\nCarry on?",
           nullptr, signatureCount_)
            .arg(what),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Yes;
}

void MainWindow::insertFilesAt(const QStringList& paths, int at) {
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
            QMessageBox::warning(this, tr("Insert pages"),
                                 file.size() > kMaxBytes
                                     ? tr("“%1” is larger than 128 MB; combine it with Combine Files instead.")
                                           .arg(QFileInfo(path).fileName())
                                     : tr("“%1” could not be read.").arg(QFileInfo(path).fileName()));
            return;
        }
        const QByteArray data = file.readAll();
        onWorker([=](RenderWorker* w) { w->insertPages(where, data, QString()); });
    }
    statusBar()->showMessage(tr("Inserting %n file(s)…", nullptr, static_cast<int>(paths.size())), 4000);
}

// --- Export ------------------------------------------------------------------

QStringList MainWindow::exportImages(const QString& pages, int dpi, const QString& pattern) {
    QStringList written;
    std::vector<int> chosen;
    try {
        chosen = leht::page_set(pages.toStdString(), pageCount_);
    } catch (const leht::Error&) {
        return written;
    }
    auto* progress = new QProgressDialog(tr("Exporting pages…"), tr("Cancel"), 0,
                                         static_cast<int>(chosen.size()), this);
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

void MainWindow::exportText(const QString& pages, const QString& path) {
    auto once = std::make_shared<QMetaObject::Connection>();
    *once = connect(worker_, &RenderWorker::textReady, this,
                    [this, once, path](const QVector<int>& numbers, const QStringList& texts) {
        disconnect(*once);
        QFile file(path);
        if (numbers.isEmpty() || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, tr("Export text"), tr("Could not write “%1”.").arg(path));
            return;
        }
        // Pages apart by a form feed, as pdftotext does: a plain-text page break.
        file.write(texts.join(QLatin1Char('\f')).toUtf8());
        file.close();
        statusBar()->showMessage(tr("Exported the text of %n page(s).", nullptr, static_cast<int>(numbers.size())),
                                 6000);
    });
    onWorker([pages](RenderWorker* w) { w->extractText(pages); });
}

// --- Colours, first run, keyboard -------------------------------------------

QColor MainWindow::toolColor(const QString& toolId) const {
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

void MainWindow::showFirstRunHints() {
    if (!isShowingWelcome()) {
        return;
    }
    (void)FirstRunHints::showOnce(
        this,
        {{welcome_->findChild<QWidget*>(QStringLiteral("dropZone")), tr("Open a PDF"),
          tr("Click Open, or drop PDFs and pictures here. Several at once can be combined into one.")},
         {welcome_->findChild<QWidget*>(QStringLiteral("task_sign")), tr("Start from a task"),
          tr("Sign, fill in a form, combine files, make a scan searchable: pick what you want done, "
             "then the file.")},
         {menuBar(), tr("Everything is in the menus"),
          tr("Each command has its place in the menu bar, with its shortcut. Edit → Preferences holds "
             "your name for comments and your signing defaults.")}},
        QLatin1String(FirstRunHints::kWelcomeKey));
}

void MainWindow::focusRegion(int step) {
    // The parts of the window, in reading order; only those showing count.
    QVector<QWidget*> regions;
    if (auto* bar = findChild<QToolBar*>(QStringLiteral("mainBar")); bar != nullptr && bar->isVisible()) {
        regions << bar;
    }
    if (!isShowingWelcome()) {
        regions << modes_;
        if (sidebar_->isVisible()) {
            regions << sidebar_;
        }
        regions << viewStack_->currentWidget();
    } else {
        regions << welcome_;
    }
    if (regions.isEmpty()) {
        return;
    }
    int at = -1;
    for (int i = 0; i < regions.size(); ++i) {
        if (QWidget* f = QApplication::focusWidget(); f != nullptr && (f == regions[i] || regions[i]->isAncestorOf(f))) {
            at = i;
        }
    }
    const int n = static_cast<int>(regions.size());
    const int next = ((at < 0 ? (step > 0 ? -1 : 0) : at) + step + n) % n;
    QWidget* target = regions[next];
    // Into the region: its first focusable child, or itself.
    QWidget* focus = target->focusPolicy() != Qt::NoFocus ? target : nullptr;
    if (focus == nullptr) {
        for (QWidget* child : target->findChildren<QWidget*>()) {
            if (child->isVisible() && child->isEnabled() && (child->focusPolicy() & Qt::TabFocus) != 0) {
                focus = child;
                break;
            }
        }
    }
    (focus != nullptr ? focus : target)->setFocus(Qt::TabFocusReason);
}
