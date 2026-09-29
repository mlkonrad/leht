// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"

#include "actions.hpp"
#include "color_swatches.hpp"
#include "document_tab.hpp"
#include "file_tools.hpp"
#include "file_tools_dialogs.hpp"
#include "first_run_hints.hpp"
#include "form_panel.hpp"
#include "icons.hpp"
#include "mode_bar.hpp"
#include "outline_model.hpp"
#include "page_grid.hpp"
#include "page_view.hpp"
#include "preferences.hpp"
#include "protect_dialog.hpp"
#include "recent_files.hpp"
#include "render_worker.hpp"
#include "sidebar.hpp"
#include "welcome_view.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>

#include <memory>

namespace {

/// Whether `window` shows an open document whose certification allows what
/// needs `needs` (see ActionRegistry::Spec::certNeeds).
bool docAllows(const MainWindow* window, int needs) {
    const DocumentTab* d = window->current();
    return d != nullptr && d->isOpen() && d->certAllows(needs);
}

}  // namespace

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

    actions_ = new ActionRegistry(this);
    buildActions();
    buildEditActions();
    buildFileTools();
    buildPageActions();
    buildMainToolbar();
    buildLayout();
    buildMenus();
    applyAppearance();
    showWelcome();
}

MainWindow::~MainWindow() {
    // Each document waits for its own worker thread as it goes.
    const QList<DocumentTab*> tabs = tabs_;
    for (DocumentTab* d : tabs) {
        release(d);
        delete d;
    }
    fileTools_->cancel();
    fileToolsThread_.quit();
    fileToolsThread_.wait();
}

// --- Documents -------------------------------------------------------------------

DocumentTab* MainWindow::current() const { return current_; }

PageView* MainWindow::view() const { return current_ != nullptr ? current_->view() : nullptr; }

RenderWorker* MainWindow::worker() const { return current_ != nullptr ? current_->worker() : nullptr; }

Sidebar* MainWindow::sidebar() const { return current_ != nullptr ? current_->sidebar() : nullptr; }

int MainWindow::pendingRedactions() const { return current_ != nullptr ? current_->pendingRedactions() : 0; }

bool MainWindow::applyRedactions(bool confirm) { return current_ != nullptr && current_->applyRedactions(confirm); }

QStringList MainWindow::exportImages(const QString& pages, int dpi, const QString& pattern) {
    return current_ != nullptr ? current_->exportImages(pages, dpi, pattern) : QStringList();
}

void MainWindow::exportText(const QString& pages, const QString& path) {
    if (current_ != nullptr) {
        current_->exportText(pages, path);
    }
}

bool MainWindow::save() { return current_ != nullptr && current_->save(); }

bool MainWindow::saveAs() { return current_ != nullptr && current_->saveAs(); }

bool MainWindow::isModified() const { return current_ != nullptr && current_->isModified(); }

bool MainWindow::printDocument(QPrinter& printer, int fromPage, int toPage) {
    return current_ != nullptr && current_->printDocument(printer, fromPage, toPage);
}

QColor MainWindow::toolColor(const QString& toolId) const { return DocumentTab::toolColor(toolId); }

DocumentTab* MainWindow::addDocument() {
    auto* tab = new DocumentTab();
    adopt(tab);
    return tab;
}

