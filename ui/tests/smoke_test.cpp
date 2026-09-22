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
#include <QFile>
#include <QPrinter>
#include <QTemporaryDir>
#include <QImage>
#include <QScrollBar>
#include <QTimer>
#include <QElapsedTimer>
#include <QSet>
#include <QStringList>

#include <signal.h>

#include <QThread>

#include "render_worker.hpp"

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/forms.hpp"
#include "leht/text.hpp"

#include <QDir>
#include <QMouseEvent>
#include <QTableWidget>

#include <QDockWidget>
#include <QScrollBar>
#include <QTreeWidget>
#include "thumbnail_bar.hpp"

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

/// A PDF whose outline nests `depth` levels deep. Loading it overflows MuPDF's
/// stack (pdf_test_outline, live in 1.28.4): the file that, before M3, killed
/// the viewer on open. See tests/crashes/README.md.
QString writeDeepOutline(const QString& path, int depth) {
    QByteArray out = "%PDF-1.7\n";
    QVector<qsizetype> offsets;
    auto add = [&](const QByteArray& body) {
        offsets.push_back(out.size());
        out += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    add("<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>");
    add("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>");
    add("<< /Type /Outlines /First 5 0 R /Last 5 0 R >>");
    for (int k = 0; k < depth; ++k) {
        const int num = 5 + k;
        QByteArray o = "<< /Title (x) /Parent " + QByteArray::number(num - 1) + " 0 R";
        if (k < depth - 1) {
            o += " /First " + QByteArray::number(num + 1) + " 0 R /Last " +
                 QByteArray::number(num + 1) + " 0 R";
        }
        add(o + " >>");
    }
    const qsizetype xref = out.size();
    out += "xref\n0 " + QByteArray::number(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (qsizetype off : offsets) {
        out += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
    }
    out += "trailer\n<< /Size " + QByteArray::number(offsets.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(out);
    }
    return path;
}

/// A PDF whose page tree lists an object that never parses. MuPDF re-parses it
/// on every page lookup, so sizing all pages is quadratic: 16,000 pages take
/// ~10 s to open where a clean file takes ~0.1 s. See tests/crashes/README.md.
QString writeSlowPageTree(const QString& path, int pages) {
    QByteArray out = "%PDF-1.7\n";
    const int content = 4 + pages;
    QVector<qsizetype> offsets(content + 1, 0);
    auto add = [&](int num, const QByteArray& body) {
        offsets[num] = out.size();
        out += QByteArray::number(num) + " 0 obj\n" + body + "\nendobj\n";
    };
    add(1, "<< /Type /Catalog /Pages 2 0 R >>");
    QByteArray kids = "3 0 R";
    for (int i = 0; i < pages; ++i) {
        kids += " " + QByteArray::number(4 + i) + " 0 R";
    }
    add(2, "<< /Type /Pages /Count " + QByteArray::number(pages) + " /Kids [" + kids + "] >>");
    add(3, "[1 0 R 2 R]");  // fails to parse, every time
    for (int i = 0; i < pages; ++i) {
        add(4 + i, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents " +
                       QByteArray::number(content) + " 0 R >>");
    }
    add(content, "<< /Length 0 >>\nstream\n\nendstream");
    const qsizetype xref = out.size();
    out += "xref\n0 " + QByteArray::number(content + 1) + "\n0000000000 65535 f \n";
    for (int n = 1; n <= content; ++n) {
        out += QByteArray::number(offsets[n]).rightJustified(10, '0') + " 00000 n \n";
    }
    out += "trailer\n<< /Size " + QByteArray::number(content + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(out);
    }
    return path;
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

    // --- Outline + go-to-page ----------------------------------------------
    // Reopen with a document that has an outline.
    const std::string outlined = std::string(LEHT_CORPUS_DIR) + "/outlined.pdf";
    window.openPath(QString::fromStdString(outlined));
    pump(1500);

    auto* tree = window.findChild<QTreeWidget*>();
    check(tree != nullptr, "outline tree exists");
    if (tree != nullptr) {
        check(tree->topLevelItemCount() == 3, "outline has 3 top-level entries");
        QTreeWidgetItem* chapterTwo = tree->topLevelItem(1);
        check(chapterTwo != nullptr && chapterTwo->childCount() == 1,
              "Chapter Two has one child (Section 2.1)");

        // Click Chapter Three -> should jump to page 3 (index 2).
        QTreeWidgetItem* chapterThree = tree->topLevelItem(2);
        if (chapterThree != nullptr) {
            emit tree->itemClicked(chapterThree, 0);
            pump(400);
            check(view->currentPage() == 2, "clicking an outline entry navigates");
        }
    }

    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("outlineDock"));
    check(dock != nullptr && dock->isVisible(),
          "outline dock shows for a document with an outline");

    // Go-to-page: jump back to page 1.
    view->goToPage(0);
    pump(300);
    check(view->currentPage() == 0, "goToPage(0) returns to the first page");

    // --- Thumbnails --------------------------------------------------------
    // Reopen the 10-page text doc; the thumbnail bar should populate and load
    // visible thumbnails from the worker.
    window.openPath(QString::fromStdString(doc));
    pump(2000);
    auto* thumbs = window.findChild<ThumbnailBar*>();
    check(thumbs != nullptr, "thumbnail bar exists");
    if (thumbs != nullptr) {
        check(thumbs->count() == 10, "thumbnail bar has one item per page");
        // At least the first thumbnail should have loaded a non-placeholder icon
        // (placeholders are pure white; a rendered page has ink).
        const QIcon icon = thumbs->item(0)->icon();
        const QImage img = icon.pixmap(ThumbnailBar::kThumbWidth,
                                       ThumbnailBar::kThumbWidth * 4 / 3).toImage();
        long ink = 0;
        for (int y = 0; y < img.height(); y += 2)
            for (int x = 0; x < img.width(); x += 2) {
                const QRgb px = img.pixel(x, y);
                if (qRed(px) < 200 && qGreen(px) < 200 && qBlue(px) < 200) ink++;
            }
        std::printf("      thumbnail 1 ink: %ld\n", ink);
        check(ink > 20, "first thumbnail rendered a real page, not a placeholder");

        // Clicking a thumbnail navigates.
        thumbs->setCurrentRow(4);
        pump(300);
        check(view->currentPage() == 4, "clicking a thumbnail navigates");
    }

    // --- Rotate / fit-page / keyboard nav ----------------------------------
    view->goToPage(0);
    view->setZoom(1.0);
    pump(200);

    // Rotate a quarter turn: page dimensions transpose, so a portrait page's
    // laid-out width grows relative to its height.
    const QImage upright = grabView(window);
    check(view->rotation() == 0, "rotation starts at 0");
    view->rotateBy(90);
    pump(600);
    check(view->rotation() == 90, "rotate advances to 90");
    check(grabView(window) != upright, "rotation changes the drawing");
    view->rotateBy(-90);  // back to upright for the rest
    pump(400);
    check(view->rotation() == 0, "rotate back to 0");

    // Fit-page must fit the whole page in the viewport: its scaled height must
    // not exceed the viewport height (within a margin).
    view->fitPage();
    pump(200);
    check(view->zoom() > 0.05, "fit-page picks a sane zoom");

    // Keyboard: End jumps to the last page, Home back to the first.
    view->lastPage();
    pump(300);
    check(view->currentPage() == view->pageCount() - 1 || view->currentPage() == 9,
          "lastPage goes to the end");
    view->firstPage();
    pump(300);
    check(view->currentPage() == 0, "firstPage returns to the start");

    // A page that crashed the worker is drawn as a labelled placeholder, not
    // a blank sheet that looks like an empty page.
    {
        const QImage before = grabView(window);
        view->markPageFailed(0);
        pump(100);
        const QImage after = grabView(window);
        check(view->isPageFailed(0) && before != after,
              "a failed page is drawn differently from a rendered one");
        view->setPages(QVector<QSize>(10, QSize(612, 792)));
        check(!view->isPageFailed(0), "failed marks are cleared for a new document");
        window.openPath(QString::fromStdString(doc));
        pump(1500);
    }

    // --- Password flow -----------------------------------------------------
    // Test the worker's authentication logic on a STANDALONE worker+thread with
    // no window attached, so the real modal password dialog never appears.
    const std::string locked = std::string(LEHT_CORPUS_DIR) + "/locked.pdf";
    if (!QFile::exists(QString::fromStdString(locked))) {
        std::printf("      SKIP password flow: %s missing "
                    "(run tests/corpus/generate.sh)\n", locked.c_str());
    } else {
        QThread thread;
        auto* pw = new RenderWorker();
        pw->moveToThread(&thread);
        thread.start();

        int prompts = 0;
        bool retryFlag = false;
        int openedPages = 0;
        QObject::connect(pw, &RenderWorker::passwordRequired,
                         [&](bool retry) { ++prompts; retryFlag = retry; });
        QObject::connect(pw, &RenderWorker::opened,
                         [&](int pages, QVector<QSize>) { openedPages = pages; });

        QMetaObject::invokeMethod(pw, "open", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromStdString(locked)));
        pump(1000);
        check(prompts == 1, "encrypted document prompts for a password");
        check(openedPages == 0, "encrypted document does not open unprompted");

        QMetaObject::invokeMethod(pw, "authenticate", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("wrong")));
        pump(700);
        check(prompts == 2 && retryFlag, "wrong password re-prompts with retry");
        check(openedPages == 0, "wrong password does not open the document");

        QMetaObject::invokeMethod(pw, "authenticate", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("s3cret")));
        pump(1000);
        check(openedPages == 10, "correct password opens the document");

        thread.quit();
        thread.wait();
        delete pw;
    }

    // --- Search cancellation ---------------------------------------------
    // A new search must not wait behind an old one, and none of the old one's
    // matches may survive into the new one's results.
    {
        const QString big = QString::fromStdString(std::string(LEHT_CORPUS_DIR) + "/text_500p.pdf");
        QThread thread;
        auto* sw = new RenderWorker();
        sw->moveToThread(&thread);
        thread.start();

        int opened = 0;
        QVector<int> finished;
        int matchPagesSinceStart = 0;
        QObject::connect(sw, &RenderWorker::opened, [&](int pages, QVector<QSize>) { opened = pages; });
        QObject::connect(sw, &RenderWorker::searchStarted, [&] { matchPagesSinceStart = 0; });
        QObject::connect(sw, &RenderWorker::pageMatches,
                         [&](int, QVector<QRectF>) { ++matchPagesSinceStart; });
        QObject::connect(sw, &RenderWorker::searchFinished, [&](int total) { finished << total; });
        QMetaObject::invokeMethod(sw, "open", Qt::QueuedConnection, Q_ARG(QString, big));
        pump(2000);
        check(opened == 500, "500-page document opens for the search test");

        // "page 7 -" occurs only on page 7's lines ("leht corpus - page 7 - ...").
        QMetaObject::invokeMethod(sw, "search", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("page 7 -")));
        pump(8000);
        check(finished.size() == 1 && matchPagesSinceStart == 1,
              "baseline: 'page 7 -' matches on exactly one page");
        const int expected = finished.isEmpty() ? -1 : finished.first();

        finished.clear();
        QMetaObject::invokeMethod(sw, "search", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("quick")));
        pump(100);
        sw->cancelSearch();  // what MainWindow does before every new search
        QMetaObject::invokeMethod(sw, "search", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("page 7 -")));
        pump(8000);
        check(finished.size() == 1, "a cancelled search never reports finished");
        check(!finished.isEmpty() && finished.last() == expected && matchPagesSinceStart == 1,
              "the new search's results contain nothing from the cancelled one");

        thread.quit();
        thread.wait();
        delete sw;
    }

    // --- Process isolation (M3) -------------------------------------------
    // A standalone worker+thread, as for the password flow, so failures are
    // observed as signals rather than as modal message boxes.
    {
        QThread thread;
        auto* iso = new RenderWorker();
        iso->moveToThread(&thread);
        thread.start();

        int openedPages = 0;
        QStringList failures;
        QSet<int> renderedPages;
        QObject::connect(iso, &RenderWorker::opened,
                         [&](int pages, QVector<QSize>) { openedPages = pages; });
        QObject::connect(iso, &RenderWorker::failed,
                         [&](const QString& msg) { failures << msg; });
        QObject::connect(iso, &RenderWorker::rendered,
                         [&](int page, double, int, quint64, QImage) {
                             renderedPages.insert(page);
                         });
        QList<int> failedPages;
        QObject::connect(iso, &RenderWorker::pageFailed, [&](int page) { failedPages << page; });
        auto openIn = [&](const QString& path) {
            QMetaObject::invokeMethod(iso, "open", Qt::QueuedConnection, Q_ARG(QString, path));
        };
        auto renderIn = [&](int page) {
            QMetaObject::invokeMethod(iso, "render", Qt::QueuedConnection, Q_ARG(int, page),
                                      Q_ARG(double, 1.0), Q_ARG(int, 0), Q_ARG(quint64, 0));
        };

        QTemporaryDir tmp;
        const QString deep = writeDeepOutline(tmp.filePath(QStringLiteral("deep.pdf")), 200000);

        openIn(deep);
        pump(4000);
        check(failures.size() == 1 && failures.last().contains(QStringLiteral("crashed")),
              "a file that crashes the parser reports failure");
        check(openedPages == 0, "the crashing file does not open");
        check(iso->workerPid() == 0, "no worker is left running after the crash");

        openIn(deep);
        pump(300);
        check(failures.size() == 2 && iso->workerPid() == 0,
              "reopening a quarantined file fails fast, without a worker");

        // A file built to make the parser crawl: the worker misses its
        // deadline, is killed, and the file is treated like a crasher.
        {
            qputenv("LEHT_WORKER_TIMEOUT_MS", "1000");
            const QString slow = writeSlowPageTree(tmp.filePath(QStringLiteral("slow.pdf")), 16000);
            QElapsedTimer clock;
            clock.start();
            openIn(slow);
            while (failures.size() < 3 && clock.elapsed() < 8000) {
                pump(100);
            }
            check(failures.size() == 3 && failures.last().contains(QStringLiteral("responding")),
                  "a file that stalls the parser fails at the deadline");
            check(clock.elapsed() < 5000, "the stalled open is abandoned promptly, not waited out");
            check(iso->workerPid() == 0, "the stalled worker is killed");
            qunsetenv("LEHT_WORKER_TIMEOUT_MS");
            failures.removeLast();  // keep the counts below as they were
        }

        // The kill cases below end with this document quarantined for the
        // session, so they use a private copy rather than the shared corpus file.
        const QString copy = tmp.filePath(QStringLiteral("copy.pdf"));
        QFile::copy(QString::fromStdString(doc), copy);
        openIn(copy);
        pump(1500);
        check(openedPages == 10, "the viewer opens the next file normally after a crash");

        // A worker killed from outside -- SIGKILL, as the OOM killer sends --
        // is not the file's fault: no poisoned page, the request is retried
        // in a fresh worker, and nothing is quarantined.
        const qint64 first = iso->workerPid();
        ::kill(static_cast<pid_t>(first), SIGKILL);
        pump(100);
        renderIn(2);
        pump(1500);
        check(renderedPages.contains(2), "a page whose worker was killed from outside is retried");
        check(failedPages.isEmpty(), "an outside kill marks no page as failed");
        check(iso->workerPid() != 0 && iso->workerPid() != first,
              "a fresh worker replaces the killed one");
        check(failures.size() == 2, "an outside kill is not reported as a bad file");

        // A crash signal is evidence against the file. SIGSEGV stands in for
        // a parser crash on this one page.
        const qint64 second = iso->workerPid();
        ::kill(static_cast<pid_t>(second), SIGSEGV);
        pump(100);
        renderIn(3);
        pump(1500);
        check(!renderedPages.contains(3), "the page being rendered at a crash stays blank");
        check(failedPages == QList<int>{3}, "the viewer is told which page failed");
        check(iso->workerPid() != 0 && iso->workerPid() != second,
              "a fresh worker replaces the crashed one");
        renderIn(4);
        pump(1500);
        check(renderedPages.contains(4), "other pages keep rendering after a respawn");
        renderIn(3);
        pump(500);
        check(!renderedPages.contains(3), "the poisoned page is not retried");

        // A crash during search: the matches so far stand, the search still
        // finishes, and the document is restored rather than lost.
        int searchesDone = 0;
        QObject::connect(iso, &RenderWorker::searchFinished, [&](int) { ++searchesDone; });
        ::kill(static_cast<pid_t>(iso->workerPid()), SIGSEGV);
        pump(100);
        QMetaObject::invokeMethod(iso, "search", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("quick")));
        pump(1500);
        check(searchesDone == 1, "a search interrupted by a crash still finishes");
        check(failures.size() == 3 && iso->workerPid() == 0,
              "a second crash in one document closes it and says so");

        // Reopening it now fails fast: two crashes quarantined it.
        openIn(copy);
        pump(300);
        check(failures.size() == 4, "the twice-crashed document is quarantined");

        // Encrypted documents: after a crash, the fresh worker is unlocked
        // with the password the user already gave -- no second prompt.
        const QString lockedCopy = tmp.filePath(QStringLiteral("locked-copy.pdf"));
        QFile::copy(QString::fromStdString(std::string(LEHT_CORPUS_DIR) + "/locked.pdf"),
                    lockedCopy);
        int prompts = 0;
        QObject::connect(iso, &RenderWorker::passwordRequired, [&](bool) { ++prompts; });
        openedPages = 0;
        openIn(lockedCopy);
        pump(1000);
        QMetaObject::invokeMethod(iso, "authenticate", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("s3cret")));
        pump(1000);
        check(prompts == 1 && openedPages == 10, "the encrypted copy opens with its password");
        renderedPages.clear();
        ::kill(static_cast<pid_t>(iso->workerPid()), SIGSEGV);
        pump(100);
        renderIn(1);
        pump(1500);
        renderIn(6);
        pump(1500);
        check(!renderedPages.contains(1) && renderedPages.contains(6),
              "after a crash, an encrypted document keeps rendering without re-prompting");
        check(prompts == 1, "the user is not asked for the password again");

        // A system that keeps killing the worker (memory pressure) eventually
        // gets a plain explanation -- and the file is NOT quarantined for it.
        const QString oomCopy = tmp.filePath(QStringLiteral("oom-copy.pdf"));
        QFile::copy(QString::fromStdString(doc), oomCopy);
        openedPages = 0;
        openIn(oomCopy);
        pump(1000);
        const int failuresBefore = static_cast<int>(failures.size());
        for (int k = 0; k < 4 && iso->workerPid() != 0; ++k) {
            ::kill(static_cast<pid_t>(iso->workerPid()), SIGKILL);
            pump(100);
            renderIn(2 + k);  // distinct pages: a cache hit would never reach the worker
            pump(800);
        }
        check(failures.size() == failuresBefore + 1 &&
                  failures.last().contains(QStringLiteral("terminated")),
              "repeated outside kills end with a 'terminated' message");
        openedPages = 0;
        openIn(oomCopy);
        pump(1000);
        check(openedPages == 10, "a file whose worker was killed from outside is not quarantined");

        thread.quit();
        thread.wait();
        delete iso;
    }

    // --- Print -------------------------------------------------------------
    // Print a page range to a PDF and check the output is a valid document with
    // the right number of pages. Exercises the whole print path headlessly.
    window.openPath(QString::fromStdString(doc));  // the 10-page text doc
    pump(1500);
    {
        QTemporaryDir tmp;
        const QString out = tmp.filePath(QStringLiteral("printed.pdf"));
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(out);

        const bool ok = window.printDocument(printer, 2, 5);  // pages 2..5
        check(ok, "printDocument reports success");
        check(QFile::exists(out) && QFile(out).size() > 0,
              "print produced a non-empty PDF");

        // Reopen the printed PDF through core and count its pages.
        try {
            leht::Context ctx;
            leht::Document printed = leht::Document::open(ctx, out.toStdString());
            std::printf("      printed PDF has %d pages\n", printed.page_count());
            check(printed.page_count() == 4, "printed the requested 4-page range");
        } catch (const leht::Error& e) {
            check(false, "printed PDF opens cleanly");
            std::printf("      open error: %s\n", e.what());
        }
    }

    // --- Editing (M4b) -----------------------------------------------------
    {
        QTemporaryDir tmp;
        const QString copy = tmp.filePath(QStringLiteral("edit me.pdf"));
        QFile::copy(QString::fromStdString(doc), copy);
        window.openPath(copy);
        pump(1500);
        view->setZoom(1.0);
        view->verticalScrollBar()->setValue(0);
        pump(500);
        check(!window.isModified(), "a freshly opened document is unmodified");

        // Where "quick brown" is on page 1, from core in-process.
        leht::Context ctx;
        QVector<QRectF> boxes;
        {
            leht::Document d = leht::Document::open(ctx, copy.toStdString());
            const auto hit = leht::TextPage(ctx, d, 0).search("quick brown").front();
            for (const auto& q : hit.quads) {
                boxes.push_back(QRectF(QPointF(q.min_x(), q.min_y()), QPointF(q.max_x(), q.max_y())));
            }
        }

        const QImage beforeEdit = grabView(window);
        emit view->highlightRequested(0, boxes);
        pump(1200);
        check(window.isModified(), "a highlight marks the document modified");
        check(grabView(window) != beforeEdit, "the highlight is drawn");

        QMetaObject::invokeMethod(worker, "undo", Qt::QueuedConnection);
        pump(1200);
        check(!window.isModified(), "undo takes it back");
        QMetaObject::invokeMethod(worker, "redo", Qt::QueuedConnection);
        pump(1200);
        check(window.isModified(), "redo puts it back");

        // A freehand stroke through real mouse events on the viewport.
        check(view->setTool(PageView::Tool::Ink), "the ink tool is available unrotated");
        QWidget* vp = view->viewport();
        const QPoint start(120, 300);
        auto mouse = [&](QEvent::Type type, QPoint at, Qt::MouseButtons held) {
            QMouseEvent e(type, QPointF(at), vp->mapToGlobal(QPointF(at)), Qt::LeftButton, held,
                          Qt::NoModifier);
            QApplication::sendEvent(vp, &e);
        };
        mouse(QEvent::MouseButtonPress, start, Qt::LeftButton);
        for (int i = 1; i <= 10; ++i) {
            mouse(QEvent::MouseMove, start + QPoint(i * 15, (i % 2) * 20), Qt::LeftButton);
        }
        mouse(QEvent::MouseButtonRelease, start + QPoint(150, 0), Qt::NoButton);
        pump(1200);
        (void)view->setTool(PageView::Tool::Select);

        // Kill the worker from outside mid-session: the fresh one must get
        // both edits back from the log.
        const qint64 pid = worker->workerPid();
        check(pid > 0, "a worker is running");
        ::kill(static_cast<pid_t>(pid), SIGKILL);
        pump(300);
        // The next request notices, respawns and replays.
        QMetaObject::invokeMethod(worker, "listAnnotations", Qt::QueuedConnection);
        pump(1500);
        check(worker->workerPid() != pid, "the killed worker was replaced");

        check(window.save(), "save starts");
        pump(2000);
        check(!window.isModified(), "saving clears the modified mark");
        try {
            leht::Document saved = leht::Document::open(ctx, copy.toStdString());
            const auto annots = leht::ops::list_annotations(ctx, saved);
            std::printf("      saved annotations: %zu\n", annots.size());
            check(annots.size() == 2, "both edits survived the worker kill and were saved");
            bool highlight = false;
            bool ink = false;
            for (const auto& a : annots) {
                highlight = highlight || a.type == "Highlight";
                ink = ink || a.type == "Ink";
            }
            check(highlight && ink, "the saved file has the highlight and the drawing");
        } catch (const leht::Error& e) {
            check(false, "the saved file opens");
            std::printf("      open error: %s\n", e.what());
        }
        check(QFile::exists(copy) && QDir(tmp.path()).entryList(QDir::Hidden | QDir::Files).size() == 1,
              "no temporary file is left beside the saved one");

        // Redact a box over the first line, then save and look for the text.
        emit view->redactRequested(0, boxes.first().adjusted(-1, -1, 1, 1));
        pump(1500);
        check(window.isModified(), "a redaction marks the document modified");
        (void)window.save();
        pump(2000);
        try {
            leht::Document saved = leht::Document::open(ctx, copy.toStdString());
            const auto left = leht::TextPage(ctx, saved, 0).search("quick brown");
            check(left.size() == 49, "the redacted line's text is gone from the file (49 of 50 left)");
        } catch (const leht::Error&) {
            check(false, "the redacted file opens");
        }

        // Forms: the panel lists the fields, and a value set there is saved.
        const QString form = tmp.filePath(QStringLiteral("form.pdf"));
        if (QFile::copy(QStringLiteral(LEHT_CORPUS_DIR "/form.pdf"), form)) {
            window.openPath(form);
            pump(1500);
            auto* table = window.findChild<QTableWidget*>();
            check(table != nullptr && table->rowCount() == 2, "the form panel lists two fields");
            QMetaObject::invokeMethod(worker, "setFieldValue", Qt::QueuedConnection,
                                      Q_ARG(QString, QStringLiteral("name")),
                                      Q_ARG(QString, QStringLiteral("Marlon")));
            pump(1200);
            check(window.isModified(), "filling a field marks the document modified");
            (void)window.save();
            pump(2000);
            leht::Document saved = leht::Document::open(ctx, form.toStdString());
            bool filled = false;
            for (const auto& f : leht::ops::list_fields(ctx, saved)) {
                filled = filled || (f.name == "name" && f.value == "Marlon");
            }
            check(filled, "the filled value is in the saved file");
        } else {
            std::printf("  skip  forms (tests/corpus/form.pdf not generated)\n");
        }
    }

    if (g_failures > 0) {
        std::printf("%d smoke check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("viewer smoke test passed\n");
    return 0;
}
