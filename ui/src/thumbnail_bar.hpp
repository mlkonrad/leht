// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QListWidget>
#include <QSet>

/// A vertical strip of page thumbnails.
///
/// Thumbnails are loaded lazily: the bar asks for renders only of the items
/// currently in view (plus a small margin), so a 1000-page document does not
/// render 1000 thumbnails up front. Clicking a thumbnail navigates to its page,
/// and the bar highlights the page the main view is showing.
class ThumbnailBar : public QListWidget {
    Q_OBJECT

public:
    explicit ThumbnailBar(QWidget* parent = nullptr);

    /// Target thumbnail width in device-independent pixels.
    static constexpr int kThumbWidth = 132;

    /// Rebuilds the strip with `pageCount` placeholder items.
    void setPageCount(int pageCount);
    void clearThumbnails();

    /// Highlights `page` and scrolls it into view without emitting a navigation.
    void setCurrentPageQuiet(int page);

public slots:
    /// A finished thumbnail from the worker.
    void onThumbnail(int page, const QImage& image);

signals:
    /// The bar wants `page` rendered as a thumbnail `targetWidth` px wide.
    void needThumbnail(int page, int targetWidth);
    /// The user picked a page.
    void pageChosen(int page);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void requestVisible();

    QSet<int> requested_;  // thumbnails asked for or already delivered
    bool syncing_ = false;  // suppress navigation while syncing the highlight
};
