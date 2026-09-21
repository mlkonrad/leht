// SPDX-License-Identifier: AGPL-3.0-or-later
#include "page_view.hpp"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMouseEvent>
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
    setFocusPolicy(Qt::StrongFocus);  // so PageUp/Down, Home/End, arrows arrive
}

void PageView::setPages(const QVector<QSize>& baseSizes) {
    baseSizes_ = baseSizes;
    rendered_.clear();
    requested_.clear();
    failed_.clear();
    lastReportedPage_ = -1;
    relayout();
    requestVisible();
}

void PageView::clear() {
    baseSizes_.clear();
    rendered_.clear();
    requested_.clear();
    failed_.clear();
    clearMatches();
    relayout();
    viewport()->update();
}

QSize PageView::scaledSize(int page) const {
    QSize base = baseSizes_.value(page);
    if (rotation_ == 90 || rotation_ == 270) {
        base.transpose();  // a quarter-turn swaps width and height
    }
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
    const bool turned = rotation_ == 90 || rotation_ == 270;
    int widest = 1;
    for (const QSize& s : baseSizes_) {
        widest = std::max(widest, turned ? s.height() : s.width());
    }
    const int avail = viewport()->width() - 2 * kMargin;
    setZoom(double(avail) / widest);
}

void PageView::fitPage() {
    const int page = currentPage();
    if (page < 0) {
        return;
    }
    const bool turned = rotation_ == 90 || rotation_ == 270;
    const QSize base = baseSizes_.value(page);
    const double w = turned ? base.height() : base.width();
    const double h = turned ? base.width() : base.height();
    const double zx = (viewport()->width() - 2 * kMargin) / w;
    const double zy = (viewport()->height() - 2 * kMargin) / h;
    setZoom(std::min(zx, zy));
}

void PageView::rotateBy(int degrees) {
    rotation_ = (((rotation_ + degrees) % 360) + 360) % 360;
    // Every cached image is now the wrong orientation; drop them and re-render.
    rendered_.clear();
    requested_.clear();
    relayout();
    bumpGeneration();
    requestVisible();
    viewport()->update();
}

void PageView::nextPage() { goToPage(std::min(currentPage() + 1, pageCount() - 1)); }
void PageView::previousPage() { goToPage(std::max(currentPage() - 1, 0)); }
void PageView::firstPage() { goToPage(0); }
void PageView::lastPage() { goToPage(pageCount() - 1); }

void PageView::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_PageDown:
        case Qt::Key_Space:
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepAdd);
            break;
        case Qt::Key_PageUp:
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
            break;
        case Qt::Key_Home:
            firstPage();
            break;
        case Qt::Key_End:
            lastPage();
            break;
        case Qt::Key_Down:
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd);
            break;
        case Qt::Key_Up:
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
            break;
        default:
            QAbstractScrollArea::keyPressEvent(event);
            return;
    }
    event->accept();
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

void PageView::goToPage(int page, double yBase) {
    if (page < 0 || page >= baseSizes_.size()) {
        return;
    }
    const int y = pageTop(page) + int(yBase * zoom_) - kMargin;
    verticalScrollBar()->setValue(std::clamp(y, 0, verticalScrollBar()->maximum()));
    requestVisible();
    viewport()->update();
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
            emit needRender(p, zoom_, rotation_, generation_);
        }
    }
}

void PageView::onRendered(int page, double zoom, int rotation,
                          quint64 /*generation*/, QImage image) {
    requested_.remove(page);
    // Accept it even if zoom/rotation moved on: a scaled stale image beats a
    // blank placeholder, and the fresh one will replace it.
    rendered_.insert(page, Rendered{image, zoom, rotation});
    viewport()->update();
}

QRect PageView::pageRectInViewport(int page) const {
    const QSize size = scaledSize(page);
    const int x = kMargin + (columnWidth() - size.width()) / 2 -
                  horizontalScrollBar()->value();
    const int y = pageTop(page) - verticalScrollBar()->value();
    return QRect(x, y, size.width(), size.height());
}

QRectF PageView::baseRectToViewport(int page, const QRectF& base) const {
    const QRect pr = pageRectInViewport(page);
    return QRectF(pr.x() + base.x() * zoom_, pr.y() + base.y() * zoom_,
                  base.width() * zoom_, base.height() * zoom_);
}

void PageView::viewportToPage(QPoint pos, int& page, QPointF& base) const {
    page = -1;
    for (int p = 0; p < baseSizes_.size(); ++p) {
        const QRect pr = pageRectInViewport(p);
        if (pr.top() > pos.y()) {
            break;  // pages are top-to-bottom; past the cursor already
        }
        if (pr.contains(pos)) {
            page = p;
            base = QPointF((pos.x() - pr.x()) / zoom_, (pos.y() - pr.y()) / zoom_);
            return;
        }
    }
}

// --- Find -------------------------------------------------------------------

void PageView::clearMatches() {
    matches_.clear();
    matchOrder_.clear();
    currentMatch_ = -1;
    selectionPage_ = -1;
    selectionBoxes_.clear();
    selectionText_.clear();
    viewport()->update();
}

void PageView::addMatches(int page, const QVector<QRectF>& boxes) {
    matches_[page] = boxes;
    viewport()->update();
}

