// SPDX-License-Identifier: AGPL-3.0-or-later
#include "page_grid.hpp"

#include "welcome_view.hpp"  // droppedFiles()

#include <QAction>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kThumbHeight = PageGrid::kThumbWidth * 4 / 3;

QIcon placeholder() {
    QPixmap pm(PageGrid::kThumbWidth, kThumbHeight);
    pm.fill(Qt::white);
    QPainter p(&pm);
    p.setPen(QColor(0, 0, 0, 40));
    p.drawRect(0, 0, pm.width() - 1, pm.height() - 1);
    return QIcon(pm);
}

}  // namespace

PageGrid::PageGrid(QWidget* parent) : QListWidget(parent) {
    setObjectName(QStringLiteral("pageGrid"));
    setViewMode(QListView::IconMode);
    setIconSize(QSize(kThumbWidth, kThumbHeight));
    setGridSize(QSize(kThumbWidth + 36, kThumbHeight + 44));
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Snap);
    setWrapping(true);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionRectVisible(true);
    // Drags start here and end here, but the drop is ours to interpret: the
    // list must not rearrange its own items (dropEvent asks for an edit).
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setSpacing(8);
    setAccessibleName(tr("Pages"));
    setAccessibleDescription(tr("Select pages to turn, delete or extract them; drag them to reorder"));

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { requestVisible(); });
    connect(this, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { emit openPage(row(item)); });
}

void PageGrid::setPageCount(int pageCount) {
    const QVector<int> keep = selectedPages();
    clear();
    requested_.clear();
    const QIcon ph = placeholder();
    for (int p = 0; p < pageCount; ++p) {
        auto* item = new QListWidgetItem(ph, QString::number(p + 1), this);
        item->setData(Qt::AccessibleTextRole, tr("Page %1").arg(p + 1));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(tr("Page %1").arg(p + 1));
    }
    selectPages(keep);
    requestVisible();
}

void PageGrid::invalidate() {
    requested_.clear();
    requestVisible();
}

QVector<int> PageGrid::selectedPages() const {
    QVector<int> pages;
    for (const QModelIndex& index : selectionModel()->selectedIndexes()) {
        pages.push_back(index.row());
    }
    if (pages.isEmpty() && currentRow() >= 0) {
        pages.push_back(currentRow());
    }
    std::sort(pages.begin(), pages.end());
    return pages;
}

QString PageGrid::rangeSpec(const QVector<int>& pages) {
    QStringList parts;
    for (int i = 0; i < pages.size();) {
        int j = i;
        while (j + 1 < pages.size() && pages[j + 1] == pages[j] + 1) {
            ++j;
        }
        parts << (i == j ? QString::number(pages[i] + 1)
                         : QStringLiteral("%1-%2").arg(pages[i] + 1).arg(pages[j] + 1));
        i = j + 1;
    }
    return parts.join(QLatin1Char(','));
}

void PageGrid::selectPages(const QVector<int>& pages) {
    clearSelection();
    for (const int p : pages) {
        if (QListWidgetItem* it = item(p)) {
            it->setSelected(true);
        }
    }
    if (!pages.isEmpty() && item(pages.first()) != nullptr) {
        setCurrentRow(pages.first(), QItemSelectionModel::NoUpdate);
        scrollToItem(item(pages.first()));
    }
}

int PageGrid::insertionPoint() const {
    const QVector<int> pages = selectedPages();
    return pages.isEmpty() ? count() : pages.last() + 1;
}

void PageGrid::setContextActions(const QList<QAction*>& actions) {
    contextActions_ = actions;
}

