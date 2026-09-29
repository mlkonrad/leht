// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_toolbar.hpp"

#include "actions.hpp"
#include "icons.hpp"
#include "welcome_view.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPalette>
#include <QSpinBox>
#include <QToolButton>

namespace {

constexpr int kQuickIconSize = 22;

QStringList ids(std::initializer_list<const char*> list) {
    QStringList out;
    for (const char* id : list) {
        out << QLatin1String(id);
    }
    return out;
}

/// `lucide` in the highlighted-text colour, for the one filled button, at the
/// screen scales likely to be asked for.
QIcon onHighlight(const QString& lucide) {
    const QColor color = QApplication::palette().color(QPalette::HighlightedText);
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        icon.addPixmap(icons::tinted(lucide, color, kQuickIconSize, scale));
    }
    return icon;
}

}  // namespace

std::vector<MainToolbar::Group> MainToolbar::standardGroups() {
    return {
        {.id = QStringLiteral("edit"), .text = tr("Edit"), .icon = QStringLiteral("file-pen-line"),
         .tip = tr("Add text, highlight, notes and drawings"), .defaultId = QStringLiteral("toolText"),
         .menuIds = ids({"toolText", "toolHighlight", "toolUnderline", "toolStrike", "toolNote", "toolDraw",
                         "toolStamp", "-", "toolMove", "toolErase"}),
         .task = QStringLiteral("edit")},
        {.id = QStringLiteral("sign"), .text = tr("Sign"), .icon = QStringLiteral("signature"),
         .tip = tr("Sign the document, or fill in its form"), .defaultId = QStringLiteral("toolSign"),
         .menuIds = ids({"toolSign", "signInvisibly", "fillForm", "-", "addLongTermValidation", "flattenForm"}),
         .task = QLatin1String(WelcomeView::kSign), .primary = true},
        {.id = QStringLiteral("files"), .text = tr("Files"), .icon = QStringLiteral("files"),
         .tip = tr("Combine, split, shrink, protect and export files"),
         .defaultId = QStringLiteral("combineFiles"),
         .menuIds = ids({"combineFiles", "splitDocument", "reduceFileSize", "-", "protect", "unprotect", "-",
                         "exportImages", "exportText"})},
        {.id = QStringLiteral("pages"), .text = tr("Pages"), .icon = QStringLiteral("layout-grid"),
         .tip = tr("Turn, reorder, insert and delete pages; crop, watermark, recognize text"),
         .defaultId = QStringLiteral("pageGrid"),
         .menuIds = ids({"pageGrid", "-", "pageRotateLeft", "pageRotateRight", "pageInsertFile", "pageInsertBlank",
                         "pageExtract", "pageDelete", "-", "toolCrop", "watermark", "-", "recognizeText",
                         "toolRedact", "redactText"}),
         .task = QStringLiteral("pages")},
    };
}

MainToolbar::MainToolbar(const ActionRegistry* actions, const std::vector<Group>& groups, QWidget* parent)
    : QToolBar(tr("Main"), parent), actions_(actions) {
    setObjectName(QStringLiteral("mainBar"));
    setMovable(false);
    setFloatable(false);
    setIconSize(QSize(20, 20));
    setToolButtonStyle(Qt::ToolButtonIconOnly);  // the quick buttons set their own
    setAttribute(Qt::WA_StyledBackground);

    // Left: the file and undo commands, icons only.
    actions_->populate(this, ids({"toggleSidebar", "-", "open", "save", "-", "undo", "redo"}));

    // Then the quick tools: after a separator, or centred between two springs.
    const auto spring = [this] {
        auto* spacer = new QWidget(this);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        return addWidget(spacer);
    };
    leadSeparator_ = addSeparator();
    leadSpring_ = spring();
    leadSpring_->setVisible(false);
    quick_.reserve(groups.size());
    for (const Group& group : groups) {
        addQuick(group);
    }
    spring();

    // Right: where you are and how big.
    pageSpin_ = new QSpinBox(this);
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
    addWidget(pageSpin_);
    pageLabel_ = new QLabel(this);
    pageLabel_->setObjectName(QStringLiteral("pageCount"));
    pageLabel_->setContentsMargins(4, 0, 8, 0);
    addWidget(pageLabel_);
    addSeparator();
    actions_->populate(this, ids({"zoomOut"}));
    zoomLabel_ = new QLabel(this);
    zoomLabel_->setObjectName(QStringLiteral("zoomLevel"));
    zoomLabel_->setAlignment(Qt::AlignCenter);
    zoomLabel_->setMinimumWidth(48);
    zoomLabel_->setToolTip(tr("Zoom"));
    addWidget(zoomLabel_);
    actions_->populate(this, ids({"zoomIn", "fitWidth", "-", "find"}));

    applyStyleSheet();
    setLabelsShown(true);
}

