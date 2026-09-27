// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QHash>
#include <QWidget>

class QButtonGroup;
class QLabel;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

/// The left sidebar: a rail of icon tabs and the panel of the chosen one
/// (pages, outline, comments, signatures, form). Clicking the open tab again
/// folds the panel away, leaving the rail. A panel whose content the document
/// lacks (no outline, no form) has its tab hidden.
class Sidebar : public QWidget {
    Q_OBJECT

public:
    explicit Sidebar(QWidget* parent = nullptr);

    /// Adds a tab. `id` names it for showPanel() and is the tab's objectName
    /// with "Tab" appended.
    void addPanel(const QString& id, const QString& icon, const QString& title, QWidget* panel);
    /// Shows or hides a tab; hiding the open one opens the first available.
    void setPanelAvailable(const QString& id, bool available);
    [[nodiscard]] bool isPanelAvailable(const QString& id) const;
    /// Opens the panel (and the sidebar, if folded).
    void showPanel(const QString& id);
    /// The open panel's id, or empty when folded.
    [[nodiscard]] QString currentPanel() const;
    void setExpanded(bool expanded);
    [[nodiscard]] bool isExpanded() const;

signals:
    void panelChanged(const QString& id);  ///< empty when folded
    void expandedChanged(bool expanded);

private:
    struct Tab {
        QToolButton* button = nullptr;
        QWidget* page = nullptr;
    };
    void select(const QString& id);

    QVBoxLayout* rail_ = nullptr;
    QWidget* body_ = nullptr;
    QLabel* title_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QButtonGroup* group_ = nullptr;
    QHash<QString, Tab> tabs_;
    QStringList order_;
    QString current_;
};
