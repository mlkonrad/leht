// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>
#include <QToolBar>

#include <vector>

class ActionRegistry;
class QLabel;
class QSpinBox;
class QToolButton;

/// The bar at the top of the window, in three zones: the file and undo
/// commands (icons only) on the left, the four quick tools (Edit, Sign, Files,
/// Pages, each a big labelled button with a menu of the rest of its group)
/// right after them or centred (setQuickAlignment), and the page number and
/// zoom on the right.
///
/// Every button is built from ActionRegistry ids, so its text, icon, tip,
/// shortcut and certification lock come from one place. The page spin box and
/// the page and zoom labels are made here but belong to the window: it wires
/// and fills them (see pageSpin()).
class MainToolbar : public QToolBar {
    Q_OBJECT

public:
    /// One quick button. A click runs `defaultId`; the ▾ lists `menuIds`
    /// ("-" is a separator). `task` is what the welcome view calls the same
    /// job: with no document open, a click asks for it (taskRequested) instead.
    struct Group {
        QString id;  ///< the button is "quick_<id>"
        QString text;
        QString icon;
        QString tip;
        QString defaultId;
        QStringList menuIds;
        QString task;
        bool primary = false;  ///< filled with the highlight colour
    };

    /// The four groups, as the plan has them.
    [[nodiscard]] static std::vector<Group> standardGroups();

    MainToolbar(const ActionRegistry* actions, const std::vector<Group>& groups, QWidget* parent = nullptr);

    /// Made here, owned by the bar, wired and filled by the window.
    [[nodiscard]] QSpinBox* pageSpin() const { return pageSpin_; }
    [[nodiscard]] QLabel* pageLabel() const { return pageLabel_; }
    [[nodiscard]] QLabel* zoomLabel() const { return zoomLabel_; }
    [[nodiscard]] QToolButton* quickButton(const QString& id) const;

    /// Icons and labels, or icons only. Labels also go (and come back) by
    /// themselves when the window is too narrow for them.
    void setLabelsShown(bool shown);
    [[nodiscard]] bool labelsShown() const { return labels_; }
    /// Whether the labels are showing right now (false while squeezed).
    [[nodiscard]] bool labelsVisible() const { return labels_ && !compact_; }

    /// Where the quick tools sit: Qt::AlignLeft, after undo and redo (the
    /// default), or Qt::AlignHCenter, in the middle of the bar.
    void setQuickAlignment(Qt::Alignment alignment);
    [[nodiscard]] Qt::Alignment quickAlignment() const {
        return centred_ ? Qt::AlignHCenter : Qt::AlignLeft;
    }

    /// With no document, the buttons with a task ask for a file first.
    void setDocumentOpen(bool open);

signals:
    /// A quick button was clicked with no document open: `task` (a
    /// WelcomeView task id, or "edit" / "pages") after a file is chosen.
    void taskRequested(const QString& task);
    /// From the bar's context menu.
    void hideRequested();
    void labelsToggled(bool shown);
    void quickAlignmentRequested(Qt::Alignment alignment);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Quick {
        Group group;
        QToolButton* button = nullptr;
        QAction* defaultAction = nullptr;
        QList<QAction*> menuActions;
    };

    void addQuick(const Group& group);
    void updateQuick(Quick& quick);
    void runQuick(const Quick& quick);
    void setQuickStyle(Qt::ToolButtonStyle style);
    [[nodiscard]] int quickWidth() const;
    void fitToWidth();
    void applyStyleSheet();

    const ActionRegistry* actions_;
    std::vector<Quick> quick_;
    QSpinBox* pageSpin_ = nullptr;
    QLabel* pageLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    bool labels_ = true;
    bool compact_ = false;
    int labelWidth_ = 0;  // what the labels add, measured when they were dropped
    bool documentOpen_ = false;
    // Left: a separator after redo. Centred: a spring there instead, which
    // balances the one after the quick tools. Hidden, neither takes room.
    QAction* leadSeparator_ = nullptr;
    QAction* leadSpring_ = nullptr;
    bool centred_ = false;
};