void PageGrid::onThumbnail(int page, const QImage& image) {
    if (page < 0 || page >= count() || image.isNull()) {
        return;
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(static_cast<qreal>(image.width()) / kThumbWidth);
    item(page)->setIcon(QIcon(pixmap));
}

void PageGrid::showEvent(QShowEvent* event) {
    QListWidget::showEvent(event);
    requestVisible();
}

void PageGrid::resizeEvent(QResizeEvent* event) {
    QListWidget::resizeEvent(event);
    requestVisible();
}

void PageGrid::requestVisible() {
    if (count() == 0 || !isVisible()) {
        return;
    }
    const int margin = viewport()->height() / 2;
    const QRect vp = viewport()->rect().adjusted(0, -margin, 0, margin);
    for (int p = 0; p < count(); ++p) {
        if (requested_.contains(p) || !visualItemRect(item(p)).intersects(vp)) {
            continue;
        }
        requested_.insert(p);
        emit needThumbnail(p, static_cast<int>(std::lround(kThumbWidth * devicePixelRatioF())));
    }
}

int PageGrid::dropPosition(const QPoint& pos) const {
    // The nearest item on the row, and which half of it the pointer is in.
    int best = -1;
    int bestDistance = 0;
    for (int p = 0; p < count(); ++p) {
        const QRect r = visualItemRect(item(p));
        if (pos.y() < r.top() - spacing() || pos.y() > r.bottom() + spacing()) {
            continue;
        }
        const int distance = std::abs(r.center().x() - pos.x());
        if (best < 0 || distance < bestDistance) {
            best = p;
            bestDistance = distance;
        }
    }
    if (best < 0) {
        return count();
    }
    return pos.x() < visualItemRect(item(best)).center().x() ? best : best + 1;
}

void PageGrid::dragEnterEvent(QDragEnterEvent* event) {
    if (event->source() == this || !droppedFiles(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
        dropMarker_ = dropPosition(event->position().toPoint());
        viewport()->update();
    }
}

void PageGrid::dragMoveEvent(QDragMoveEvent* event) {
    if (event->source() == this || !droppedFiles(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
        const int marker = dropPosition(event->position().toPoint());
        if (marker != dropMarker_) {
            dropMarker_ = marker;
            viewport()->update();
        }
    }
}

void PageGrid::dropEvent(QDropEvent* event) {
    const int before = dropPosition(event->position().toPoint());
    dropMarker_ = -1;
    viewport()->update();
    if (event->source() == this) {
        // Ignored as far as Qt knows, so the list keeps its items as they are.
        event->setDropAction(Qt::IgnoreAction);
        event->accept();
        emit moveRequested(selectedPages(), before);
        return;
    }
    const QStringList paths = droppedFiles(event->mimeData());
    if (!paths.isEmpty()) {
        event->acceptProposedAction();
        emit filesDropped(paths, before);
    }
}

void PageGrid::paintEvent(QPaintEvent* event) {
    QListWidget::paintEvent(event);
    if (dropMarker_ < 0 || count() == 0) {
        return;
    }
    // A bar in the gap the drop would land in.
    const bool after = dropMarker_ >= count();
    const QRect r = visualItemRect(item(after ? count() - 1 : dropMarker_));
    const int x = after ? r.right() + spacing() / 2 + 2 : r.left() - spacing() / 2 - 2;
    QPainter p(viewport());
    p.fillRect(QRect(x - 2, r.top(), 4, r.height()), palette().color(QPalette::Highlight));
}

void PageGrid::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && !selectedPages().isEmpty()) {
        emit deletePressed();
        return;
    }
    QListWidget::keyPressEvent(event);
}

void PageGrid::contextMenuEvent(QContextMenuEvent* event) {
    QListWidgetItem* under = itemAt(event->pos());
    if (under != nullptr && !under->isSelected()) {
        clearSelection();
        under->setSelected(true);
        setCurrentItem(under);
    }
    QMenu menu(this);
    for (QAction* a : contextActions_) {
        if (a == nullptr) {
            menu.addSeparator();
        } else {
            menu.addAction(a);
        }
    }
    if (!menu.isEmpty()) {
        menu.exec(event->globalPos());
    }
}