void PageView::finishMatches(int /*total*/) {
    // Build a page-ordered flat list for next/prev.
    matchOrder_.clear();
    QList<int> pages = matches_.keys();
    std::sort(pages.begin(), pages.end());
    for (int p : pages) {
        for (int i = 0; i < matches_[p].size(); ++i) {
            matchOrder_.push_back({p, i});
        }
    }
    currentMatch_ = matchOrder_.isEmpty() ? -1 : 0;
    if (currentMatch_ >= 0) {
        scrollToCurrentMatch();
    }
    emit matchNavigated(currentMatch_, matchOrder_.size());
    viewport()->update();
}

void PageView::nextMatch() {
    if (matchOrder_.isEmpty()) {
        return;
    }
    currentMatch_ = (currentMatch_ + 1) % matchOrder_.size();
    scrollToCurrentMatch();
    emit matchNavigated(currentMatch_, matchOrder_.size());
    viewport()->update();
}

void PageView::prevMatch() {
    if (matchOrder_.isEmpty()) {
        return;
    }
    currentMatch_ =
        (currentMatch_ - 1 + matchOrder_.size()) % matchOrder_.size();
    scrollToCurrentMatch();
    emit matchNavigated(currentMatch_, matchOrder_.size());
    viewport()->update();
}

void PageView::scrollToCurrentMatch() {
    if (currentMatch_ < 0 || currentMatch_ >= matchOrder_.size()) {
        return;
    }
    const auto [page, idx] = matchOrder_.at(currentMatch_);
    const QRectF box = matches_.value(page).value(idx);
    // Put the match a third of the way down the viewport.
    const int targetY = pageTop(page) + int(box.center().y() * zoom_) -
                        viewport()->height() / 3;
    verticalScrollBar()->setValue(targetY);
    requestVisible();
}

// --- Selection --------------------------------------------------------------

void PageView::setSelection(int page, const QVector<QRectF>& boxes,
                            const QString& text) {
    selectionPage_ = page;
    selectionBoxes_ = boxes;
    selectionText_ = text;
    viewport()->update();
}

void PageView::copySelection() const {
    if (!selectionText_.isEmpty()) {
        QApplication::clipboard()->setText(selectionText_);
    }
}

void PageView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    int page = -1;
    QPointF base;
    viewportToPage(event->pos(), page, base);
    if (page < 0) {
        return;
    }
    selecting_ = true;
    selectAnchorPage_ = page;
    selectAnchorBase_ = base;
    selectionPage_ = -1;
    selectionBoxes_.clear();
    selectionText_.clear();
    viewport()->update();
}

void PageView::mouseMoveEvent(QMouseEvent* event) {
    if (!selecting_) {
        return;
    }
    int page = -1;
    QPointF base;
    viewportToPage(event->pos(), page, base);
    // Selection stays on the anchor page; clamp the far point to it.
    if (page != selectAnchorPage_) {
        return;
    }
    emit selectRequested(selectAnchorPage_, selectAnchorBase_, base,
                         /*Chars=*/0);
}

void PageView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        selecting_ = false;
    }
}

void PageView::mouseDoubleClickEvent(QMouseEvent* event) {
    int page = -1;
    QPointF base;
    viewportToPage(event->pos(), page, base);
    if (page < 0) {
        return;
    }
    // Word select: same point twice, Words mode.
    emit selectRequested(page, base, base, /*Words=*/1);
}

void PageView::markPageFailed(int page) {
    if (page < 0 || page >= baseSizes_.size()) {
        return;
    }
    failed_.insert(page);
    requested_.remove(page);
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
        if (failed_.contains(p)) {
            painter.fillRect(pageRect, QColor(236, 236, 236));
            painter.setPen(QColor(90, 90, 90));
            painter.drawText(pageRect.adjusted(24, 24, -24, -24),
                             Qt::AlignCenter | Qt::TextWordWrap,
                             tr("This page could not be displayed safely.\n"
                                "It crashed or stalled the document parser; the rest "
                                "of the document is unaffected."));
        } else if (it != rendered_.constEnd() && !it->image.isNull()) {
            // Scale a stale-zoom image to the current page rect; Qt does this
            // fast, and it is replaced the moment the sharp render lands.
            painter.drawImage(pageRect, it->image);
        } else {
            painter.fillRect(pageRect, Qt::white);
        }
        painter.setPen(QColor(0, 0, 0, 60));
        painter.drawRect(pageRect);

        // Search matches on this page: translucent yellow, current one orange.
        const auto mit = matches_.constFind(p);
        if (mit != matches_.constEnd()) {
            for (int i = 0; i < mit->size(); ++i) {
                const QRectF box = baseRectToViewport(p, mit->at(i));
                const bool isCurrent =
                    currentMatch_ >= 0 &&
                    matchOrder_.value(currentMatch_) == qMakePair(p, i);
                painter.fillRect(box, isCurrent ? QColor(255, 150, 0, 160)
                                                : QColor(255, 235, 0, 110));
            }
        }

        // Selection on this page: translucent blue.
        if (selectionPage_ == p) {
            for (const QRectF& b : selectionBoxes_) {
                painter.fillRect(baseRectToViewport(p, b), QColor(60, 120, 220, 90));
            }
        }
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