void MainToolbar::addQuick(const Group& group) {
    Quick quick{.group = group};
    auto* button = new QToolButton(this);
    button->setObjectName(QStringLiteral("quick_") + group.id);
    button->setText(group.text);
    button->setIconSize(QSize(kQuickIconSize, kQuickIconSize));
    button->setIcon(group.primary ? onHighlight(group.icon) : icons::named(group.icon));
    button->setProperty("primary", group.primary);
    button->setAutoRaise(true);
    button->setAccessibleName(group.text);
    button->setAccessibleDescription(group.tip);

    quick.defaultAction = actions_->find(group.defaultId);
    QString tip = group.tip;
    if (quick.defaultAction != nullptr && !quick.defaultAction->shortcut().isEmpty()) {
        tip += QStringLiteral(" (%1)").arg(quick.defaultAction->shortcut().toString(QKeySequence::NativeText));
    }
    button->setToolTip(tip);

    // The rest of the group, the registry's own actions: their text, icon,
    // shortcut and enabled state are the menu bar's.
    auto* menu = new QMenu(button);
    menu->setObjectName(QStringLiteral("quick_%1_menu").arg(group.id));
    menu->setAccessibleName(tr("More %1 tools").arg(group.text));
    std::vector<ActionRegistry::Item> items;
    for (const QString& id : group.menuIds) {
        items.emplace_back(id);
    }
    actions_->populate(menu, items);
    for (QAction* a : menu->actions()) {
        if (!a->isSeparator()) {
            quick.menuActions << a;
        }
    }
    if (!quick.menuActions.isEmpty()) {
        button->setMenu(menu);
        button->setPopupMode(QToolButton::MenuButtonPopup);
    }

    quick.button = button;
    const std::size_t index = quick_.size();
    quick_.push_back(std::move(quick));
    connect(button, &QToolButton::clicked, this, [this, index] { runQuick(quick_[index]); });
    // Enabled follows the actions as they change; nothing polls.
    const auto update = [this, index] { updateQuick(quick_[index]); };
    for (QAction* a : quick_[index].menuActions) {
        connect(a, &QAction::changed, this, update);
    }
    if (QAction* d = quick_[index].defaultAction; d != nullptr && !quick_[index].menuActions.contains(d)) {
        connect(d, &QAction::changed, this, update);
    }
    addWidget(button);
    updateQuick(quick_[index]);
}

void MainToolbar::updateQuick(Quick& quick) {
    const bool asks = !documentOpen_ && !quick.group.task.isEmpty();
    bool any = asks || (quick.defaultAction != nullptr && quick.defaultAction->isEnabled());
    for (const QAction* a : quick.menuActions) {
        any = any || a->isEnabled();
    }
    quick.button->setEnabled(any);
}

void MainToolbar::runQuick(const Quick& quick) {
    if (!documentOpen_ && !quick.group.task.isEmpty()) {
        emit taskRequested(quick.group.task);
        return;
    }
    QAction* d = quick.defaultAction;
    if (d != nullptr && d->isEnabled()) {
        // A tool already in hand stays in hand: a click is "this", not a toggle.
        if (!d->isCheckable() || !d->isChecked()) {
            d->trigger();
        }
    } else if (quick.button->menu() != nullptr) {
        quick.button->showMenu();  // the default is not allowed here; offer the rest
    }
}

QToolButton* MainToolbar::quickButton(const QString& id) const {
    for (const Quick& q : quick_) {
        if (q.group.id == id) {
            return q.button;
        }
    }
    return nullptr;
}

void MainToolbar::setDocumentOpen(bool open) {
    documentOpen_ = open;
    for (Quick& q : quick_) {
        updateQuick(q);
    }
}

