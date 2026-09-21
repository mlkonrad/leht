// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render_worker.hpp"

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/page_cache.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"

#include <QThread>

#include <functional>
#include <utility>

namespace {

/// Copies a leht::Bitmap (RGB, own stride) into a self-owning QImage. The copy
/// is deliberate: the Bitmap is a worker-thread local, and the QImage travels
/// to the GUI thread by value through a queued signal, so it must own its data.
QImage toQImage(const leht::Bitmap& bmp) {
    if (bmp.empty() || bmp.channels != 3) {
        return {};
    }
    const QImage view(bmp.pixels.data(), bmp.width, bmp.height, bmp.stride,
                      QImage::Format_RGB888);
    return view.copy();  // detaches from the Bitmap's storage
}

}  // namespace

RenderWorker::RenderWorker() = default;
RenderWorker::~RenderWorker() = default;

void RenderWorker::open(const QString& path) {
    try {
        // Tear down any previous document in REVERSE dependency order, before
        // the old Context is dropped. The renderer, text pages and document all
        // hold pointers into the context, so the context must outlive them --
        // reassigning ctx_ first would free it out from under them and crash on
        // their eventual destruction. This is what a reopen used to do.
        textPages_.clear();
        renderer_.reset();
        cache_.reset();
        doc_.reset();
        ctx_.reset();

        // Everything MuPDF is created on this thread and only ever touched here.
        ctx_ = std::make_unique<leht::Context>();
        doc_ = std::make_unique<leht::Document>(
            leht::Document::open(*ctx_, path.toStdString()));

        if (doc_->needs_password()) {
            emit failed(QStringLiteral("This document is password-protected."));
            doc_.reset();
            return;
        }

        renderer_ = std::make_unique<leht::Renderer>(*ctx_, *doc_);
        cache_ = std::make_unique<leht::PageCache>();
        textPages_.clear();

        const int pages = doc_->page_count();
        QVector<QSize> sizes;
        sizes.reserve(pages);
        for (int p = 0; p < pages; ++p) {
            const leht::PageSize size = renderer_->page_size(p, 1.0F);
            sizes.push_back(QSize(size.width, size.height));
        }
        emit opened(pages, sizes);

        // Outline is optional; a document without one simply emits no rows.
        QVector<OutlineRow> rows;
        std::function<void(const std::vector<leht::OutlineItem>&, int)> flatten =
            [&](const std::vector<leht::OutlineItem>& items, int depth) {
                for (const leht::OutlineItem& item : items) {
                    rows.push_back(OutlineRow{depth,
                                              QString::fromStdString(item.title),
                                              item.page, item.y});
                    flatten(item.children, depth + 1);
                }
            };
        flatten(doc_->outline(), 0);
        emit outlineReady(rows);
    } catch (const leht::Error& e) {
        emit failed(QString::fromUtf8(e.what()));
    } catch (const std::exception& e) {
        emit failed(QString::fromUtf8(e.what()));
    }
}

void RenderWorker::render(int page, double zoom, int rotation,
                          quint64 generation) {
    if (renderer_ == nullptr) {
        return;
    }
    // Drop stale work: if the view has moved on, this page is no longer wanted.
    if (generation < generation_.loadAcquire()) {
        return;
    }

    try {
        const auto z = static_cast<float>(zoom);
        auto image = QImage();

        // The page cache is keyed by (page, zoom, rotation), so rotated and
        // unrotated views do not collide.
        if (auto hit = cache_->get(page, z, rotation)) {
            image = toQImage(*hit);
        } else if (auto bmp = renderer_->render(page, z, rotation)) {
            image = toQImage(*bmp);
            cache_->put(page, z, rotation, std::move(*bmp));
        } else {
            return;  // render was cancelled; nothing to show
        }

        if (!image.isNull()) {
            emit rendered(page, zoom, rotation, generation, image);
        }
    } catch (const leht::Error&) {
        // A single bad page must not take the viewer down; leave it blank.
    }
}

leht::TextPage* RenderWorker::textPage(int page) {
    if (auto it = textPages_.constFind(page); it != textPages_.constEnd()) {
        return it->get();
    }
    // Built at zoom 1.0: match and selection coordinates come out in base page
    // pixels, which the view scales to whatever zoom it is showing.
    auto tp = std::make_shared<leht::TextPage>(*ctx_, *doc_, page, 1.0F);
    return textPages_.insert(page, std::move(tp)).value().get();
}

void RenderWorker::search(const QString& needle) {
    if (doc_ == nullptr) {
        return;
    }
    if (needle.isEmpty()) {
        emit searchFinished(0);
        return;
    }

    const std::string q = needle.toStdString();
    const int pages = doc_->page_count();
    int total = 0;

    for (int p = 0; p < pages; ++p) {
        QVector<QRectF> boxes;
        try {
            for (const leht::SearchHit& hit : textPage(p)->search(q)) {
                // One box per quad, so a line-wrapped match highlights each run
                // without covering the gap. Bounding rect of the quad is the
                // usual highlight shape for text.
                for (const leht::TextQuad& quad : hit.quads) {
                    boxes.push_back(QRectF(quad.min_x(), quad.min_y(),
                                           quad.max_x() - quad.min_x(),
                                           quad.max_y() - quad.min_y()));
                }
            }
        } catch (const leht::Error&) {
            continue;  // an unreadable page just contributes no matches
        }
        if (!boxes.isEmpty()) {
            total += static_cast<int>(boxes.size());
            emit pageMatches(p, boxes);
        }
    }
    emit searchFinished(total);
}

void RenderWorker::selectRegion(int page, QPointF aBase, QPointF bBase,
                                int mode) {
    if (doc_ == nullptr || page < 0) {
        return;
    }
    try {
        const leht::Selection sel = textPage(page)->select(
            static_cast<float>(aBase.x()), static_cast<float>(aBase.y()),
            static_cast<float>(bBase.x()), static_cast<float>(bBase.y()),
            static_cast<leht::SelectMode>(mode));

        QVector<QRectF> boxes;
        boxes.reserve(static_cast<int>(sel.quads.size()));
        for (const leht::TextQuad& q : sel.quads) {
            boxes.push_back(QRectF(q.min_x(), q.min_y(), q.max_x() - q.min_x(),
                                   q.max_y() - q.min_y()));
        }
        emit selectionReady(page, boxes, QString::fromStdString(sel.text));
    } catch (const leht::Error&) {
        // No selection rather than a crash.
    }
}

void RenderWorker::renderThumbnail(int page, int targetWidth) {
    if (renderer_ == nullptr || doc_ == nullptr || page < 0) {
        return;
    }
    try {
        const leht::PageSize base = renderer_->page_size(page, 1.0F);
        if (base.width <= 0) {
            return;
        }
        const auto zoom = static_cast<float>(targetWidth) /
                          static_cast<float>(base.width);
        // Rendered directly, not through the page cache: a thumbnail must not
        // evict the full-size pages the reader is actually looking at.
        if (auto bmp = renderer_->render(page, zoom)) {
            const QImage img = toQImage(*bmp);
            if (!img.isNull()) {
                emit thumbnailReady(page, img);
            }
        }
    } catch (const leht::Error&) {
        // A bad page simply gets no thumbnail.
    }
}
