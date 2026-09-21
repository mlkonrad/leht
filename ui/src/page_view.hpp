// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QAbstractScrollArea>
#include <QHash>
#include <QImage>
#include <QSize>
#include <QVector>

/// Continuous vertical page view.
///
/// Lays every page out in a single scrolling column at the current zoom, paints
/// the pages that intersect the viewport, and asks for renders of the visible
/// pages (plus a small margin so scrolling stays ahead of the eye). Pages not
/// yet rendered show a plain placeholder rather than blocking.
///
/// The view holds no engine state — it knows only page sizes and receives
/// finished images. All rendering happens on the RenderWorker thread.
class PageView : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit PageView(QWidget* parent = nullptr);

    /// Page base sizes at zoom 1.0, from the worker's opened() signal.
    void setPages(const QVector<QSize>& baseSizes);
    void clear();

    [[nodiscard]] double zoom() const { return zoom_; }
    void setZoom(double zoom);
    void zoomBy(double factor);
    void fitWidth();

    [[nodiscard]] int pageCount() const { return baseSizes_.size(); }
    /// The page currently nearest the top of the viewport, 0-based.
    [[nodiscard]] int currentPage() const;

public slots:
    /// A finished render from the worker. Ignored if the zoom has since changed.
    void onRendered(int page, double zoom, quint64 generation, QImage image);

signals:
    /// The view wants `page` rendered at `zoom`. `generation` lets the worker
    /// drop this request if a newer one has superseded it.
    void needRender(int page, double zoom, quint64 generation);
    /// The generation advanced (scroll or zoom); the worker should catch up.
    void generationChanged(quint64 generation);
    void currentPageChanged(int page);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    static constexpr int kGap = 12;      // px between pages, at any zoom
    static constexpr int kMargin = 8;     // px around the column

    [[nodiscard]] QSize scaledSize(int page) const;
    [[nodiscard]] int columnWidth() const;
    [[nodiscard]] int totalHeight() const;
    /// Top y of `page` in content coordinates.
    [[nodiscard]] int pageTop(int page) const;
    void relayout();
    void requestVisible();
    void bumpGeneration();

    QVector<QSize> baseSizes_;             // at zoom 1.0
    double zoom_ = 1.0;
    quint64 generation_ = 0;
    int lastReportedPage_ = -1;

    /// Rendered pages, keyed by page index. Each entry remembers the zoom it was
    /// rendered at so a stale-zoom image can be shown (scaled) until the sharp
    /// one arrives, rather than flashing a placeholder.
    struct Rendered {
        QImage image;
        double zoom = 0.0;
    };
    QHash<int, Rendered> rendered_;
    QSet<int> requested_;                  // in flight at the current generation
};
