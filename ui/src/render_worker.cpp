// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render_worker.hpp"

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/page_cache.hpp"
#include "leht/renderer.hpp"

#include <QThread>

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

        const int pages = doc_->page_count();
        QVector<QSize> sizes;
        sizes.reserve(pages);
        for (int p = 0; p < pages; ++p) {
            const leht::PageSize size = renderer_->page_size(p, 1.0F);
            sizes.push_back(QSize(size.width, size.height));
        }
        emit opened(pages, sizes);
    } catch (const leht::Error& e) {
        emit failed(QString::fromUtf8(e.what()));
    } catch (const std::exception& e) {
        emit failed(QString::fromUtf8(e.what()));
    }
}

void RenderWorker::render(int page, double zoom, quint64 generation) {
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

        // Serve from the byte-budgeted cache when possible.
        if (auto hit = cache_->get(page, z)) {
            image = toQImage(*hit);
        } else if (auto bmp = renderer_->render(page, z)) {
            image = toQImage(*bmp);
            cache_->put(page, z, 0, std::move(*bmp));
        } else {
            return;  // render was cancelled; nothing to show
        }

        if (!image.isNull()) {
            emit rendered(page, zoom, generation, image);
        }
    } catch (const leht::Error&) {
        // A single bad page must not take the viewer down; leave it blank.
    }
}
