// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QWidget>

class QAction;
class QTabBar;
class QToolBar;

/// The strip above the page: what you are doing (Read, Comment, Fill & Sign,
/// Redact…) on the left, and only that mode's tools beside it. Replaces one
/// long row of every tool at once.
class ModeBar : public QWidget {
    Q_OBJECT

public:
    explicit ModeBar(QWidget* parent = nullptr);

    /// Adds a mode with its tools, in order ("-" style separators are
    /// QAction separators). `id` is kept as the tab's data.
    void addMode(const QString& id, const QString& icon, const QString& title, const QString& tip,
                 const QList<QAction*>& tools);
    void setMode(const QString& id);
    [[nodiscard]] QString mode() const;
    /// A mode that is not allowed (a certification forbids everything in it)
    /// stays visible but disabled, with `why` as its tooltip.
    void setModeEnabled(const QString& id, bool enabled, const QString& why = {});
    void setToolButtonStyle(Qt::ToolButtonStyle style);

signals:
    void modeChanged(const QString& id);

private:
    void showTools(int index);

    QTabBar* tabs_ = nullptr;
    QToolBar* tools_ = nullptr;
    QList<QList<QAction*>> modeTools_;
    QStringList tips_;
};
