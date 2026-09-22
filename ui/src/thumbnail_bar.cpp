// SPDX-License-Identifier: AGPL-3.0-or-later
#include "thumbnail_bar.hpp"

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QShowEvent>

namespace {

/// A neutral placeholder shown until a page's thumbnail arrives.
QIcon placeholder() {
    QPixmap pm(ThumbnailBar::kThumbWidth, ThumbnailBar::kThumbWidth * 4 / 3);
    pm.fill(Qt::white);
    QPainter p(&pm);
    p.setPen(QColor(0, 0, 0, 40));
    p.drawRect(0, 0, pm.width() - 1, pm.height() - 1);
    return QIcon(pm);
}

}  // namespace

ThumbnailBar::ThumbnailBar(QWidget* parent) : QListWidget(parent) {
    setViewMode(QListView::ListMode);
    setIconSize(QSize(kThumbWidth, kThumbWidth * 4 / 3));
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(false);
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSpacing(4);

    connect(this, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!syncing_ && row >= 0) {
            emit pageChosen(row);
        }
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { requestVisible(); });
}

void ThumbnailBar::setPageCount(int pageCount) {
    clearThumbnails();
    const QIcon ph = placeholder();
    for (int p = 0; p < pageCount; ++p) {
        auto* item = new QListWidgetItem(ph, QString::number(p + 1), this);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    }
    requestVisible();
}

void ThumbnailBar::clearThumbnails() {
    clear();
    requested_.clear();
}

void ThumbnailBar::invalidate(const QVector<int>& pages, bool allPages) {
    if (allPages) {
        requested_.clear();
    } else {
        for (const int p : pages) {
            requested_.remove(p);
        }
    }
    requestVisible();
}

void ThumbnailBar::onThumbnail(int page, const QImage& image) {
    if (page < 0 || page >= count() || image.isNull()) {
        return;
    }
    item(page)->setIcon(QIcon(QPixmap::fromImage(image)));
}

void ThumbnailBar::setCurrentPageQuiet(int page) {
    if (page < 0 || page >= count()) {
        return;
    }
    syncing_ = true;
    setCurrentRow(page);
    scrollToItem(item(page), QAbstractItemView::PositionAtCenter);
    syncing_ = false;
    requestVisible();
}

void ThumbnailBar::showEvent(QShowEvent* event) {
    QListWidget::showEvent(event);
    requestVisible();
}

void ThumbnailBar::requestVisible() {
    if (count() == 0) {
        return;
    }
    // Ask for every item whose row rect intersects the viewport, plus a margin
    // above and below so scrolling stays ahead.
    const int margin = viewport()->height();
    const QRect vp = viewport()->rect().adjusted(0, -margin, 0, margin);
    for (int p = 0; p < count(); ++p) {
        if (requested_.contains(p)) {
            continue;
        }
        const QRect r = visualItemRect(item(p));
        if (r.bottom() < vp.top() || r.top() > vp.bottom()) {
            continue;
        }
        requested_.insert(p);
        emit needThumbnail(p, kThumbWidth);
    }
}