void MainToolbar::setLabelsShown(bool shown) {
    labels_ = shown;
    compact_ = false;
    setQuickStyle(shown ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    fitToWidth();
}

void MainToolbar::setQuickAlignment(Qt::Alignment alignment) {
    centred_ = (alignment & Qt::AlignHCenter) != 0;
    leadSeparator_->setVisible(!centred_);
    leadSpring_->setVisible(centred_);
    fitToWidth();
}

void MainToolbar::setQuickStyle(Qt::ToolButtonStyle style) {
    for (const Quick& q : quick_) {
        q.button->setToolButtonStyle(style);
    }
}

int MainToolbar::quickWidth() const {
    int width = 0;
    for (const Quick& q : quick_) {
        width += q.button->sizeHint().width();
    }
    return width;
}

void MainToolbar::fitToWidth() {
    // Labels go before anything is pushed into the » menu, and come back when
    // there is room for them again.
    if (!labels_ || layout() == nullptr || width() <= 0) {
        return;
    }
    const int needed = layout()->sizeHint().width();
    if (!compact_ && needed > width()) {
        const int before = quickWidth();
        setQuickStyle(Qt::ToolButtonIconOnly);
        labelWidth_ = before - quickWidth();
        compact_ = true;
    } else if (compact_ && needed + labelWidth_ <= width()) {
        setQuickStyle(Qt::ToolButtonTextBesideIcon);
        compact_ = false;
    }
}

void MainToolbar::resizeEvent(QResizeEvent* event) {
    QToolBar::resizeEvent(event);
    fitToWidth();
}

void MainToolbar::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    connect(menu.addAction(tr("Hide Toolbar")), &QAction::triggered, this, &MainToolbar::hideRequested);
    QAction* labels = menu.addAction(tr("Show Labels"));
    labels->setCheckable(true);
    labels->setChecked(labels_);
    connect(labels, &QAction::toggled, this, &MainToolbar::labelsToggled);
    menu.addSeparator();
    auto* where = new QActionGroup(&menu);
    QAction* left = menu.addAction(tr("Tools on the Left"));
    QAction* centre = menu.addAction(tr("Tools in the Centre"));
    for (QAction* a : {left, centre}) {
        a->setCheckable(true);
        where->addAction(a);
    }
    (centred_ ? centre : left)->setChecked(true);
    connect(left, &QAction::triggered, this, [this] { emit quickAlignmentRequested(Qt::AlignLeft); });
    connect(centre, &QAction::triggered, this, [this] { emit quickAlignmentRequested(Qt::AlignHCenter); });
    menu.exec(event->globalPos());
    event->accept();
}

void MainToolbar::changeEvent(QEvent* event) {
    QToolBar::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange) {
        // palette() in a style sheet is read once; read it again, and the
        // filled button's icon with it.
        applyStyleSheet();
        for (const Quick& q : quick_) {
            if (q.group.primary) {
                q.button->setIcon(onHighlight(q.group.icon));
            }
        }
    }
}

void MainToolbar::applyStyleSheet() {
    // Rounded, borderless buttons that light up under the pointer, a tint of
    // the highlight for what is on, and one filled button (Sign). Palette
    // colours only, so light, dark and high contrast all hold.
    QColor tint = palette().color(QPalette::Highlight);
    tint.setAlpha(64);
    const QString on = QStringLiteral("rgba(%1, %2, %3, %4)")
                           .arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alpha());
    setStyleSheet(QStringLiteral(
        "QToolBar#mainBar { border: none; border-bottom: 1px solid palette(mid); padding: 3px 6px; spacing: 2px; }"
        "QToolBar#mainBar::separator { background: palette(mid); width: 1px; margin: 8px 6px; }"
        "QToolBar#mainBar QToolButton { border: 1px solid transparent; border-radius: 8px; padding: 4px; }"
        "QToolBar#mainBar QToolButton:hover { background: palette(alternate-base); border-color: palette(mid); }"
        "QToolBar#mainBar QToolButton:pressed, QToolBar#mainBar QToolButton:checked {"
        "  background: %1; border-color: palette(highlight); }"
        "QToolBar#mainBar QToolButton:disabled { color: palette(mid); }"
        "QToolBar#mainBar QToolButton[popupMode=\"1\"] { padding: 5px 16px 5px 8px; margin: 0 2px;"
        "  font-weight: 600; }"
        "QToolBar#mainBar QToolButton::menu-button { border: none; border-left: 1px solid transparent;"
        "  background: transparent; border-top-right-radius: 8px; border-bottom-right-radius: 8px; width: 14px; }"
        "QToolBar#mainBar QToolButton::menu-button:hover { border-left-color: palette(mid); }"
        "QToolBar#mainBar QToolButton[primary=\"true\"] { background: palette(highlight);"
        "  color: palette(highlighted-text); border-color: palette(highlight); }"
        "QToolBar#mainBar QToolButton[primary=\"true\"]:hover { border-color: palette(highlighted-text); }"
        "QToolBar#mainBar QToolButton[primary=\"true\"]:disabled { background: transparent;"
        "  color: palette(mid); border-color: palette(mid); }"
        // Sign reads as one pill: its ▾ segment drawn by the sheet, not the style,
        // in the same fill, split off by a thin divider.
        "QToolBar#mainBar QToolButton#quick_sign::menu-button { background: palette(highlight);"
        "  border: 1px solid palette(highlight); border-left: 1px solid palette(highlighted-text);"
        "  border-top-left-radius: 0; border-bottom-left-radius: 0;"
        "  border-top-right-radius: 8px; border-bottom-right-radius: 8px; }"
        "QToolBar#mainBar QToolButton#quick_sign::menu-button:disabled { background: transparent;"
        "  border-color: palette(mid); }")
                      .arg(on));
}