void MainWindow::adopt(DocumentTab* tab) {
    tab->setParent(documentStack_);
    documentStack_->addWidget(tab);
    tabs_.push_back(tab);
    syncingTabs_ = true;
    tabBar_->addTab(QString());
    syncingTabs_ = false;
    updateTabText(tab);

    // Everything the document says reaches the window's chrome only while it
    // is the one shown; the rest of the time it keeps its state to itself.
    QList<QMetaObject::Connection>& links = links_[tab];
    const auto shown = [this, tab] { return tab == current_; };
    links << connect(tab, &DocumentTab::statusMessage, this, [this, shown](const QString& text, int timeout) {
        if (!shown()) {
            return;
        }
        if (text.isEmpty()) {
            statusBar()->clearMessage();
        } else {
            statusBar()->showMessage(text, timeout);
        }
    });
    links << connect(tab, &DocumentTab::titleChanged, this, [this, tab, shown] {
        updateTabText(tab);
        if (shown()) {
            updateTitle();
        }
    });
    links << connect(tab, &DocumentTab::stateChanged, this, [this, shown] {
        if (shown()) {
            actions_->refresh();
            updatePageControls();
        }
    });
    links << connect(tab, &DocumentTab::certificationChanged, this, [this, shown] {
        if (shown()) {
            applyCertification();
        }
    });
    links << connect(tab, &DocumentTab::opening, this, [this, shown] {
        if (shown()) {
            stack_->setCurrentWidget(documentPage_);
            loadChrome();
        }
    });
    links << connect(tab, &DocumentTab::opened, this, [this, tab, shown] {
        if (shown()) {
            loadChrome();
        }
        if (!tab->path().isEmpty()) {
            recent::add(tab->path());
            // The document tour, once, the first time a document opens.
            QTimer::singleShot(400, this, [this] {
                DocumentTab* d = current();
                if (d == nullptr || !d->isOpen() || !isVisible()) {
                    return;
                }
                (void)FirstRunHints::showOnce(
                    this,
                    {{modes_, tr("Pick what you are doing"),
                      tr("Read, Comment, Fill & Sign, Pages or Redact: each shows only its own tools. Every "
                         "command is also in the menus.")},
                     {d->sidebar(), tr("The sidebar"),
                      tr("Pages, outline, comments, form fields and signatures. Click the open tab again to "
                         "fold it away; F9 shows or hides it.")},
                     {d->view(), tr("Get around"),
                      tr("Ctrl+F finds text, Ctrl+plus and minus zoom, F6 moves between the parts of the "
                         "window. Help → Keyboard Shortcuts lists them all.")}},
                    QLatin1String(FirstRunHints::kDocumentKey));
            });
        }
        QPointer<DocumentTab> alive(tab);
        QTimer::singleShot(0, this, [this, alive] {
            if (alive != nullptr) {
                runPendingTask(alive);
            }
        });
    });
    links << connect(tab, &DocumentTab::closed, this, [this, tab] {
        // Not while the tab is still in its own signal: after it.
        QPointer<DocumentTab> alive(tab);
        QTimer::singleShot(0, this, [this, alive] {
            if (alive != nullptr && tabs_.contains(alive)) {
                closeTab(alive, false);
            }
        });
    });
    links << connect(tab, &DocumentTab::pageChanged, this, [this, shown] {
        if (shown()) {
            updatePageControls();
        }
    });
    links << connect(tab, &DocumentTab::zoomChanged, this, [this, shown] {
        if (shown()) {
            updateZoomLabel();
        }
    });
    links << connect(tab, &DocumentTab::matchNavigated, this, [this, shown](int index, int total) {
        if (shown()) {
            findLabel_->setText(total <= 0 ? tr("no matches") : tr("%1 of %2").arg(index + 1).arg(total));
        }
    });
    links << connect(tab, &DocumentTab::toolRefused, this, [this, shown] {
        if (shown() && !tools_->actions().isEmpty()) {
            tools_->actions().first()->setChecked(true);  // Select
            syncSwatches();
        }
    });
    links << connect(tab, &DocumentTab::modeRequested, this, [this, shown](const QString& mode) {
        if (shown()) {
            modes_->setMode(mode);
        }
    });
    links << connect(tab, &DocumentTab::placeSignatureRequested, this, [this, shown] {
        QAction* sign = actions_->find(QStringLiteral("toolSign"));
        if (shown() && sign != nullptr && sign->isEnabled()) {
            modes_->setMode(QStringLiteral("sign"));
            sign->trigger();
            statusBar()->showMessage(tr("Drag a box where the signature should go."), 12000);
        }
    });
    links << connect(tab, &DocumentTab::flattenRequested, this, [this] {
        if (QAction* flatten = actions_->find(QStringLiteral("flattenForm")); flatten != nullptr && flatten->isEnabled()) {
            flatten->trigger();
        }
    });
    links << connect(tab, &DocumentTab::trustSettingsRequested, this,
                     [this] { openPreferences(static_cast<int>(PreferencesDialog::Page::Trust)); });
    links << connect(tab->sidebar(), &Sidebar::expandedChanged, this, [this, shown](bool expanded) {
        if (shown()) {
            actions_->find(QStringLiteral("toggleSidebar"))->setChecked(expanded);
        }
    });
    // The grid's context menu offers this window's page commands.
    tab->pageGrid()->setContextActions({actions_->find(QStringLiteral("pageRotateLeft")),
                                        actions_->find(QStringLiteral("pageRotateRight")), nullptr,
                                        actions_->find(QStringLiteral("pageInsertFile")),
                                        actions_->find(QStringLiteral("pageInsertBlank")),
                                        actions_->find(QStringLiteral("pageExtract")), nullptr,
                                        actions_->find(QStringLiteral("pageDelete"))});

    stack_->setCurrentWidget(documentPage_);
    setCurrent(tab);
}

void MainWindow::release(DocumentTab* tab) {
    for (const QMetaObject::Connection& c : links_.take(tab)) {
        disconnect(c);
    }
    tab->pageGrid()->setContextActions({});
    discarded_.remove(tab);
}

void MainWindow::closeTab(DocumentTab* tab, bool ask) {
    if (tab == nullptr || !tabs_.contains(tab)) {
        return;
    }
    if (ask) {
        QPointer<DocumentTab> alive(tab);
        if (tab != current_ && tab->isModified()) {
            setCurrent(tab);  // the question is about the document in view
        }
        if (!tab->resolveUnsaved([this, alive] {
                if (alive != nullptr) {
                    closeTab(alive, false);
                }
            })) {
            return;
        }
    }
    detach(tab);
    // Out of the window at once, so nothing finds it; its worker thread is
    // waited for when it is deleted.
    tab->hide();
    tab->setParent(nullptr);
    tab->deleteLater();
}

void MainWindow::detach(DocumentTab* tab) {
    const qsizetype at = tabs_.indexOf(tab);
    if (at < 0) {
        return;
    }
    if (tab == current_) {
        hideFindBar();
    }
    release(tab);
    tabs_.removeAt(at);
    syncingTabs_ = true;
    tabBar_->removeTab(static_cast<int>(at));
    syncingTabs_ = false;
    documentStack_->removeWidget(tab);
    if (tab == current_) {
        current_ = nullptr;
        if (tabs_.isEmpty()) {
            showWelcome();
        } else {
            setCurrent(tabs_.value(std::min(at, tabs_.size() - 1)));
        }
    }
}

void MainWindow::closeTabs(const QList<QPointer<DocumentTab>>& tabs) {
    for (qsizetype i = 0; i < tabs.size(); ++i) {
        DocumentTab* tab = tabs[i];
        if (tab == nullptr || !tabs_.contains(tab)) {
            continue;
        }
        if (tab->isModified()) {
            setCurrent(tab);
            // A save it starts closes this one and goes on with the rest.
            if (!tab->resolveUnsaved([this, rest = tabs.mid(i)] { closeTabs(rest); })) {
                return;
            }
        }
        closeTab(tab, false);
    }
}

