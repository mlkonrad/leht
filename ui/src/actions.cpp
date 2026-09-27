// SPDX-License-Identifier: AGPL-3.0-or-later
#include "actions.hpp"

#include "icons.hpp"

#include <algorithm>

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QWidget>

QString plainActionText(QString text) {
    text.remove(QChar(0x2026));  // …
    if (text.endsWith(QLatin1String("..."))) {
        text.chop(3);
    }
    // "&&" is a literal ampersand; a lone "&" marks the mnemonic.
    text.replace(QLatin1String("&&"), QStringLiteral("\x01"));
    text.remove(QLatin1Char('&'));
    text.replace(QStringLiteral("\x01"), QStringLiteral("&"));
    return text;
}

ActionRegistry::ActionRegistry(QWidget* window) : QObject(window), window_(window) {}

QAction* ActionRegistry::add(const Spec& spec) {
    auto* action = new QAction(window_);
    action->setObjectName(spec.id);
    action->setText(spec.text);
    action->setCheckable(spec.checkable);
    // Parented to the window and added to it, a shortcut works whether or not
    // the action is in a visible menu or toolbar.
    window_->addAction(action);
    apply(action, spec);
    return action;
}

QAction* ActionRegistry::adopt(QAction* action, const Spec& spec) {
    if (action == nullptr) {
        return nullptr;
    }
    if (!spec.text.isEmpty()) {
        action->setText(spec.text);
    }
    apply(action, spec);
    if (!action->shortcuts().isEmpty() && !window_->actions().contains(action)) {
        window_->addAction(action);  // or the shortcut waits for its menu to be open
    }
    return action;
}

void ActionRegistry::apply(QAction* action, const Spec& spec) {
    if (!spec.icon.isEmpty() || !spec.themeIcon.isEmpty()) {
        action->setIcon(icons::named(spec.icon, spec.themeIcon));
    }
    if (!spec.shortcuts.isEmpty()) {
        action->setShortcuts(spec.shortcuts);
    }
    // A tooltip says what it does and how to get there faster.
    QString tip = spec.tip.isEmpty() ? plainActionText(action->text()) : spec.tip;
    if (!action->shortcut().isEmpty()) {
        tip += QStringLiteral(" (%1)").arg(action->shortcut().toString(QKeySequence::NativeText));
    }
    action->setToolTip(tip);
    action->setProperty("baseTip", tip);
    action->setStatusTip(spec.tip);
    if (spec.certNeeds != 0) {
        action->setProperty("certMenu", true);
        action->setProperty("certNeeds", spec.certNeeds);
    }
    actions_.insert(spec.id, action);
    specs_.emplace_back(action, spec);
    if (spec.enabledWhen) {
        action->setEnabled(spec.enabledWhen());
    }
}

QAction* ActionRegistry::find(const QString& id) const {
    if (QAction* a = actions_.value(id)) {
        return a;
    }
    return window_->findChild<QAction*>(id);
}

namespace {

/// Drops separators that would lead, trail or double up once missing actions
/// are left out.
void tidySeparators(QWidget* w) {
    QAction* previous = nullptr;
    for (QAction* a : w->actions()) {
        if (a->isSeparator() && (previous == nullptr || previous->isSeparator())) {
            w->removeAction(a);
            continue;
        }
        previous = a;
    }
    if (previous != nullptr && previous->isSeparator()) {
        w->removeAction(previous);
    }
}

}  // namespace

void ActionRegistry::populate(QMenu* menu, const std::vector<Item>& items) const {
    for (const Item& item : items) {
        if (!item.children.empty()) {
            QMenu* sub = menu->addMenu(item.id);
            populate(sub, item.children);
            if (sub->actions().isEmpty()) {
                menu->removeAction(sub->menuAction());
            }
        } else if (item.id == QLatin1String("-")) {
            menu->addSeparator();
        } else if (QAction* a = find(item.id)) {
            menu->addAction(a);
        }
    }
    tidySeparators(menu);
}

void ActionRegistry::populate(QMenuBar* bar, const std::vector<Menu>& layout) const {
    for (const Menu& m : layout) {
        QMenu* menu = bar->addMenu(m.title);
        populate(menu, m.items);
        if (menu->actions().isEmpty()) {
            bar->removeAction(menu->menuAction());
        }
    }
}

void ActionRegistry::populate(QToolBar* bar, const QStringList& ids) const {
    for (const QString& id : ids) {
        if (id == QLatin1String("-")) {
            bar->addSeparator();
        } else if (QAction* a = find(id)) {
            bar->addAction(a);
        }
    }
    tidySeparators(bar);
}

void ActionRegistry::refresh() {
    for (const auto& [action, spec] : specs_) {
        if (spec.enabledWhen) {
            action->setEnabled(spec.enabledWhen());
        }
    }
}

void ActionRegistry::reloadIcons() {
    for (const auto& [action, spec] : specs_) {
        if (!spec.icon.isEmpty() || !spec.themeIcon.isEmpty()) {
            action->setIcon(icons::named(spec.icon, spec.themeIcon));
        }
    }
}

std::vector<ActionRegistry::ShortcutRow> ActionRegistry::shortcutSheet() const {
    std::vector<ShortcutRow> rows;
    for (const auto& [action, spec] : specs_) {
        if (action->shortcuts().isEmpty()) {
            continue;
        }
        QStringList keys;
        for (const QKeySequence& k : action->shortcuts()) {
            keys << k.toString(QKeySequence::NativeText);
        }
        rows.push_back({spec.group, plainActionText(action->text()), keys.join(QStringLiteral(", "))});
    }
    // Groups in the order they were first registered, which follows the menus.
    QStringList order;
    for (const ShortcutRow& r : rows) {
        if (!order.contains(r.group)) {
            order << r.group;
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [&order](const ShortcutRow& a, const ShortcutRow& b) {
        return order.indexOf(a.group) < order.indexOf(b.group);
    });
    return rows;
}

void showShortcutSheet(QWidget* parent, const ActionRegistry& registry) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("shortcutSheet"));
    dialog.setWindowTitle(QObject::tr("Keyboard Shortcuts"));
    dialog.resize(460, 560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* tree = new QTreeWidget(&dialog);
    tree->setColumnCount(2);
    tree->setHeaderLabels({QObject::tr("Command"), QObject::tr("Keys")});
    tree->setRootIsDecorated(false);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    QString group;
    for (const ActionRegistry::ShortcutRow& row : registry.shortcutSheet()) {
        if (row.group != group || tree->topLevelItemCount() == 0) {
            group = row.group;
            auto* heading = new QTreeWidgetItem(tree, {group});
            QFont bold = heading->font(0);
            bold.setWeight(QFont::DemiBold);
            heading->setFont(0, bold);
            heading->setFirstColumnSpanned(true);
        }
        new QTreeWidgetItem(tree, {QStringLiteral("    ") + row.text, row.keys});
    }
    layout->addWidget(tree);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}
