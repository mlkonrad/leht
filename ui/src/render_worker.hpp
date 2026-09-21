// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QAtomicInteger>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include "outline_model.hpp"

#include <memory>

namespace leht {
class Context;
class Document;
class Renderer;
class PageCache;
class TextPage;
}  // namespace leht

/// Owns the whole engine and runs it on its own thread.
///
/// This exists because of the threading finding (docs/threading.md): MuPDF
/// requires that one thread touch the document, and rendering must not block
/// the UI. So the Context, Document and Renderer are all created and used HERE,
/// on the worker thread — never on the GUI thread. The GUI talks to this object
/// only through queued signals and slots.
///
/// Requests carry a monotonic `generation`. When the user scrolls or zooms, the
/// view bumps the generation; the worker skips any queued request older than
/// the latest before spending time on it, so a fast scroll does not render a
/// backlog of pages nobody is looking at any more.
class RenderWorker : public QObject {
    Q_OBJECT

public:
    RenderWorker();
    ~RenderWorker() override;

    /// Called from the GUI thread. The worker reads it before each render and
    /// drops stale ones. Atomic because it is written and read across threads.
    void setGeneration(quint64 generation) { generation_.storeRelease(generation); }

public slots:
    /// Opens a document (worker thread). Emits opened() on success, failed() on
    /// a real error, or passwordRequired() if the file is encrypted -- in which
    /// case the document is held open awaiting authenticate().
    void open(const QString& path);

    /// Tries `password` on a document opened but awaiting one. Continues the
    /// open on success (emits opened()); re-emits passwordRequired(retry=true)
    /// on a wrong password.
    void authenticate(const QString& password);

    /// Renders one page at `zoom` to RGB. Skipped if `generation` is behind the
    /// latest set via setGeneration(). Emits rendered() on success.
    void render(int page, double zoom, int rotation, quint64 generation);

    /// Searches every page for `needle`. Emits pageMatches() per page as it goes
    /// (so highlights appear progressively) then searchFinished(). Match boxes
    /// are in BASE coordinates — page pixels at zoom 1.0 — so the view scales
    /// them to the current zoom and they survive zoom changes without re-search.
    void search(const QString& needle);

    /// Selects text between two points, given in base coordinates, on one page.
    /// `mode` is a leht::SelectMode cast to int. Emits selectionReady().
    void selectRegion(int page, QPointF aBase, QPointF bBase, int mode);

    /// Renders a small thumbnail of `page`, scaled so its width is about
    /// `targetWidth` px. Emits thumbnailReady(). Rendered outside the page
    /// cache so it does not evict full-size pages.
    void renderThumbnail(int page, int targetWidth);

    /// Renders `page` at `zoom` and RETURNS the image, for callers that need it
    /// synchronously (printing). Invoked from the GUI thread with a blocking
    /// queued connection; returns a null image on failure. Does not touch the
    /// page cache or the generation.
    Q_INVOKABLE QImage renderAt(int page, double zoom);

signals:
    void opened(int pageCount, QVector<QSize> baseSizes);
    void outlineReady(QVector<OutlineRow> rows);
    void passwordRequired(bool retry);
    void failed(const QString& message);
    void rendered(int page, double zoom, int rotation, quint64 generation, QImage image);
    void pageMatches(int page, QVector<QRectF> boxes);
    void searchFinished(int totalMatches);
    void selectionReady(int page, QVector<QRectF> boxes, QString text);
    void thumbnailReady(int page, QImage image);

private:
    /// A text layer for `page`, built at zoom 1.0 and cached. Both search and
    /// selection use it; caching avoids re-extracting a page's text per query.
    leht::TextPage* textPage(int page);

    /// Finishes opening an unlocked document: sets up the renderer and emits
    /// opened() and outlineReady(). Shared by open() and authenticate().
    void finishOpen();

    std::unique_ptr<leht::Context> ctx_;
    std::unique_ptr<leht::Document> doc_;
    std::unique_ptr<leht::Renderer> renderer_;
    std::unique_ptr<leht::PageCache> cache_;
    QHash<int, std::shared_ptr<leht::TextPage>> textPages_;
    QAtomicInteger<quint64> generation_ = 0;
};
