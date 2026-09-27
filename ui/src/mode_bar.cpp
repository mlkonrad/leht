// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mode_bar.hpp"

#include "icons.hpp"

#include <QAction>
#include <QHBoxLayout>
#include <QTabBar>
#include <QToolBar>

ModeBar::ModeBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("modeBar"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 2);
    layout->setSpacing(12);
    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("modeTabs"));
    tabs_->setDocumentMode(true);
    tabs_->setDrawBase(false);
    tabs_->setExpanding(false);
    tabs_->setIconSize(QSize(16, 16));
    layout->addWidget(tabs_);
    tools_ = new QToolBar(this);
    tools_->setObjectName(QStringLiteral("modeTools"));
    tools_->setIconSize(QSize(20, 20));
    tools_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    layout->addWidget(tools_, 1);
    // Flat tabs, the current one underlined: a switch between modes, not
    // folders of a notebook.
    setStyleSheet(QStringLiteral(
        "QTabBar#modeTabs::tab { border: none; border-bottom: 2px solid transparent;"
        "  padding: 5px 12px; margin-right: 2px; background: transparent; }"
        "QTabBar#modeTabs::tab:hover { background: palette(alternate-base); }"
        "QTabBar#modeTabs::tab:selected { border-bottom-color: palette(highlight); font-weight: 600; }"
        "QTabBar#modeTabs::tab:disabled { color: palette(mid); }"
        "QWidget#modeBar { border-bottom: 1px solid palette(mid); }"));
    setAttribute(Qt::WA_StyledBackground);

    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        showTools(index);
        if (index >= 0) {
            emit modeChanged(tabs_->tabData(index).toString());
        }
    });
}

void ModeBar::addMode(const QString& id, const QString& icon, const QString& title, const QString& tip,
                      const QList<QAction*>& tools) {
    const QSignalBlocker quiet(tabs_);  // the first tab becomes current: not a user's change
    const int index = tabs_->addTab(icons::named(icon), title);
    tabs_->setTabData(index, id);
    tabs_->setTabToolTip(index, tip);
    tabs_->setAccessibleTabName(index, title);
    modeTools_.push_back(tools);
    tips_.push_back(tip);
    if (index == tabs_->currentIndex()) {
        showTools(index);
    }
}

void ModeBar::showTools(int index) {
    tools_->clear();
    if (index < 0 || index >= modeTools_.size()) {
        return;
    }
    for (QAction* a : modeTools_[index]) {
        tools_->addAction(a);
    }
}

void ModeBar::setMode(const QString& id) {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->tabData(i).toString() == id && tabs_->isTabEnabled(i)) {
            tabs_->setCurrentIndex(i);
            return;
        }
    }
}

QString ModeBar::mode() const {
    return tabs_->currentIndex() < 0 ? QString() : tabs_->tabData(tabs_->currentIndex()).toString();
}

void ModeBar::setModeEnabled(const QString& id, bool enabled, const QString& why) {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->tabData(i).toString() != id) {
            continue;
        }
        tabs_->setTabEnabled(i, enabled);
        tabs_->setTabToolTip(i, enabled || why.isEmpty() ? tips_.value(i) : why);
        if (!enabled && tabs_->currentIndex() == i) {
            setMode(tabs_->tabData(0).toString());  // back to Read, which is always allowed
        }
    }
}

void ModeBar::setToolButtonStyle(Qt::ToolButtonStyle style) {
    tools_->setToolButtonStyle(style);
}
