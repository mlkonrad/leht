// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QList>
#include <QListWidget>
#include <QSet>
#include <QVector>

class QAction;

/// The Pages mode: every page as a thumbnail in a grid, to select several of,
/// drag into a new order, or drop files between. It changes nothing itself --
/// it asks, and the window turns the request into an edit for the worker; the
/// grid is rebuilt from the document the edit leaves.
class PageGrid : public QListWidget {
    Q_OBJECT

public:
    explicit PageGrid(QWidget* parent = nullptr);

    static constexpr int kThumbWidth = 150;

    void setPageCount(int pageCount);
    /// The document changed: everything is fetched again, keeping the old
    /// pictures until the new ones come.
    void invalidate();
    /// Selected pages, 0-based and ascending; the current one if none is.
    [[nodiscard]] QVector<int> selectedPages() const;
    /// As a range spec for the edit ("1-3,7"), 1-based.
    [[nodiscard]] static QString rangeSpec(const QVector<int>& pages);
    /// Selects exactly `pages` (after an edit, so a moved block stays selected).
    void selectPages(const QVector<int>& pages);
    /// Where a paste or insert goes: after the last selected page, else the end.
    [[nodiscard]] int insertionPoint() const;
    /// The right-click menu's entries (the window's own actions).
    void setContextActions(const QList<QAction*>& actions);

public slots:
    void onThumbnail(int page, const QImage& image);

signals:
    void needThumbnail(int page, int targetWidth);
    /// Move `pages` (0-based) to stand before the page now at `before`.
    void moveRequested(const QVector<int>& pages, int before);
    /// Files dropped between pages: insert them before `before`.
    void filesDropped(const QStringList& paths, int before);
    /// A page double-clicked: show it in the reading view.
    void openPage(int page);
    /// Delete or Backspace with pages selected.
    void deletePressed();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void requestVisible();
    /// The gap a drop at `pos` falls into: the page it goes before.
    [[nodiscard]] int dropPosition(const QPoint& pos) const;

    QSet<int> requested_;
    QList<QAction*> contextActions_;
    int dropMarker_ = -1;  ///< the gap being dragged over, drawn as a bar
};
