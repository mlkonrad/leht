// SPDX-License-Identifier: AGPL-3.0-or-later
#include "color_swatches.hpp"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QToolButton>

ColorSwatches::ColorSwatches(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("colorSwatches"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    button_ = new QToolButton(this);
    button_->setObjectName(QStringLiteral("colorButton"));
    button_->setAutoRaise(true);
    button_->setPopupMode(QToolButton::InstantPopup);
    button_->setIconSize(QSize(20, 20));
    button_->setToolTip(tr("Colour"));
    button_->setAccessibleName(tr("Colour"));
    layout->addWidget(button_);

    menu_ = new QMenu(button_);
    colors_ = {
        {QColor(255, 220, 0), tr("Yellow")}, {QColor(120, 220, 120), tr("Green")},
        {QColor(255, 150, 200), tr("Pink")}, {QColor(20, 90, 200), tr("Blue")},
        {QColor(200, 30, 30), tr("Red")},    {QColor(0, 0, 0), tr("Black")},
    };
    for (const auto& [colour, name] : colors_) {
        QAction* a = menu_->addAction(swatch(colour), name);
        a->setCheckable(true);
        a->setData(colour);
        const QColor c = colour;
        connect(a, &QAction::triggered, this, [this, c] {
            setColor(c);
            emit colorChosen(c);
        });
    }
    menu_->addSeparator();
    menu_->addAction(swatch(Qt::white, true), tr("Other Colour…"), this, [this] {
        const QColor picked = QColorDialog::getColor(color_, this, tr("Colour"));
        if (picked.isValid()) {
            setColor(picked);
            emit colorChosen(picked);
        }
    });
    button_->setMenu(menu_);
    setColor(colors_.first().first);
}

QIcon ColorSwatches::swatch(const QColor& color, bool wheel) {
    QPixmap pixmap(40, 40);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF circle(2, 2, 16, 16);
    if (wheel) {
        const QColor quarters[] = {QColor(230, 60, 60), QColor(240, 200, 40), QColor(60, 180, 90),
                                   QColor(50, 110, 220)};
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 4; ++i) {
            p.setBrush(quarters[i]);
            p.drawPie(circle, i * 90 * 16, 90 * 16);
        }
    } else {
        p.setBrush(color);
        p.setPen(QPen(QColor(0, 0, 0, 90), 1));
        p.drawEllipse(circle);
    }
    return QIcon(pixmap);
}

void ColorSwatches::setColor(const QColor& color) {
    color_ = color;
    button_->setIcon(swatch(color));
    QString name = color.name();
    for (QAction* a : menu_->actions()) {
        const bool here = a->data().value<QColor>() == color && a->isCheckable();
        a->setChecked(here);
        name = here ? a->text() : name;
    }
    button_->setToolTip(tr("Colour: %1").arg(name));
    button_->setAccessibleDescription(name);
}
