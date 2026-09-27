// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QAction;
class QMenu;
class QMenuBar;
class QToolBar;
class QWidget;

/// Every command the window offers, by id. The menu bar, toolbars, context
/// menus and the shortcut sheet are all built from here, so a command has one
/// text, icon, shortcut and tooltip wherever it appears.
///
/// An id is the action's objectName, which the smoke test finds it by. Actions
/// made elsewhere (a panel's own menu, say) can be listed in a menu by their
/// objectName too: `find` falls back to the window's children.
class ActionRegistry : public QObject {
    Q_OBJECT

public:
    struct Spec {
        QString id;
        QString text;  ///< as in a menu; an ellipsis when it asks for more first
        QString icon;  ///< Lucide name (ui/icons/lucide), may be empty
        QString themeIcon;  ///< freedesktop name, used if the user prefers the theme
        QList<QKeySequence> shortcuts;
        QString tip;
        /// The certification level from which it is allowed (see
        /// MainWindow::applyCertification): 0 always, 2 signing, 3 annotating,
        /// 4 never in a certified document.
        int certNeeds = 0;
        bool checkable = false;
        /// Disabled until this says otherwise; re-asked by refresh(). Empty
        /// means the registry leaves enabling to whoever owns the action.
        std::function<bool()> enabledWhen;
        QString group;  ///< heading on the shortcut sheet
    };

    /// One entry in a menu layout: an action id, "-" for a separator, or a
    /// submenu when `children` is not empty (then `id` is its title).
    struct Item {
        QString id;
        std::vector<Item> children;
        Item(const char* i) : id(QString::fromUtf8(i)) {}  // NOLINT: terse tables
        Item(QString i) : id(std::move(i)) {}  // NOLINT
        Item(QString title, std::vector<Item> items) : id(std::move(title)), children(std::move(items)) {}
    };
    struct Menu {
        QString title;
        std::vector<Item> items;
    };

    /// `window` parents the actions, so their shortcuts work while any part of
    /// it has focus.
    explicit ActionRegistry(QWidget* window);

    QAction* add(const Spec& spec);
    /// Gives an action made elsewhere the registry's treatment: icon, tip,
    /// shortcuts and certification level from `spec` (its id must match the
    /// objectName), without changing what it does.
    QAction* adopt(QAction* action, const Spec& spec);

    /// The action with this id: one of ours, else any QAction in the window
    /// with that objectName. nullptr if there is none.
    [[nodiscard]] QAction* find(const QString& id) const;

    /// Builds the menus into `bar`. Ids with no action are left out, and the
    /// separators they would leave doubled or dangling go with them, so a menu
    /// can name commands that only some builds have (OCR, say).
    void populate(QMenuBar* bar, const std::vector<Menu>& layout) const;
    void populate(QMenu* menu, const std::vector<Item>& items) const;
    void populate(QToolBar* bar, const QStringList& ids) const;

    /// Re-asks every enabledWhen.
    void refresh();
    /// Loads every icon again (after the icon-theme preference changed).
    void reloadIcons();

    /// Every registered action with a shortcut, grouped, for the shortcut sheet.
    struct ShortcutRow {
        QString group;
        QString text;
        QString keys;
    };
    [[nodiscard]] std::vector<ShortcutRow> shortcutSheet() const;

private:
    void apply(QAction* action, const Spec& spec);

    QWidget* window_;
    QHash<QString, QAction*> actions_;
    std::vector<std::pair<QAction*, Spec>> specs_;  // in registration order
};

/// Help > Keyboard Shortcuts: every shortcut in `registry`, by group.
void showShortcutSheet(QWidget* parent, const ActionRegistry& registry);

/// Removes the "…" and any "&" mnemonic, for a tooltip or a sheet.
[[nodiscard]] QString plainActionText(QString text);
