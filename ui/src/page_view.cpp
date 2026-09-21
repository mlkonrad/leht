// SPDX-License-Identifier: AGPL-3.0-or-later
#include "page_view.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

PageView::PageView(QWidget* parent) : QAbstractScrollArea(parent) {
    setFrameShape(QFrame::NoFrame);
    viewport()->setBackgroundRole(QPalette::Dark);
    verticalScrollBar()->setSingleStep(40);
}

void PageView::setPages(const QVector<QSize>& baseSizes) {
    baseSizes_ = baseSizes;
    rendered_.clear();
    requested_.clear();
    lastReportedPage_ = -1;
    relayout();
    requestVisible();
}

void PageView::clear() {
    baseSizes_.clear();
    rendered_.clear();
    requested_.clear();
    relayout();
    viewport()->update();
}

QSize PageView::scaledSize(int page) const {
    const QSize base = baseSizes_.value(page);
    return QSize(std::lround(base.width() * zoom_),
                 std::lround(base.height() * zoom_));
}

int PageView::columnWidth() const {
    int w = 0;
    for (int p = 0; p < baseSizes_.size(); ++p) {
        w = std::max(w, scaledSize(p).width());
    }
    return w;
}

int PageView::totalHeight() const {
    int h = kMargin;
    for (int p = 0; p < baseSizes_.size(); ++p) {
        h += scaledSize(p).height() + kGap;
    }
    return h - kGap + kMargin;  // no trailing gap
}

int PageView::pageTop(int page) const {
    int y = kMargin;
    for (int p = 0; p < page; ++p) {
        y += scaledSize(p).height() + kGap;
    }
    return y;
}

void PageView::relayout() {
    const int contentW = columnWidth();
    const int contentH = totalHeight();
    const QSize vp = viewport()->size();

    horizontalScrollBar()->setRange(0, std::max(0, contentW - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setRange(0, std::max(0, contentH - vp.height()));
    verticalScrollBar()->setPageStep(vp.height());
}

void PageView::setZoom(double zoom) {
    zoom = std::clamp(zoom, 0.1, 12.0);
    if (std::abs(zoom - zoom_) < 1e-6) {
        return;
    }

    // Keep the page under the viewport centre roughly in place across a zoom.
    const int anchor = currentPage();
    const double intoPage =
        anchor >= 0 && scaledSize(anchor).height() > 0
            ? double(verticalScrollBar()->value() - pageTop(anchor)) /
                  scaledSize(anchor).height()
            : 0.0;

    zoom_ = zoom;
    relayout();
    if (anchor >= 0) {
        verticalScrollBar()->setValue(
            pageTop(anchor) + int(intoPage * scaledSize(anchor).height()));
    }

    // Every cached image is now the wrong size; keep them as stretched proxies
    // until sharp ones arrive, but request fresh renders.
    requested_.clear();
    bumpGeneration();
    requestVisible();
    viewport()->update();
}

void PageView::zoomBy(double factor) { setZoom(zoom_ * factor); }

void PageView::fitWidth() {
    if (baseSizes_.isEmpty()) {
        return;
    }
    int widestBase = 1;
    for (const QSize& s : baseSizes_) {
        widestBase = std::max(widestBase, s.width());
    }
    const int avail = viewport()->width() - 2 * kMargin;
    setZoom(double(avail) / widestBase);
}

int PageView::currentPage() const {
    if (baseSizes_.isEmpty()) {
        return -1;
    }
    const int mid = verticalScrollBar()->value() + viewport()->height() / 2;
    for (int p = 0; p < baseSizes_.size(); ++p) {
        if (mid < pageTop(p) + scaledSize(p).height() + kGap) {
            return p;
        }
    }
    return baseSizes_.size() - 1;
}

void PageView::bumpGeneration() {
    ++generation_;
    emit generationChanged(generation_);
}

void PageView::requestVisible() {
    if (baseSizes_.isEmpty()) {
        return;
    }
    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();

    for (int p = 0; p < baseSizes_.size(); ++p) {
        const int y0 = pageTop(p);
        const int y1 = y0 + scaledSize(p).height();
        // A one-viewport margin above and below, so scrolling stays ahead.
        const bool near = y1 >= top - viewport()->height() &&
                          y0 <= bottom + viewport()->height();
        if (!near) {
            continue;
        }
        const auto it = rendered_.constFind(p);
        const bool sharp =
            it != rendered_.constEnd() && std::abs(it->zoom - zoom_) < 1e-6;
        if (!sharp && !requested_.contains(p)) {
            requested_.insert(p);
            emit needRender(p, zoom_, generation_);
        }
    }
}

void PageView::onRendered(int page, double zoom, quint64 /*generation*/,
                          QImage image) {
    requested_.remove(page);
    // Accept it even if the zoom has moved on: a slightly-stale image scaled to
    // fit still beats a blank placeholder, and the fresh one will replace it.
    rendered_.insert(page, Rendered{image, zoom});
    viewport()->update();
}

void PageView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), palette().dark());

    if (baseSizes_.isEmpty()) {
        painter.setPen(palette().light().color());
        painter.drawText(viewport()->rect(), Qt::AlignCenter,
                         tr("Open a PDF to begin  (Ctrl+O)"));
        return;
    }

    const int scrollX = horizontalScrollBar()->value();
    const int scrollY = verticalScrollBar()->value();
    const int colW = columnWidth();
    const int top = scrollY;
    const int bottom = scrollY + viewport()->height();

    for (int p = 0; p < baseSizes_.size(); ++p) {
        const QSize size = scaledSize(p);
        const int y0 = pageTop(p);
        if (y0 + size.height() < top || y0 > bottom) {
            continue;  // not visible
        }

        // Centre each page in the column.
        const int x = kMargin + (colW - size.width()) / 2 - scrollX;
        const QRect pageRect(x, y0 - scrollY, size.width(), size.height());

        const auto it = rendered_.constFind(p);
        if (it != rendered_.constEnd() && !it->image.isNull()) {
            // Scale a stale-zoom image to the current page rect; Qt does this
            // fast, and it is replaced the moment the sharp render lands.
            painter.drawImage(pageRect, it->image);
        } else {
            painter.fillRect(pageRect, Qt::white);
        }
        painter.setPen(QColor(0, 0, 0, 60));
        painter.drawRect(pageRect);
    }

    const int page = currentPage();
    if (page != lastReportedPage_) {
        lastReportedPage_ = page;
        emit currentPageChanged(page);
    }
}

void PageView::resizeEvent(QResizeEvent* /*event*/) {
    relayout();
    requestVisible();
}

void PageView::scrollContentsBy(int /*dx*/, int /*dy*/) {
    requestVisible();
    viewport()->update();
}

void PageView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const double steps = event->angleDelta().y() / 120.0;
        zoomBy(std::pow(1.15, steps));
        event->accept();
    } else {
        QAbstractScrollArea::wheelEvent(event);
    }
}
