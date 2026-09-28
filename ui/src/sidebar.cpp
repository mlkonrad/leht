// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sidebar.hpp"

#include "icons.hpp"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

Sidebar::Sidebar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sidebar"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* railWidget = new QWidget(this);
    railWidget->setObjectName(QStringLiteral("sidebarRail"));
    rail_ = new QVBoxLayout(railWidget);
    rail_->setContentsMargins(4, 6, 4, 6);
    rail_->setSpacing(4);
    rail_->addStretch(1);
    layout->addWidget(railWidget);

    body_ = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(body_);
    bodyLayout->setContentsMargins(0, 6, 0, 0);
    bodyLayout->setSpacing(4);
    title_ = new QLabel(body_);
    QFont bold = title_->font();
    bold.setWeight(QFont::DemiBold);
    title_->setFont(bold);
    title_->setContentsMargins(6, 0, 0, 0);
    bodyLayout->addWidget(title_);
    stack_ = new QStackedWidget(body_);
    bodyLayout->addWidget(stack_, 1);
    layout->addWidget(body_, 1);

    group_ = new QButtonGroup(this);
    group_->setExclusive(false);  // so the open tab can be clicked shut

    setStyleSheet(QStringLiteral(
        "QWidget#sidebarRail { border-right: 1px solid palette(mid); }"
        "QWidget#sidebarRail QToolButton { border: none; border-left: 3px solid transparent;"
        "  border-radius: 0; padding: 6px 5px 6px 4px; }"
        "QWidget#sidebarRail QToolButton:hover { background: palette(alternate-base); }"
        "QWidget#sidebarRail QToolButton:checked { background: palette(alternate-base);"
        "  border-left-color: palette(highlight); }"));
}

void Sidebar::addPanel(const QString& id, const QString& icon, const QString& title, QWidget* panel) {
    auto* button = new QToolButton(this);
    button->setObjectName(id + QStringLiteral("Tab"));
    button->setIcon(icons::named(icon));
    button->setIconSize(QSize(20, 20));
    button->setCheckable(true);
    button->setToolTip(title);
    button->setAccessibleName(title);
    button->setAutoRaise(true);
    button->setProperty("title", title);
    rail_->insertWidget(rail_->count() - 1, button);  // before the stretch
    group_->addButton(button);
    stack_->addWidget(panel);
    if (panel->accessibleName().isEmpty()) {
        panel->setAccessibleName(title);  // a screen reader names the panel it lands in
    }
    tabs_.insert(id, {button, panel});
    order_ << id;
    connect(button, &QToolButton::clicked, this, [this, id] {
        if (current_ == id && isExpanded()) {
            setExpanded(false);
        } else {
            showPanel(id);
        }
    });
    if (current_.isEmpty()) {
        select(id);
    }
}

void Sidebar::select(const QString& id) {
    const auto it = tabs_.constFind(id);
    if (it == tabs_.constEnd()) {
        return;
    }
    current_ = id;
    for (const Tab& t : std::as_const(tabs_)) {
        t.button->setChecked(t.button == it->button);
    }
    stack_->setCurrentWidget(it->page);
    title_->setText(it->button->property("title").toString());
}

void Sidebar::showPanel(const QString& id) {
    if (!tabs_.contains(id) || !isPanelAvailable(id)) {
        return;
    }
    const bool changed = current_ != id || !isExpanded();
    select(id);
    setExpanded(true);
    if (changed) {
        emit panelChanged(id);
    }
}

void Sidebar::setPanelAvailable(const QString& id, bool available) {
    const auto it = tabs_.constFind(id);
    if (it == tabs_.constEnd()) {
        return;
    }
    it->button->setVisible(available);
    if (!available && current_ == id) {
        for (const QString& other : std::as_const(order_)) {
            if (other != id && isPanelAvailable(other)) {
                select(other);
                emit panelChanged(isExpanded() ? other : QString());
                return;
            }
        }
    }
}

bool Sidebar::isPanelAvailable(const QString& id) const {
    const auto it = tabs_.constFind(id);
    return it != tabs_.constEnd() && !it->button->isHidden();
}

QString Sidebar::currentPanel() const {
    return isExpanded() ? current_ : QString();
}

void Sidebar::setExpanded(bool expanded) {
    if (expanded == isExpanded()) {
        return;
    }
    body_->setVisible(expanded);
    if (auto it = tabs_.constFind(current_); it != tabs_.constEnd()) {
        it->button->setChecked(expanded);
    }
    // Folded, only the rail is left: the splitter should not keep the width.
    // The rail's own width: sizeHint() would still count the body just hidden.
    setMaximumWidth(expanded ? QWIDGETSIZE_MAX : rail_->parentWidget()->sizeHint().width());
    emit expandedChanged(expanded);
    emit panelChanged(currentPanel());
}

bool Sidebar::isExpanded() const {
    return !body_->isHidden();
}
