// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QColor>
#include <QList>
#include <QWidget>

class QMenu;
class QToolButton;

/// The colour the current comment tool draws in: one compact button showing
/// it, whose menu offers a few colours by name and any other.
class ColorSwatches : public QWidget {
    Q_OBJECT

public:
    explicit ColorSwatches(QWidget* parent = nullptr);

    /// Shows `color` as the current one.
    void setColor(const QColor& color);
    [[nodiscard]] QColor color() const { return color_; }

signals:
    void colorChosen(const QColor& color);

private:
    [[nodiscard]] static QIcon swatch(const QColor& color, bool wheel = false);

    QToolButton* button_ = nullptr;
    QMenu* menu_ = nullptr;
    QList<QPair<QColor, QString>> colors_;
    QColor color_;
};
