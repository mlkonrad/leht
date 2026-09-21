// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QAtomicInteger>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>

namespace leht {
class Context;
class Document;
class Renderer;
class PageCache;
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
    /// Opens a document (worker thread). Emits opened() or failed().
    void open(const QString& path);

    /// Renders one page at `zoom` to RGB. Skipped if `generation` is behind the
    /// latest set via setGeneration(). Emits rendered() on success.
    void render(int page, double zoom, quint64 generation);

signals:
    void opened(int pageCount, QVector<QSize> baseSizes);
    void failed(const QString& message);
    void rendered(int page, double zoom, quint64 generation, QImage image);

private:
    std::unique_ptr<leht::Context> ctx_;
    std::unique_ptr<leht::Document> doc_;
    std::unique_ptr<leht::Renderer> renderer_;
    std::unique_ptr<leht::PageCache> cache_;
    QAtomicInteger<quint64> generation_ = 0;
};
