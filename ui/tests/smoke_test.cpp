// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless smoke test for the viewer's wiring. Not a unit test of core/ (that
// is covered elsewhere) — this proves the GUI path holds together: open a
// document, render on the worker thread, show real content, and that scrolling
// and zooming change what is drawn. Runs under QT_QPA_PLATFORM=offscreen.

#include "main_window.hpp"
#include "page_view.hpp"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QImage>
#include <QScrollBar>
#include <QTimer>

#include "render_worker.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok  %s\n", what);
    } else {
        std::printf("FAIL  %s\n", what);
        ++g_failures;
    }
}

/// Pumps the event loop for `ms`, letting the worker thread deliver renders.
void pump(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QImage grabView(MainWindow& w) { return w.centralWidget()->grab().toImage(); }

long inkSamples(const QImage& img) {
    long ink = 0;
    for (int y = 0; y < img.height(); y += 3) {
        for (int x = 0; x < img.width(); x += 3) {
            const QRgb p = img.pixel(x, y);
            if (qRed(p) < 220 && qGreen(p) < 220 && qBlue(p) < 220) {
                ++ink;
            }
        }
    }
    return ink;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    MainWindow window;
    window.resize(800, 1000);
    window.show();

    const std::string doc = std::string(LEHT_CORPUS_DIR) + "/text_10p.pdf";
    window.openPath(QString::fromStdString(doc));
    pump(2000);  // open + first renders

    auto* view = window.findChild<PageView*>();
    check(view != nullptr, "page view exists");
    if (view == nullptr) {
        return 1;
    }

    check(view->pageCount() == 10, "opened 10 pages");

    const QImage first = grabView(window);
    const long firstInk = inkSamples(first);
    std::printf("      page 1 ink samples: %ld\n", firstInk);
    check(firstInk > 200, "page 1 renders real content off the worker thread");

    // Scroll to the bottom: the drawn content must change.
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    pump(1500);
    const QImage scrolled = grabView(window);
    check(scrolled != first, "scrolling changes what is drawn");
    check(view->currentPage() > 0, "scrolling advances the current page");

    // Zoom in: page 1 back at the top, larger, still rendering.
    view->verticalScrollBar()->setValue(0);
    const double before = view->zoom();
    view->zoomBy(1.5);
    pump(1500);
    check(view->zoom() > before, "zoom increases the scale");
    const QImage zoomed = grabView(window);
    check(inkSamples(zoomed) > 200, "content still renders after zoom");

    // --- Find --------------------------------------------------------------
    // Drive search directly against the view+worker path. "quick" appears once
    // per line, 50 per page, 10 pages -> 500 matches.
    view->verticalScrollBar()->setValue(0);
    view->setZoom(1.0);
    pump(300);

    int lastTotal = -1;
    QObject::connect(view, &PageView::matchNavigated,
                     [&](int, int total) { lastTotal = total; });

    RenderWorker* worker = window.worker();
    check(worker != nullptr, "worker reachable");
    view->clearMatches();
    QMetaObject::invokeMethod(worker, "search", Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("quick")));
    pump(2500);
    std::printf("      matches for \"quick\": %d\n", view->matchCount());
    check(view->matchCount() == 500, "found 500 matches for a per-line word");
    check(lastTotal == 500, "match-navigated total reported to the UI");

    const QImage withMatches = grabView(window);
    check(withMatches != first, "match highlights change the drawing");

    // Navigate: next should advance the current match index.
    const int beforeIdx = view->currentMatchIndex();
    view->nextMatch();
    pump(200);
    check(view->currentMatchIndex() != beforeIdx, "next-match advances");

    // --- Selection ---------------------------------------------------------
    // Select a word region on page 1 via the worker, check text comes back.
    view->verticalScrollBar()->setValue(0);
    view->setZoom(1.0);
    pump(300);
    QString selText;
    // The worker's selectionReady is already wired to the view by MainWindow, so
    // invoking selectRegion drives the real path end to end.
    QMetaObject::invokeMethod(worker, "selectRegion", Qt::QueuedConnection,
                              Q_ARG(int, 0),
                              Q_ARG(QPointF, QPointF(60, 40)),
                              Q_ARG(QPointF, QPointF(60, 40)),
                              Q_ARG(int, 1 /*Words*/));
    pump(800);
    selText = view->selectedText();
    std::printf("      selected text: \"%s\"\n", selText.toUtf8().constData());
    check(!selText.isEmpty(), "word selection returns text");

    view->copySelection();
    check(QApplication::clipboard()->text() == selText,
          "copy puts the selection on the clipboard");

    if (g_failures > 0) {
        std::printf("%d smoke check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("viewer smoke test passed\n");
    return 0;
}
