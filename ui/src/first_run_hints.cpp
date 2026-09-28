// SPDX-License-Identifier: AGPL-3.0-or-later
#include "first_run_hints.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

FirstRunHints* FirstRunHints::showOnce(QWidget* window, QVector<Hint> hints, const QString& key, bool force) {
    QSettings settings;
    if (!force && settings.value(key, false).toBool()) {
        return nullptr;
    }
    hints.removeIf([](const Hint& h) { return h.target.isNull(); });
    if (hints.isEmpty()) {
        return nullptr;
    }
    settings.setValue(key, true);  // once, even if closed half-way
    auto* tour = new FirstRunHints(window, std::move(hints));
    tour->show();
    tour->raise();
    tour->showHint(0);
    return tour;
}

FirstRunHints::FirstRunHints(QWidget* window, QVector<Hint> hints) : QWidget(window), hints_(std::move(hints)) {
    setObjectName(QStringLiteral("firstRunHints"));
    setAttribute(Qt::WA_DeleteOnClose);
    setGeometry(window->rect());
    window->installEventFilter(this);  // follow the window's size

    bubble_ = new QWidget(this);
    bubble_->setObjectName(QStringLiteral("hintBubble"));
    bubble_->setAttribute(Qt::WA_StyledBackground);
    bubble_->setStyleSheet(QStringLiteral(
        "QWidget#hintBubble { background: palette(base); border: 2px solid palette(highlight); border-radius: 10px; }"));
    bubble_->setMaximumWidth(340);
    auto* layout = new QVBoxLayout(bubble_);
    layout->setContentsMargins(14, 12, 14, 10);
    title_ = new QLabel(bubble_);
    QFont bold = title_->font();
    bold.setWeight(QFont::DemiBold);
    title_->setFont(bold);
    text_ = new QLabel(bubble_);
    text_->setWordWrap(true);
    layout->addWidget(title_);
    layout->addWidget(text_);
    auto* buttons = new QHBoxLayout;
    step_ = new QLabel(bubble_);
    step_->setEnabled(false);
    buttons->addWidget(step_);
    buttons->addStretch(1);
    auto* done = new QPushButton(tr("Got it"), bubble_);
    done->setObjectName(QStringLiteral("hintsDone"));
    next_ = new QPushButton(tr("Next"), bubble_);
    next_->setObjectName(QStringLiteral("hintsNext"));
    next_->setDefault(true);
    buttons->addWidget(done);
    buttons->addWidget(next_);
    layout->addLayout(buttons);
    connect(done, &QPushButton::clicked, this, &FirstRunHints::finish);
    connect(next_, &QPushButton::clicked, this, [this] { showHint(index_ + 1); });
    setFocusPolicy(Qt::StrongFocus);
}

void FirstRunHints::showHint(int index) {
    // Skip hints whose target is gone or out of sight.
    while (index < hints_.size() && (hints_[index].target.isNull() || !hints_[index].target->isVisible())) {
        ++index;
    }
    if (index >= hints_.size()) {
        finish();
        return;
    }
    index_ = index;
    title_->setText(hints_[index].title);
    text_->setText(hints_[index].text);
    step_->setText(tr("%1 of %2").arg(index + 1).arg(hints_.size()));
    next_->setText(index + 1 < hints_.size() ? tr("Next") : tr("Done"));
    place();
    next_->setFocus();
    bubble_->setAccessibleName(hints_[index].title);
    bubble_->setAccessibleDescription(hints_[index].text);
}

void FirstRunHints::place() {
    if (index_ < 0 || hints_[index_].target.isNull()) {
        return;
    }
    QWidget* target = hints_[index_].target;
    targetRect_ = QRect(target->mapTo(parentWidget(), QPoint(0, 0)), target->size()).adjusted(-4, -4, 4, 4);
    bubble_->adjustSize();
    // Below the target if there is room, else above; kept inside the window.
    QPoint at(targetRect_.left(), targetRect_.bottom() + 12);
    if (at.y() + bubble_->height() > height()) {
        at.setY(targetRect_.top() - 12 - bubble_->height());
    }
    at.setX(std::clamp(at.x(), 8, std::max(8, width() - bubble_->width() - 8)));
    at.setY(std::clamp(at.y(), 8, std::max(8, height() - bubble_->height() - 8)));
    bubble_->move(at);
    update();
}

void FirstRunHints::finish() {
    parentWidget()->removeEventFilter(this);
    close();
}

bool FirstRunHints::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        place();
    }
    return QWidget::eventFilter(watched, event);
}

void FirstRunHints::keyPressEvent(QKeyEvent* event) {
    // Escape from anywhere in the bubble ends the tour; the buttons pass it up.
    if (event->key() == Qt::Key_Escape) {
        finish();
        return;
    }
    QWidget::keyPressEvent(event);
}

void FirstRunHints::paintEvent(QPaintEvent*) {
    // Dim everything but the target, which keeps a highlighted frame.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath outside;
    outside.addRect(rect());
    QPainterPath hole;
    hole.addRoundedRect(targetRect_, 6, 6);
    p.fillPath(outside.subtracted(hole), QColor(0, 0, 0, 90));
    p.setPen(QPen(palette().color(QPalette::Highlight), 2));
    p.drawRoundedRect(targetRect_, 6, 6);
}
