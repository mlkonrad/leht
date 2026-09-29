// SPDX-License-Identifier: AGPL-3.0-or-later
// MainWindow's structure: the toolbar, the sidebar and its panels, the mode
// bar, the welcome view, the menus -- and what the welcome view and dropped
// files ask for. The document logic stays in main_window.cpp.
#include "main_window.hpp"

#include "actions.hpp"
#include "color_swatches.hpp"
#include "document_tab.hpp"
#include "form_panel.hpp"
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

namespace {

/// The tools that draw in a colour of the user's choosing.
bool colourable(const QString& id) {
    return id == QLatin1String("toolHighlight") || id == QLatin1String("toolUnderline") ||
           id == QLatin1String("toolStrike") || id == QLatin1String("toolDraw") ||
           id == QLatin1String("toolText");
}

}  // namespace

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
        DocumentTab* d = current();
        // Back on a document's own mode (loadChrome): it is already as it was.
        if (loadingChrome_ || d == nullptr) {
            return;
        }
        d->setMode(mode);
        // Pages mode opens on the page grid; every other mode reads.
        d->showPageGrid(mode == QLatin1String("pages"));
        if (QAction* grid = actions_->find(QStringLiteral("pageGrid"))) {
            grid->setChecked(d->isShowingGrid());
        }
        // A tool the new mode does not show is put down.
        if (!modeTools_.value(mode).contains(tools_->checkedAction())) {
            tools_->actions().first()->trigger();
        }
        // The panel that goes with the mode, if the sidebar is open.
        Sidebar* sidebar = d->sidebar();
        if (sidebar->isExpanded()) {
            if (mode == QLatin1String("comment")) {
                sidebar->showPanel(QStringLiteral("comments"));
            } else if (mode == QLatin1String("sign") && sidebar->isPanelAvailable(QStringLiteral("form"))) {
                sidebar->showPanel(QStringLiteral("form"));
            }
        }
    });
    // The comment tools' colour, at the end of their row; it follows the tool.
    swatches_ = new ColorSwatches(this);
    swatches_->setToolTip(tr("The colour the chosen tool draws in"));
    modes_->setModeExtra(QStringLiteral("comment"), swatches_);
    connect(tools_, &QActionGroup::triggered, this, &MainWindow::syncSwatches);
    connect(swatches_, &ColorSwatches::colorChosen, this, [this](const QColor& colour) {
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

    // The documents, one DocumentTab each, under the mode bar they share.
    documentStack_ = new QStackedWidget(this);
    documentStack_->setObjectName(QStringLiteral("documents"));
    documentPage_ = new QWidget(this);
    documentPage_->setObjectName(QStringLiteral("documentPage"));
    auto* column = new QVBoxLayout(documentPage_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(modes_);
    column->addWidget(documentStack_, 1);

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

void MainWindow::syncSwatches() {
    QAction* tool = tools_->checkedAction();
    const QString id = tool != nullptr ? tool->objectName() : QString();
    swatches_->setEnabled(colourable(id));
    if (colourable(id)) {
        swatches_->setColor(toolColor(id));
    }
    if (PageView* v = view()) {
        v->setNewTextColor(toolColor(QStringLiteral("toolText")));
    }
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
                           "flattenForm", "-", "updateTrustedList", "trustedCertificates"})},
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
    connect(&dialog, &PreferencesDialog::trustChanged, this, [] {
        // Every open document's signatures are judged again, by the new list.
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* window = qobject_cast<MainWindow*>(w)) {
                for (DocumentTab* d : window->documents()) {
                    d->recheckSignatures();
                }
            }
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
    hideFindBar();
    applyCertification();  // nothing certified: every mode on, for the next document
    updatePageControls();
    updateZoomLabel();
    updateTitle();
    welcome_->setFocus();
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
    DocumentTab* d = current();
    const QString path = d != nullptr && d->isOpen() ? d->path() : QString();
    const bool startsWithOpen = !path.isEmpty() && initial.value(0) == path;
    CombineDialog dialog(this, startsWithOpen ? path : QString());
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
    if (path.isEmpty() || !inputs.contains(path) || d->resolveUnsaved(go)) {
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
    DocumentTab* d = addDocument();
    d->setPendingTask(task);
    d->openPath(path);
}

void MainWindow::runPendingTask(DocumentTab* tab) {
    const QString task = tab->takePendingTask();
    if (task.isEmpty() || !tab->isOpen()) {
        return;
    }
    if (tab != current()) {
        setCurrent(tab);
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
        if (tab->sidebar()->isPanelAvailable(QStringLiteral("form"))) {
            tab->sidebar()->showPanel(QStringLiteral("form"));
        } else {
            // The fields arrive after the pages; say so only if there are none.
            QPointer<DocumentTab> alive(tab);
            QTimer::singleShot(1500, this, [this, alive] {
                if (alive != nullptr && alive == current() && alive->isOpen() &&
                    !alive->sidebar()->isPanelAvailable(QStringLiteral("form"))) {
                    statusBar()->showMessage(tr("This document has no form fields to fill in."), 8000);
                }
            });
        }
    } else if (task == QLatin1String(WelcomeView::kOcr)) {
        tab->recognizeText();
    } else if (task == QLatin1String(WelcomeView::kReduce)) {
        if (QAction* reduce = actions_->find(QStringLiteral("reduceFileSize"))) {
            reduce->trigger();
        }
    }
    // kVerify is answered by DocumentTab::onSignaturesReady (verifyPending_).
}

// --- Pages mode: organising pages ------------------------------------------

void MainWindow::buildPageActions() {
    const auto doc = [this](auto fn) {
        return [this, fn] {
            if (DocumentTab* d = current()) {
                fn(d);
            }
        };
    };
    {
        // File > Export: not a page edit, but it lives with the page commands.
        const QString file = tr("File");
        const auto open = [this] { return current() != nullptr && current()->isOpen(); };
        QAction* images = actions_->add({.id = QStringLiteral("exportImages"), .text = tr("Pages as &Images…"),
                                         .icon = QStringLiteral("image"),
                                         .tip = tr("Save pages as PNG pictures"), .enabledWhen = open, .group = file});
        connect(images, &QAction::triggered, this, doc([](DocumentTab* d) { d->exportImagesDialog(); }));
        QAction* text = actions_->add({.id = QStringLiteral("exportText"), .text = tr("&Text…"),
                                       .icon = QStringLiteral("file-text"),
                                       .tip = tr("Save the document's text as a plain text file"),
                                       .enabledWhen = open, .group = file});
        connect(text, &QAction::triggered, this, doc([](DocumentTab* d) { d->exportTextDialog(); }));
    }
    const QString group = tr("Pages");
    const auto open = [this] { return current() != nullptr && current()->isOpen() && current()->certAllows(4); };
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
                         .enabledWhen = [this] { return current() != nullptr && current()->isOpen(); }},
                        [this](bool on) {
                            if (DocumentTab* d = current()) {
                                d->showPageGrid(on);
                            }
                        });
    grid->setProperty("certNeeds", 0);  // looking is always allowed
    grid->setProperty("certMenu", false);

    add({.id = QStringLiteral("pageRotateLeft"), .text = tr("Rotate &Left"), .icon = QStringLiteral("rotate-ccw"),
         .tip = tr("Turn the selected pages a quarter turn counter-clockwise, in the file")},
        doc([](DocumentTab* d) { d->rotatePages(-90); }));
    add({.id = QStringLiteral("pageRotateRight"), .text = tr("Rotate &Right"), .icon = QStringLiteral("rotate-cw"),
         .tip = tr("Turn the selected pages a quarter turn clockwise, in the file")},
        doc([](DocumentTab* d) { d->rotatePages(90); }));
    add({.id = QStringLiteral("pageDelete"), .text = tr("&Delete Pages"), .icon = QStringLiteral("trash-2"),
         .tip = tr("Remove the selected pages (Undo brings them back)")},
        doc([](DocumentTab* d) { d->deletePages(); }));
    add({.id = QStringLiteral("pageInsertFile"), .text = tr("Insert Pages from &File…"),
         .icon = QStringLiteral("file-plus"), .tip = tr("Put the pages of another PDF after the selected page")},
        doc([](DocumentTab* d) { d->insertFileDialog(); }));
    add({.id = QStringLiteral("pageInsertBlank"), .text = tr("Insert &Blank Page"), .icon = QStringLiteral("file"),
         .tip = tr("Add an empty page, the size of the one before it, after the selected page")},
        doc([](DocumentTab* d) { d->insertBlankPage(); }));
    add({.id = QStringLiteral("pageExtract"), .text = tr("E&xtract Pages…"), .icon = QStringLiteral("file-output"),
         .tip = tr("Save the selected pages as a new PDF; this document is not changed"),
         .enabledWhen = [this] { return current() != nullptr && current()->isOpen() && !fileToolBusy_; }},
        [this] {
            DocumentTab* d = current();
            if (d == nullptr) {
                return;
            }
            const QVector<int> pages = d->targetPages();
            // Extract reads the file on disk, so edits are settled first.
            if (pages.isEmpty() ||
                !d->resolveUnsaved([this] { actions_->find(QStringLiteral("pageExtract"))->trigger(); })) {
                return;
            }
            const QString spec = PageGrid::rangeSpec(pages);
            const QString suggested = QFileInfo(d->path()).dir().filePath(
                tr("%1 (pages %2).pdf").arg(QFileInfo(d->path()).completeBaseName(), spec));
            const QString output = QFileDialog::getSaveFileName(this, tr("Extract pages to"), suggested,
                                                                tr("PDF documents (*.pdf)"));
            if (output.isEmpty()) {
                return;
            }
            const QString input = d->path();
            runFileTool(tr("Extracting pages…"), [input, spec, output](FileTools* t, const QString& password) {
                t->split(input, password, {spec}, {output});
            });
        });
}

// --- Colours, first run, keyboard -------------------------------------------

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
    if (DocumentTab* d = current(); d != nullptr && !isShowingWelcome()) {
        regions << modes_;
        if (d->sidebar()->isVisible()) {
            regions << d->sidebar();
        }
        regions << d->pageArea();
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