MainWindow* MainWindow::moveTabToNewWindow(DocumentTab* tab) {
    if (tab == nullptr || !tabs_.contains(tab) || tabs_.size() < 2) {
        return nullptr;
    }
    detach(tab);
    // The tab keeps its worker thread and everything it holds; only the
    // window around it changes.
    MainWindow* window = newWindow();
    window->adopt(tab);
    return window;
}

void MainWindow::updateTabText(DocumentTab* tab) {
    const qsizetype at = tabs_.indexOf(tab);
    if (at < 0) {
        return;
    }
    const int i = static_cast<int>(at);
    const QString text = tab->title().isEmpty() ? tr("Opening…") : tab->tabText();
    tabBar_->setTabText(i, text);
    tabBar_->setTabToolTip(i, tab->path().isEmpty() ? text : tab->path());
    tabBar_->setAccessibleTabName(i, tab->isModified() ? tr("%1, modified").arg(tab->title()) : tab->title());
}

void MainWindow::showTabMenu(int index, const QPoint& globalPos) {
    QPointer<DocumentTab> tab = tabs_.value(index);
    if (tab == nullptr) {
        return;
    }
    QMenu menu(this);
    menu.addAction(icons::named(QStringLiteral("x")), tr("&Close"), this, [this, tab] { closeTab(tab); });
    QAction* others = menu.addAction(tr("Close &Other Tabs"), this, [this, tab] {
        QList<QPointer<DocumentTab>> list;
        for (DocumentTab* d : tabs_) {
            if (d != tab) {
                list << d;
            }
        }
        closeTabs(list);
    });
    others->setEnabled(tabs_.size() > 1);
    QAction* right = menu.addAction(tr("Close Tabs to the &Right"), this, [this, tab] {
        QList<QPointer<DocumentTab>> list;
        for (qsizetype i = tabs_.indexOf(tab) + 1; i < tabs_.size(); ++i) {
            list << tabs_[i];
        }
        closeTabs(list);
    });
    right->setEnabled(index < tabs_.size() - 1);
    menu.addSeparator();
    QAction* move = menu.addAction(icons::named(QStringLiteral("files")), tr("&Move to New Window"), this,
                                   [this, tab] { (void)moveTabToNewWindow(tab); });
    move->setObjectName(QStringLiteral("moveTabToNewWindow"));
    move->setEnabled(tabs_.size() > 1);
    QAction* copy = menu.addAction(icons::named(QStringLiteral("copy")), tr("Copy File &Path"), this,
                                   [tab] { QGuiApplication::clipboard()->setText(tab->path()); });
    copy->setEnabled(!tab->path().isEmpty());
    menu.exec(globalPos);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // A middle click closes a tab, as in a web browser.
    if (watched == tabBar_ && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            if (DocumentTab* tab = tabs_.value(tabBar_->tabAt(mouse->position().toPoint()))) {
                closeTab(tab);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setCurrent(DocumentTab* tab) {
    if (tab != current_ && current_ != nullptr && findBar_->isVisible()) {
        hideFindBar();  // a search belongs to the document it was made in
    }
    current_ = tab;
    if (tab == nullptr) {
        return;
    }
    syncingTabs_ = true;
    tabBar_->setCurrentIndex(static_cast<int>(tabs_.indexOf(tab)));
    syncingTabs_ = false;
    documentStack_->setCurrentWidget(tab);
    loadChrome();
}

void MainWindow::loadChrome() {
    DocumentTab* d = current_;
    if (d == nullptr) {
        return;
    }
    // The shared controls, as this document was left: its mode, its tool.
    loadingChrome_ = true;
    modes_->setMode(d->mode());
    if (QAction* tool = actions_->find(d->tool()); tool != nullptr && tools_->actions().contains(tool)) {
        tool->setChecked(true);
    }
    loadingChrome_ = false;
    applyCertification();
    if (QAction* grid = actions_->find(QStringLiteral("pageGrid"))) {
        grid->setChecked(d->isShowingGrid());
    }
    actions_->find(QStringLiteral("toggleSidebar"))->setChecked(d->sidebar()->isExpanded());
    syncSwatches();
    updatePageControls();
    updateZoomLabel();
    updateTitle();
}

void MainWindow::applyCertification() {
    // What a certified document still lets one do: each tool and command
    // carries the level from which it is allowed (2 form filling and signing,
    // 3 annotations, 4 never -- page content is fixed at every level), and the
    // registry's enabledWhen asks the document's certAllows(). Leht does not
    // break a certification by a click.
    DocumentTab* d = current_;
    const int level = d != nullptr ? d->certLevel() : 0;
    const auto allows = [level](int needs) { return needs == 0 || level == 0 || level >= needs; };
    for (QAction* a : findChildren<QAction*>()) {
        const int needs = a->property("certNeeds").toInt();
        if (needs == 0) {
            continue;
        }
        const QString base = a->property("baseTip").toString();
        a->setToolTip(allows(needs) ? base
                                    : tr("%1\n\nNot available: the document is certified, %2.")
                                          .arg(base, certificationWords(level)));
    }
    actions_->refresh();
    const QString why = tr("Not available: the document is certified, %1.").arg(certificationWords(level));
    modes_->setModeEnabled(QStringLiteral("comment"), allows(3), why);
    modes_->setModeEnabled(QStringLiteral("sign"), allows(2), why);
    modes_->setModeEnabled(QStringLiteral("pages"), allows(4), why);
    modes_->setModeEnabled(QStringLiteral("redact"), allows(4), why);
    QAction* tool = tools_->checkedAction();
    if (tool != nullptr && !tool->isEnabled() && d != nullptr && d->isOpen()) {
        tools_->actions().first()->trigger();  // back to Select
    }
}

void MainWindow::updateTitle() {
    const DocumentTab* d = current_;
    if (d == nullptr || d->title().isEmpty()) {
        setWindowTitle(tr("Leht"));
        return;
    }
    setWindowTitle(tr("%1%2 — Leht").arg(d->title(), d->isModified() ? QStringLiteral(" *") : QString()));
}

void MainWindow::updatePageControls() {
    const DocumentTab* d = current_;
    const int count = d != nullptr ? d->pageCount() : 0;
    const int page = d != nullptr ? d->currentPage() : -1;
    pageSpin_->setMaximum(qMax(1, count));
    pageSpin_->setEnabled(count > 0);
    if (count > 0 && page >= 0) {
        pageLabel_->setText(tr("/ %1").arg(count));
        syncingSpin_ = true;
        pageSpin_->setValue(page + 1);
        syncingSpin_ = false;
    } else {
        pageLabel_->clear();
    }
}

void MainWindow::updateZoomLabel() {
    const PageView* v = view();
    zoomLabel_->setText(v != nullptr ? tr("%1%").arg(qRound(v->zoom() * 100.0)) : QString());
}

void MainWindow::goToPageFromSpin() {
    if (syncingSpin_ || view() == nullptr) {
        return;  // the change came from scrolling, not the user
    }
    view()->goToPage(pageSpin_->value() - 1);
}

void MainWindow::showFindBar() {
    findBar_->show();
    findEscape_->setEnabled(true);
    findEdit_->setFocus();
    findEdit_->selectAll();
}

void MainWindow::hideFindBar() {
    findBar_->hide();
    findEscape_->setEnabled(false);
    if (DocumentTab* d = current_) {
        d->cancelSearch();
        d->view()->setFocus();
    }
}

void MainWindow::runSearch() {
    DocumentTab* d = current_;
    if (d == nullptr) {
        return;
    }
    const QString needle = findEdit_->text();
    findLabel_->setText(needle.isEmpty() ? QString() : tr("searching…"));
    d->search(needle);
}

// --- Opening and closing ---------------------------------------------------------

QString MainWindow::askOpenPath() {
    const QString path = current_ != nullptr ? current_->path() : QString();
    return QFileDialog::getOpenFileName(this, tr("Open PDF"),
                                        path.isEmpty() ? QString() : QFileInfo(path).absolutePath(),
                                        tr("PDF documents (*.pdf);;All files (*)"));
}

void MainWindow::openDialog() {
    const QString path = askOpenPath();
    if (!path.isEmpty()) {
        openDocument(path);
    }
}

MainWindow* MainWindow::newWindow() {
    auto* window = new MainWindow();
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->resize(size());
    window->move(pos() + QPoint(32, 32));  // beside this one, not on top of it
    window->show();
    return window;
}

MainWindow* MainWindow::openDocument(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    // Already open somewhere: its tab comes forward, rather than a copy.
    for (QWidget* w : QApplication::topLevelWidgets()) {
        auto* other = qobject_cast<MainWindow*>(w);
        if (other == nullptr || !other->isVisible()) {
            continue;
        }
        for (DocumentTab* d : other->tabs_) {
            if (d->path() == absolute) {
                other->setCurrent(d);
                if (other != this) {
                    other->bringForward();
                }
                return other;
            }
        }
    }
    addDocument()->openPath(absolute);
    return this;
}

namespace {

/// The window activated last; see MainWindow::changeEvent.
QPointer<MainWindow> gLastActive;

}  // namespace

MainWindow* MainWindow::lastActive() {
    if (gLastActive != nullptr && gLastActive->isVisible()) {
        return gLastActive;
    }
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<MainWindow*>(w); window != nullptr && window->isVisible()) {
            return window;
        }
    }
    auto* window = new MainWindow();
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    return window;
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        gLastActive = this;
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::bringForward() {
    if (isMinimized()) {
        setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    }
    show();
    raise();
    activateWindow();
}

void MainWindow::openHandedOver(const QStringList& paths, const QByteArray& activationToken) {
    MainWindow* target = lastActive();
    MainWindow* shown = target;
    for (const QString& path : paths) {
        shown = target->openDocument(path);
    }
    // On Wayland a window may take the focus only with a token from the
    // launch that asked for it; Qt's activation request uses the one in the
    // environment. Without it the compositor marks the window as wanting
    // attention instead.
    if (!activationToken.isEmpty()) {
        qputenv("XDG_ACTIVATION_TOKEN", activationToken);
    }
    shown->bringForward();
}

void MainWindow::openPath(const QString& path) {
    DocumentTab* d = current_ != nullptr ? current_ : addDocument();
    d->openPath(path);
}

void MainWindow::closeDocument() {
    if (current_ != nullptr) {
        closeTab(current_);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Each document with unsaved edits comes forward in turn and asks. A save
    // it starts ends the question for now; the close goes on when it lands.
    if (!std::exchange(resumingClose_, false)) {
        discarded_.clear();
    }
    const QList<DocumentTab*> tabs = tabs_;
    for (DocumentTab* d : tabs) {
        if (!d->isModified() || discarded_.contains(d)) {
            continue;
        }
        setCurrent(d);
        if (!d->resolveUnsaved([this] {
                resumingClose_ = true;
                close();
            })) {
            event->ignore();
            return;
        }
        discarded_.insert(d);
    }
    event->accept();
}

// --- Actions -----------------------------------------------------------------------

void MainWindow::buildActions() {
    using Spec = ActionRegistry::Spec;
    const auto open = [this] { return current_ != nullptr && current_->isOpen(); };
    const auto add = [this](Spec spec, auto&& slot) {
        QAction* a = actions_->add(spec);
        connect(a, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return a;
    };
    // An action on the current document, if there is one.
    const auto doc = [this](auto fn) {
        return [this, fn] {
            if (DocumentTab* d = current_) {
                fn(d);
            }
        };
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
         .tip = tr("Close the document"), .enabledWhen = [this] { return current_ != nullptr; }, .group = file},
        &MainWindow::closeDocument);
    add({.id = QStringLiteral("save"), .text = tr("&Save"), .icon = QStringLiteral("save"),
         .themeIcon = QStringLiteral("document-save"), .shortcuts = {QKeySequence::Save},
         .tip = tr("Save your changes to this file"), .enabledWhen = open, .group = file},
        doc([](DocumentTab* d) { (void)d->save(); }));
    add({.id = QStringLiteral("saveAs"), .text = tr("Save &As…"), .icon = QStringLiteral("save-all"),
         .themeIcon = QStringLiteral("document-save-as"), .shortcuts = {QKeySequence::SaveAs},
         .tip = tr("Save a copy under another name"), .enabledWhen = open, .group = file},
        doc([](DocumentTab* d) { (void)d->saveAs(); }));
    add({.id = QStringLiteral("properties"), .text = tr("Document P&roperties…"), .icon = QStringLiteral("file-text"),
         .themeIcon = QStringLiteral("document-properties"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_D)},
         .tip = tr("Title, author and keywords, and what the file is"), .enabledWhen = open, .group = file},
        doc([](DocumentTab* d) { d->showProperties(); }));
    add({.id = QStringLiteral("print"), .text = tr("&Print…"), .icon = QStringLiteral("printer"),
         .themeIcon = QStringLiteral("document-print"), .shortcuts = {QKeySequence::Print},
         .enabledWhen = open, .group = file},
        doc([](DocumentTab* d) { d->printDialog(); }));
    // Through close(), so unsaved edits are asked about.
    add({.id = QStringLiteral("quit"), .text = tr("&Quit"), .icon = QStringLiteral("log-out"),
         .themeIcon = QStringLiteral("application-exit"), .shortcuts = {QKeySequence::Quit}, .group = file},
        [] { QApplication::closeAllWindows(); });  // each asks about its own unsaved edits
    add({.id = QStringLiteral("newWindow"), .text = tr("&New Window"), .icon = QStringLiteral("files"),
         .themeIcon = QStringLiteral("window-new"), .shortcuts = {QKeySequence::New},
         .tip = tr("Another window, for another document"), .group = file},
        [this] { (void)newWindow(); });
    add({.id = QStringLiteral("closeWindow"), .text = tr("Close &Window"),
         .shortcuts = {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W)}, .group = file},
        [this] { close(); });

    // Tabs: one per open document.
    const auto step = [this](int by) {
        return [this, by] {
            const qsizetype n = tabs_.size();
            if (n > 1 && current_ != nullptr) {
                setCurrent(tabs_.value((tabs_.indexOf(current_) + by + n) % n));
            }
        };
    };
    const auto several = [this] { return tabs_.size() > 1; };
    add({.id = QStringLiteral("nextTab"), .text = tr("Ne&xt Tab"), .icon = QStringLiteral("chevron-right"),
         .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)},
         .tip = tr("Show the next open document"), .enabledWhen = several, .group = view},
        step(+1));
    add({.id = QStringLiteral("previousTab"), .text = tr("Pre&vious Tab"), .icon = QStringLiteral("chevron-left"),
         .shortcuts = {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), QKeySequence(Qt::CTRL | Qt::Key_PageUp)},
         .tip = tr("Show the previous open document"), .enabledWhen = several, .group = view},
        step(-1));
    for (int n = 1; n <= 9; ++n) {
        add({.id = QStringLiteral("tab%1").arg(n), .text = tr("Tab %1").arg(n),
             .shortcuts = {QKeySequence(Qt::ALT | static_cast<Qt::Key>(Qt::Key_0 + n))},
             .enabledWhen = [this, n] { return tabs_.size() >= n; }, .group = view},
            [this, n] {
                if (DocumentTab* d = tabs_.value(n - 1)) {
                    setCurrent(d);
                }
            });
    }

    // Edit.
    add({.id = QStringLiteral("undo"), .text = tr("&Undo"), .icon = QStringLiteral("undo-2"),
         .themeIcon = QStringLiteral("edit-undo"), .shortcuts = {QKeySequence::Undo},
         .enabledWhen = [this] { return current_ != nullptr && current_->canUndo(); }, .group = edit},
        doc([](DocumentTab* d) { d->onWorker([](RenderWorker* w) { w->undo(); }); }));
    add({.id = QStringLiteral("redo"), .text = tr("&Redo"), .icon = QStringLiteral("redo-2"),
         .themeIcon = QStringLiteral("edit-redo"), .shortcuts = {QKeySequence::Redo},
         .enabledWhen = [this] { return current_ != nullptr && current_->canRedo(); }, .group = edit},
        doc([](DocumentTab* d) { d->onWorker([](RenderWorker* w) { w->redo(); }); }));
    add({.id = QStringLiteral("copy"), .text = tr("&Copy"), .icon = QStringLiteral("copy"),
         .themeIcon = QStringLiteral("edit-copy"), .shortcuts = {QKeySequence::Copy},
         .tip = tr("Copy the selected text"), .enabledWhen = open, .group = edit},
        doc([](DocumentTab* d) { d->view()->copySelection(); }));
    add({.id = QStringLiteral("find"), .text = tr("&Find…"), .icon = QStringLiteral("search"),
         .themeIcon = QStringLiteral("edit-find"), .shortcuts = {QKeySequence::Find},
         .tip = tr("Find text in the document"), .enabledWhen = open, .group = edit},
        &MainWindow::showFindBar);
    add({.id = QStringLiteral("findNext"), .text = tr("Find &Next"), .icon = QStringLiteral("chevron-down"),
         .shortcuts = {QKeySequence::FindNext}, .enabledWhen = open, .group = edit},
        doc([](DocumentTab* d) { d->view()->nextMatch(); }));
    add({.id = QStringLiteral("findPrevious"), .text = tr("Find Pre&vious"), .icon = QStringLiteral("chevron-up"),
         .shortcuts = {QKeySequence::FindPrevious}, .enabledWhen = open, .group = edit},
        doc([](DocumentTab* d) { d->view()->prevMatch(); }));
    add({.id = QStringLiteral("preferences"), .text = tr("Pre&ferences…"), .icon = QStringLiteral("settings"),
         .themeIcon = QStringLiteral("preferences-system"),
         .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_Comma)}, .group = edit},
        [this] { openPreferences(); });

    // View.
    const auto zoom = [this, doc](auto fn) {
        return doc([this, fn](DocumentTab* d) {
            fn(d->view());
            updateZoomLabel();
        });
    };
    add({.id = QStringLiteral("zoomIn"), .text = tr("Zoom &In"), .icon = QStringLiteral("zoom-in"),
         .themeIcon = QStringLiteral("zoom-in"), .shortcuts = {QKeySequence::ZoomIn},
         .enabledWhen = open, .group = view},
        zoom([](PageView* v) { v->zoomBy(1.25); }));
    add({.id = QStringLiteral("zoomOut"), .text = tr("Zoom &Out"), .icon = QStringLiteral("zoom-out"),
         .themeIcon = QStringLiteral("zoom-out"), .shortcuts = {QKeySequence::ZoomOut},
         .enabledWhen = open, .group = view},
        zoom([](PageView* v) { v->zoomBy(0.8); }));
    add({.id = QStringLiteral("actualSize"), .text = tr("&Actual Size"), .icon = QStringLiteral("scan"),
         .themeIcon = QStringLiteral("zoom-original"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_1)},
         .enabledWhen = open, .group = view},
        zoom([](PageView* v) { v->setZoom(1.0); }));
    add({.id = QStringLiteral("fitWidth"), .text = tr("Fit &Width"), .icon = QStringLiteral("arrow-left-right"),
         .themeIcon = QStringLiteral("zoom-fit-width"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_0)},
         .enabledWhen = open, .group = view},
        zoom([](PageView* v) { v->fitWidth(); }));
    add({.id = QStringLiteral("fitPage"), .text = tr("Fit &Page"), .icon = QStringLiteral("maximize"),
         .themeIcon = QStringLiteral("zoom-fit-best"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_9)},
         .enabledWhen = open, .group = view},
        zoom([](PageView* v) { v->fitPage(); }));
    add({.id = QStringLiteral("rotateView"), .text = tr("&Rotate View"), .icon = QStringLiteral("rotate-cw"),
         .themeIcon = QStringLiteral("object-rotate-right"), .shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_R)},
         .tip = tr("Turn the pages on screen; the file is not changed"), .enabledWhen = open, .group = view},
        doc([](DocumentTab* d) { d->view()->rotateBy(90); }));
    QAction* sidebar = add({.id = QStringLiteral("toggleSidebar"), .text = tr("&Sidebar"),
                            .icon = QStringLiteral("panel-left"), .shortcuts = {QKeySequence(Qt::Key_F9)},
                            .tip = tr("Show or hide the sidebar"), .checkable = true,
                            .enabledWhen = open, .group = view},
                           [this](bool on) {
                               if (DocumentTab* d = current_) {
                                   d->sidebar()->setExpanded(on);
                               }
                           });
    sidebar->setChecked(true);
    add({.id = QStringLiteral("focusNextRegion"), .text = tr("Next Part of the Window"),
         .shortcuts = {QKeySequence(Qt::Key_F6)},
         .tip = tr("Move between the toolbar, the mode bar, the sidebar and the page"), .group = view},
        [this] { focusRegion(+1); });
    add({.id = QStringLiteral("focusPreviousRegion"), .text = tr("Previous Part of the Window"),
         .shortcuts = {QKeySequence(Qt::SHIFT | Qt::Key_F6)}, .group = view},
        [this] { focusRegion(-1); });
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
                                    .enabledWhen = [this, needs] { return docAllows(this, needs); },
                                    .group = comment});
        a->setProperty("certNeeds", needs);  // also for Select (0), which applyCertification skips
        tools_->addAction(a);
        const PageView::Tool tool = t.tool;
        const QString id = QLatin1String(t.id);
        connect(a, &QAction::triggered, this, [this, tool, id] {
            if (DocumentTab* d = current_) {
                (void)d->view()->setTool(tool);
                d->setTool(id);
            }
        });
    }
    tools_->actions().first()->setChecked(true);

    const auto add = [this](Spec spec, auto&& slot) {
        const int needs = spec.certNeeds;
        if (!spec.enabledWhen) {
            spec.enabledWhen = [this, needs] { return docAllows(this, needs); };
        }
        QAction* a = actions_->add(spec);
        connect(a, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return a;
    };
    const auto doc = [this](auto fn) {
        return [this, fn] {
            if (DocumentTab* d = current_) {
                fn(d);
            }
        };
    };
    const auto signedDoc = [this] {
        return current_ != nullptr && current_->isOpen() && current_->signatureCount() > 0;
    };
    const QString sign = tr("Sign");
    const QString tools = tr("Tools");
    add({.id = QStringLiteral("redactText"), .text = tr("Redact &Text…"), .icon = QStringLiteral("eye-off"),
         .tip = tr("Remove every occurrence of a word or phrase from the file"), .certNeeds = 4, .group = tools},
        doc([](DocumentTab* d) { d->redactText(); }));
    add({.id = QStringLiteral("applyRedactions"), .text = tr("&Apply Redactions…"), .icon = QStringLiteral("check"),
         .tip = tr("Remove everything under the marked areas from the file, for good"), .certNeeds = 4,
         .enabledWhen = [this] { return docAllows(this, 4) && current_->pendingRedactions() > 0; },
         .group = tools},
        doc([](DocumentTab* d) { (void)d->applyRedactions(); }));
    add({.id = QStringLiteral("clearRedactionMarks"), .text = tr("C&lear Redaction Marks"),
         .icon = QStringLiteral("x"), .tip = tr("Forget the marked areas; nothing is removed"), .certNeeds = 4,
         .enabledWhen = [this] { return current_ != nullptr && current_->pendingRedactions() > 0; },
         .group = tools},
        doc([](DocumentTab* d) { d->clearRedactionMarks(); }));
    add({.id = QStringLiteral("watermark"), .text = tr("&Watermark…"), .icon = QStringLiteral("droplets"),
         .tip = tr("Put text such as DRAFT across the pages"), .certNeeds = 4, .group = tools},
        doc([](DocumentTab* d) { d->addWatermark(); }));
    add({.id = QStringLiteral("recognizeText"), .text = tr("&Recognize Text (OCR)…"),
         .icon = QStringLiteral("scan-text"), .tip = tr("Make scanned pages searchable"), .certNeeds = 4,
         .group = tools},
        doc([](DocumentTab* d) { d->recognizeText(); }));
    add({.id = QStringLiteral("cropMargins"), .text = tr("Crop &Margins…"), .icon = QStringLiteral("crop"),
         .tip = tr("Trim the same margins from many pages"), .certNeeds = 4, .group = tools},
        doc([](DocumentTab* d) { d->cropMargins(); }));
    add({.id = QStringLiteral("signInvisibly"), .text = tr("Sign &Invisibly…"), .icon = QStringLiteral("file-pen-line"),
         .tip = tr("Sign the document without marking a page"), .certNeeds = 2, .group = sign},
        doc([](DocumentTab* d) { d->startSigning(0, QRectF()); }));
    add({.id = QStringLiteral("addLongTermValidation"), .text = tr("Add &Long-Term Validation…"),
         .icon = QStringLiteral("history"),
         .tip = tr("Embed what every signature needs to be checked after its certificates expire "
                   "(PAdES B-LT), and a document timestamp over it (B-LTA)"),
         .enabledWhen = signedDoc, .group = sign},
        doc([](DocumentTab* d) { d->addLongTermValidation(); }));
    add({.id = QStringLiteral("checkRevocation"), .text = tr("Check &Revocation Online"),
         .icon = QStringLiteral("globe"),
         .tip = tr("Ask the certificates' revocation services now whether they were revoked. Only "
                   "certificate identifiers are sent, never the document."),
         .enabledWhen = signedDoc, .group = sign},
        doc([](DocumentTab* d) { d->checkRevocation(); }));
    add({.id = QStringLiteral("flattenForm"), .text = tr("&Flatten Form…"), .icon = QStringLiteral("file-text"),
         .tip = tr("Make the filled-in values part of the page, so they can no longer be changed"),
         .certNeeds = 4,
         .enabledWhen = [this] { return docAllows(this, 4) && current_->formPanel()->count() > 0; },
         .group = sign},
        doc([](DocumentTab* d) { d->flattenForm(); }));
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

void MainWindow::updateTrustedList() {
    const auto answer = QMessageBox::question(
        this, tr("Update EU Trusted Lists"),
        tr("Leht will download the European Commission's List of Trusted Lists and the national "
           "lists it points to — about 30 servers, some 28 MB — and verify each by its "
           "signature. Nothing about you or your documents is sent.\n\nUpdate now?"));
    if (answer != QMessageBox::Yes) {
        return;
    }
    // Any worker can fetch them; with no document open, one of its own.
    DocumentTab* d = current_ != nullptr ? current_ : addDocument();
    statusBar()->showMessage(tr("Updating the EU trusted lists…"));
    d->onWorker([](RenderWorker* w) { w->updateTrustedList(); });
}

// --- File tools: Combine Files, Reduce File Size, Split Document ----------------

void MainWindow::buildFileTools() {
    fileTools_ = new FileTools();
    fileTools_->moveToThread(&fileToolsThread_);
    connect(&fileToolsThread_, &QThread::finished, fileTools_, &QObject::deleteLater);
    fileToolsThread_.start();

    // One job at a time: all three wait while one runs.
    const auto open = [this] { return current_ != nullptr && current_->isOpen(); };
    const QString file = tr("File");
    QAction* combine = actions_->add({.id = QStringLiteral("combineFiles"), .text = tr("Co&mbine Files…"),
                                      .icon = QStringLiteral("combine"),
                                      .tip = tr("Put PDFs and images together into one new PDF"),
                                      .enabledWhen = [this] { return !fileToolBusy_; }, .group = file});
    QAction* reduce = actions_->add({.id = QStringLiteral("reduceFileSize"), .text = tr("Re&duce File Size…"),
                                     .icon = QStringLiteral("minimize-2"),
                                     .tip = tr("Make a smaller copy of this document"),
                                     .enabledWhen = [this, open] { return open() && !fileToolBusy_; },
                                     .group = file});
    QAction* split = actions_->add({.id = QStringLiteral("splitDocument"), .text = tr("Sp&lit Document…"),
                                    .icon = QStringLiteral("scissors"),
                                    .tip = tr("Write this document's pages into separate files"),
                                    .enabledWhen = [this, open] { return open() && !fileToolBusy_; },
                                    .group = file});

    connect(combine, &QAction::triggered, this, [this] {
        combineFiles(current_ != nullptr && current_->isOpen() ? QStringList{current_->path()} : QStringList{});
    });

    // Password protection: a new file, by default beside this one. Like
    // Reduce and Split it reads the file on disk, so edits are settled first.
    const auto isPdf = [this] {
        return current_ != nullptr && current_->path().endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive);
    };
    QAction* protect = actions_->add({.id = QStringLiteral("protect"), .text = tr("Password &Protect…"),
                                      .icon = QStringLiteral("lock"),
                                      .tip = tr("Save a copy that asks for a password to open"),
                                      .enabledWhen = [this, open, isPdf] { return open() && isPdf() && !fileToolBusy_; },
                                      .group = file});
    QAction* unprotect = actions_->add({.id = QStringLiteral("unprotect"), .text = tr("Remove Pass&word…"),
                                        .icon = QStringLiteral("lock-open"),
                                        .tip = tr("Save a copy that opens without a password"),
                                        .enabledWhen = [this, open, isPdf] {
                                            return open() && isPdf() && current_->isEncrypted() && !fileToolBusy_;
                                        },
                                        .group = file});
    const auto target = [this](const QString& suffix) {
        const QFileInfo info(current_->path());
        return QFileDialog::getSaveFileName(
            this, tr("Save as"), info.dir().filePath(tr("%1 (%2).pdf").arg(info.completeBaseName(), suffix)),
            tr("PDF documents (*.pdf)"));
    };
    connect(protect, &QAction::triggered, this, [this, protect, target] {
        DocumentTab* d = current_;
        if (d == nullptr || !d->isOpen() || !d->resolveUnsaved([protect] { protect->trigger(); })) {
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
        const QString input = d->path();
        const QString user = dialog.openPassword();
        const QString owner = dialog.permissionsPassword();
        const int method = dialog.method();
        const int permissions = dialog.permissions();
        runFileTool(tr("Protecting…"), [=](FileTools* t, const QString& password) {
            t->protect(input, password, output, true, user, owner, method, permissions);
        });
    });
    connect(unprotect, &QAction::triggered, this, [this, unprotect, target] {
        DocumentTab* d = current_;
        if (d == nullptr || !d->isOpen() || !d->resolveUnsaved([unprotect] { unprotect->trigger(); })) {
            return;
        }
        const QString output = target(tr("no password"));
        if (output.isEmpty()) {
            return;
        }
        const QString input = d->path();
        // The job asks for the current password itself: Leht does not keep it.
        runFileTool(tr("Removing the password…"), [=](FileTools* t, const QString& password) {
            t->protect(input, password, output, false, {}, {}, 2, 0x7F);
        });
    });

    // Reduce and Split read the file on disk, so unsaved edits are settled
    // first; the dialogs then describe the file that will actually be used.
    connect(reduce, &QAction::triggered, this, [this, reduce] {
        DocumentTab* d = current_;
        if (d == nullptr || !d->isOpen() || !d->resolveUnsaved([reduce] { reduce->trigger(); })) {
            return;
        }
        ReduceDialog dialog(this, d->path(), d->signatureCount() > 0);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString input = d->path();
        const QString output = dialog.output();
        const int preset = dialog.preset();
        runFileTool(tr("Reducing file size…"),
                    [input, output, preset](FileTools* t, const QString& password) {
                        t->compress(input, password, output, preset, 0, false);
                    });
    });

    connect(split, &QAction::triggered, this, [this, split] {
        DocumentTab* d = current_;
        if (d == nullptr || !d->isOpen() || !d->resolveUnsaved([split] { split->trigger(); })) {
            return;
        }
        SplitDialog dialog(this, d->path(), d->pageCount(), d->signatureCount() > 0);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString input = d->path();
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
                    openDocument(written.first());
                }
            });
    connect(fileTools_, &FileTools::passwordRequired, this, [this](bool wrong) {
        if (fileToolsProgress_ != nullptr) {
            fileToolsProgress_->hide();
        }
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, tr("Password required"),
            wrong ? tr("Wrong password. Try again for “%1”:").arg(fileToolDocument_)
                  : tr("“%1” is password-protected. Enter its password:").arg(fileToolDocument_),
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
    fileToolDocument_ = current_ != nullptr ? current_->title() : QString();
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
