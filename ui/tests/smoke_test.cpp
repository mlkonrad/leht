// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless smoke test for the viewer's wiring. Not a unit test of core/ (that
// is covered elsewhere) — this proves the GUI path holds together: open a
// document, render on the worker thread, show real content, and that scrolling
// and zooming change what is drawn. Runs under QT_QPA_PLATFORM=offscreen.

#include "main_window.hpp"
#include "page_view.hpp"
#include "actions.hpp"
#include "annotation_properties.hpp"
#include "comments_panel.hpp"
#include "contrast.hpp"
#include <QDoubleSpinBox>
#include <QTreeWidget>
#include "mode_bar.hpp"
#include "page_grid.hpp"
#include "preferences.hpp"
#include "properties_dialog.hpp"
#include "protect_dialog.hpp"
#include <QGroupBox>
#include <QTimeZone>
#include "recent_files.hpp"
#include "sidebar.hpp"
#include "color_swatches.hpp"
#include "first_run_hints.hpp"
#include "form_panel.hpp"
#include "signature_cards.hpp"
#include "welcome_view.hpp"
#include <QAccessible>
#include <QKeyEvent>
#include <QListWidget>
#include <QStackedWidget>
#include <QPointer>
#include <QMenuBar>

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPrinter>
#include <QTemporaryDir>
#include <QImage>
#include <QScrollBar>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <signal.h>
#include <ctime>

#include <QThread>

#include "page_dialogs.hpp"
#include "file_tools.hpp"
#include "file_tools_dialogs.hpp"
#ifdef LEHT_HAVE_OCR
#include "leht/ocr/ocr.hpp"
#endif
#include "leht/ops/merge.hpp"
#include "render_worker.hpp"
#include <QPlainTextEdit>

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/forms.hpp"
#include "leht/text.hpp"
#include "leht/crypto/crypto.hpp"
#include "test_pki.hpp"
#include "leht/trustlist/model.hpp"
#include "softhsm.hpp"
#include "sk.hpp"
#include "sk_mock.hpp"
#include "phone_sign_dialog.hpp"
#include "sign_dialog.hpp"
#include <QComboBox>
#include <QMessageBox>
#include <QRadioButton>

#include <QSettings>
#include <QToolBar>

#include <QDir>
#include <QMouseEvent>
#include <QTableWidget>

#include <QDockWidget>
#include <QPushButton>
#include <QInputDialog>
#include <QLineEdit>
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

QImage grabView(MainWindow& w) { return w.view()->grab().toImage(); }

/// With LEHT_SMOKE_SHOTS=DIR, saves what the test sees as DIR/NAME.png: a way
/// to look at new UI without a display. Does nothing otherwise.
void shot(QWidget* w, const char* name) {
    const QString dir = qEnvironmentVariable("LEHT_SMOKE_SHOTS");
    if (!dir.isEmpty()) {
        w->grab().save(dir + QStringLiteral("/") + QString::fromLatin1(name) +
                       QStringLiteral(".png"));
    }
}

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
    // The first-run tours are tested on their own below; they must not cover
    // the windows every other check drives.
    QSettings().setValue(QLatin1String(FirstRunHints::kWelcomeKey), true);
    QSettings().setValue(QLatin1String(FirstRunHints::kDocumentKey), true);

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

    // A screen reader sees the page view as a document holding the shown
    // page's words, fetched only while one is listening.
    {
        QAccessible::setActive(true);
        view->goToPage(1);
        pump(600);
        view->goToPage(0);
        pump(800);
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(view);
        QAccessibleTextInterface* text = iface != nullptr ? iface->textInterface() : nullptr;
        check(iface != nullptr && iface->role() == QAccessible::Document &&
                  iface->text(QAccessible::Description) == QStringLiteral("Page 1 of 10"),
              "a screen reader hears a document, and which page");
        check(text != nullptr && text->characterCount() > 100 &&
                  text->text(0, text->characterCount()).contains(QStringLiteral("page 1 - line 0")),
              "and can read the page's words");
        if (text != nullptr) {
            int start = 0;
            int end = 0;
            check(text->textAtOffset(0, QAccessible::WordBoundary, &start, &end) == QStringLiteral("leht"),
                  "word by word");
        }
        QAccessible::setActive(false);
    }

    // High contrast: status colours are made readable on any background, and
    // the window still draws with a black-and-white palette.
    {
        const QColor amber(210, 130, 0);
        bool readable = true;
        for (const QColor& bg : {QColor(Qt::white), QColor(Qt::black), QColor(40, 40, 40)}) {
            readable = readable && contrast::ratio(contrast::readableOn(amber, bg), bg) >= 4.5;
        }
        check(readable, "status colours reach 4.5:1 on white, black and dark grey");
        const QPalette saved = QApplication::palette();
        QPalette high(Qt::white, Qt::black);  // window text white on black
        high.setColor(QPalette::Base, Qt::black);
        high.setColor(QPalette::Text, Qt::white);
        high.setColor(QPalette::Button, Qt::black);
        high.setColor(QPalette::ButtonText, Qt::white);
        high.setColor(QPalette::Highlight, QColor(255, 255, 0));
        high.setColor(QPalette::HighlightedText, Qt::black);
        high.setColor(QPalette::AlternateBase, QColor(40, 40, 40));
        high.setColor(QPalette::Mid, QColor(160, 160, 160));
        QApplication::setPalette(high);
        {
            // A theme is chosen before Leht starts: a new window, built in it.
            MainWindow contrasted;
            contrasted.resize(800, 1000);
            contrasted.show();
            contrasted.openPath(QString::fromStdString(doc));
            pump(2000);
            check(inkSamples(grabView(contrasted)) > 200, "pages still render under a high-contrast palette");
            shot(&contrasted, "ux-high-contrast");
        }
        QApplication::setPalette(saved);
        pump(200);
    }

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

    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("outlineTree"));
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

    check(window.sidebar()->isPanelAvailable(QStringLiteral("outline")),
          "the sidebar offers an Outline tab for a document with an outline");

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

    // --- File tools: Combine Files, Reduce File Size, Split Document -----------
    //
    // Each job runs in a sandboxed worker of its own. The window answers every
    // prompt and result box through `answer`, as a user would.
    {
        QTemporaryDir tmp;
        window.openPath(QString::fromStdString(doc));
        pump(1500);

        auto* combineAction = window.findChild<QAction*>(QStringLiteral("combineFiles"));
        auto* reduceAction = window.findChild<QAction*>(QStringLiteral("reduceFileSize"));
        auto* splitAction = window.findChild<QAction*>(QStringLiteral("splitDocument"));
        check(combineAction != nullptr && reduceAction != nullptr && splitAction != nullptr,
              "the Files menu offers Combine, Reduce and Split");
        check(combineAction != nullptr && combineAction->isEnabled() &&
                  reduceAction != nullptr && reduceAction->isEnabled() &&
                  splitAction != nullptr && splitAction->isEnabled(),
              "and all three are enabled with a document open");

        FileTools* tools = window.fileTools();
        QObject listener;  // the test's own connections end with this block
        QStringList written;
        QString failure;
        int passwordAsks = 0;
        bool done = false;
        QObject::connect(tools, &FileTools::finished, &listener,
                         [&](const QString&, const QStringList& files) {
                             written = files;
                             done = true;
                         });
        QObject::connect(tools, &FileTools::failed, &listener, [&](const QString& message) {
            failure = message;
            done = true;
        });
        QObject::connect(tools, &FileTools::passwordRequired, &listener,
                         [&](bool) { ++passwordAsks; });
        const auto waitDone = [&done] {
            QElapsedTimer t;
            t.start();
            while (!done && t.elapsed() < 20000) {
                pump(50);
            }
            pump(100);  // let the window's own handlers finish
            const bool was = done;
            done = false;
            return was;
        };

        // Answers whatever the window asks: fills the tool dialogs in, gives
        // the test document's password, and closes the result box.
        QString reduceTo;
        QString splitInto;
        QTimer answer;
        QObject::connect(&answer, &QTimer::timeout, [&] {
            QWidget* modal = QApplication::activeModalWidget();
            if (auto* reduce = qobject_cast<ReduceDialog*>(modal)) {
                reduce->findChild<QLineEdit*>(QStringLiteral("reduceOutput"))->setText(reduceTo);
                reduce->accept();
            } else if (auto* split = qobject_cast<SplitDialog*>(modal)) {
                split->findChild<QLineEdit*>(QStringLiteral("splitFolder"))->setText(splitInto);
                for (QPushButton* b : split->findChildren<QPushButton*>()) {
                    if (b->text() == QStringLiteral("Split")) {
                        b->click();
                    }
                }
            } else if (auto* ask = qobject_cast<QInputDialog*>(modal)) {
                ask->setTextValue(QStringLiteral("s3cret"));
                ask->accept();
            } else if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                box->accept();
            }
        });
        answer.start(50);

        // Combine: a PDF and two images, in order.
        const QString combined = tmp.filePath(QStringLiteral("combined.pdf"));
        const QString corpusDir = QStringLiteral(LEHT_CORPUS_DIR);
        QMetaObject::invokeMethod(
            tools,
            [tools, combined, corpusDir] {
                tools->combine({corpusDir + QStringLiteral("/text_10p.pdf"),
                                corpusDir + QStringLiteral("/scan.jpg"),
                                corpusDir + QStringLiteral("/page.png")},
                               combined, false);
            },
            Qt::QueuedConnection);
        check(waitDone() && failure.isEmpty() && written == QStringList{combined},
              "Combine Files writes one PDF");
        try {
            leht::Context ctx;
            check(leht::Document::open(ctx, combined.toStdString()).page_count() == 12,
                  "with the PDF's 10 pages and a page per image");
        } catch (const leht::Error& e) {
            check(false, e.what());
        }

        // A file that is neither: refused, and nothing is left behind.
        const QString notDoc = tmp.filePath(QStringLiteral("notes.txt"));
        {
            QFile f(notDoc);
            (void)f.open(QIODevice::WriteOnly);
            f.write("not a document\n");
        }
        const QString refused = tmp.filePath(QStringLiteral("refused.pdf"));
        failure.clear();
        QMetaObject::invokeMethod(
            tools, [tools, notDoc, refused] { tools->combine({notDoc}, refused, false); },
            Qt::QueuedConnection);
        check(waitDone() && failure.contains(QStringLiteral("notes.txt")) &&
                  !QFile::exists(refused) && QDir(tmp.path()).entryList(QDir::Hidden | QDir::Files)
                                                     .filter(QStringLiteral(".leht-"))
                                                     .isEmpty(),
              "a file that is not a PDF or image is refused, leaving nothing behind");

        // The split dialog's arithmetic.
        check(SplitDialog::chunks(10, 4) == QStringList({"1-4", "5-8", "9-10"}) &&
                  SplitDialog::chunks(3, 1) == QStringList({"1", "2", "3"}),
              "Split makes the right page groups");
        check(QFileInfo(SplitDialog::numberedNames(tmp.path(), QStringLiteral("x"), 12).first())
                      .fileName() == QStringLiteral("x-01.pdf"),
              "and numbers the files so they sort");

        // Split, through the window: every page to a file by default.
        splitInto = tmp.filePath(QStringLiteral("parts"));
        QDir().mkpath(splitInto);
        failure.clear();
        splitAction->trigger();
        check(waitDone() && failure.isEmpty() && written.size() == 10,
              "Split Document writes one file per page");
        if (!written.isEmpty()) {
            check(QFileInfo(written.first()).fileName() == QStringLiteral("text_10p-01.pdf"),
                  "named after the document");
            try {
                leht::Context ctx;
                check(leht::Document::open(ctx, written.last().toStdString()).page_count() == 1,
                      "each holding its page");
            } catch (const leht::Error& e) {
                check(false, e.what());
            }
        }

        // Reduce, through the window, on a document of scans.
        const QString heavy = tmp.filePath(QStringLiteral("scans.pdf"));
        {
            leht::Context ctx;
            const std::string scan = (corpusDir + QStringLiteral("/scan.jpg")).toStdString();
            (void)leht::ops::merge(ctx, {scan, scan, scan}, heavy.toStdString());
        }
        window.openPath(heavy);
        pump(1500);
        reduceTo = tmp.filePath(QStringLiteral("scans-smaller.pdf"));
        failure.clear();
        reduceAction->trigger();
        check(waitDone() && failure.isEmpty() && written == QStringList{reduceTo},
              "Reduce File Size writes a copy");
        check(QFile::exists(reduceTo) && QFileInfo(reduceTo).size() < QFileInfo(heavy).size(),
              "and the copy is smaller than the original");

        // The dialog's estimate: every preset reported, nothing written, and
        // the smallest setting smaller than the scans.
        {
            QHash<int, qint64> sizes;
            const auto estimates = QObject::connect(tools, &FileTools::estimated, &window,
                                                    [&sizes](int preset, qint64 bytes) { sizes.insert(preset, bytes); });
            QMetaObject::invokeMethod(tools, [tools, heavy] { tools->estimate(heavy, QString()); },
                                      Qt::QueuedConnection);
            QElapsedTimer waited;
            waited.start();
            while (sizes.size() < 4 && waited.elapsed() < 30000) {
                pump(100);
            }
            QObject::disconnect(estimates);
            check(sizes.size() == 4 && sizes.value(3) > 0 && sizes.value(3) < QFileInfo(heavy).size(),
                  "Reduce File Size estimates each setting before writing anything");
        }

        // A text-only file cannot get smaller at the lossless setting: nothing
        // is written, and the user is told so.
        const QString same = tmp.filePath(QStringLiteral("same.pdf"));
        QMetaObject::invokeMethod(
            tools, [tools, combined, same] { tools->compress(combined, {}, same, 0, 0, false); },
            Qt::QueuedConnection);
        (void)waitDone();
        check(failure.isEmpty() && (written.isEmpty() ? !QFile::exists(same)
                                                      : QFileInfo(same).size() <
                                                            QFileInfo(combined).size()),
              "Reduce never hands back a bigger file");

        // An encrypted document: the window asks for its password, then the
        // job runs again with it.
        const QString lockedSrc = corpusDir + QStringLiteral("/locked.pdf");
        if (QFile::exists(lockedSrc)) {
            window.openPath(lockedSrc);
            pump(1500);
            splitInto = tmp.filePath(QStringLiteral("locked-parts"));
            QDir().mkpath(splitInto);
            passwordAsks = 0;
            failure.clear();
            splitAction->trigger();
            check(waitDone() && failure.isEmpty() && passwordAsks == 1 && !written.isEmpty(),
                  "splitting an encrypted document asks for its password once, then works");
        } else {
            std::printf("  skip  file tools on an encrypted document (no locked.pdf)\n");
        }
        answer.stop();
    }


    // --- Moving, free text, watermark and crop options (M1) ------------------
    {
        QTemporaryDir tmp;
        const QString copy = tmp.filePath(QStringLiteral("move me.pdf"));
        QFile::copy(QString::fromStdString(doc), copy);
        window.openPath(copy);
        pump(1500);
        view->setZoom(1.0);
        view->verticalScrollBar()->setValue(0);
        view->horizontalScrollBar()->setValue(0);
        pump(500);

        QVector<AnnotRow> rows;
        const auto listed = QObject::connect(worker, &RenderWorker::annotationsReady, &window,
                                             [&rows](const QVector<AnnotRow>& r) { rows = r; });
        const auto freeText = [&rows]() -> AnnotRow {
            for (const AnnotRow& r : rows) {
                if (r.type == QStringLiteral("FreeText")) {
                    return r;
                }
            }
            return {};
        };
        QWidget* vp = view->viewport();
        auto mouse = [&](QEvent::Type type, QPoint at, Qt::MouseButtons held) {
            QMouseEvent e(type, QPointF(at), vp->mapToGlobal(QPointF(at)), Qt::LeftButton, held,
                          Qt::NoModifier);
            QApplication::sendEvent(vp, &e);
        };
        auto drag = [&](QPoint from, QPoint to) {
            mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
            for (int i = 1; i <= 8; ++i) {
                mouse(QEvent::MouseMove, from + (to - from) * i / 8, Qt::LeftButton);
            }
            mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
        };
        auto ctrlEnter = [](QWidget* w) {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
            QApplication::sendEvent(w, &press);
        };

        // Text tool: a click opens an editor on the page; Ctrl+Enter writes it.
        check(view->setTool(PageView::Tool::Text), "the text tool is available");
        const QPoint at(160, 120);
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, at, Qt::NoButton);
        QPlainTextEdit* editor = view->textEditor();
        check(editor != nullptr, "a click with the text tool opens an editor");
        if (editor != nullptr) {
            editor->insertPlainText(QStringLiteral("Smoke text"));
            shot(&window, "m1-text-editor");
            ctrlEnter(editor);
        }
        pump(1500);
        check(view->textEditor() == nullptr, "Ctrl+Enter closes the editor");
        AnnotRow ft = freeText();
        check(ft.id > 0 && ft.contents == QStringLiteral("Smoke text"),
              "the typed text is a free-text annotation");
        check(ft.movable && ft.resizable && ft.fontSize == 12, "and it can be moved and resized");

        // Move tool: drag it by its body...
        check(view->setTool(PageView::Tool::Move), "the move tool is available");
        const QRectF before = ft.rect;
        mouse(QEvent::MouseButtonPress, at + QPoint(20, 10), Qt::LeftButton);
        for (int i = 1; i <= 8; ++i) {
            mouse(QEvent::MouseMove, at + QPoint(20, 10) + QPoint(100, 150) * i / 8,
                  Qt::LeftButton);
        }
        shot(&window, "m1-move-dragging");
        mouse(QEvent::MouseButtonRelease, at + QPoint(120, 160), Qt::NoButton);
        pump(1500);
        shot(&window, "m1-move-selected");
        ft = freeText();
        check(view->selectedAnnotation() == ft.id, "a click selects it");
        check(std::abs(ft.rect.left() - (before.left() + 100)) < 2 &&
                  std::abs(ft.rect.top() - (before.top() + 150)) < 2,
              "dragging it moves it by as much");
        // ...and by its bottom-right handle.
        const QPoint corner = at + QPoint(100, 150) +
                              QPoint(static_cast<int>(before.width()),
                                     static_cast<int>(before.height()));
        drag(corner, corner + QPoint(80, 40));
        pump(1500);
        ft = freeText();
        check(std::abs(ft.rect.width() - (before.width() + 80)) < 3 &&
                  std::abs(ft.rect.height() - (before.height() + 40)) < 3,
              "dragging a handle resizes it");

        // Double-click (here: the API it uses) edits the words in place.
        check(view->editFreeText(ft.id) && view->textEditor() != nullptr,
              "free text opens for editing in place");
        if (QPlainTextEdit* e = view->textEditor()) {
            check(e->toPlainText() == QStringLiteral("Smoke text"), "with its current words");
            e->setPlainText(QStringLiteral("Changed words"));
            ctrlEnter(e);
        }
        pump(1500);
        check(freeText().contents == QStringLiteral("Changed words"), "new words are saved");

        // Delete removes the selection; undo brings it back.
        vp->setFocus();
        (void)view->setTool(PageView::Tool::Move);
        const QPoint inside = at + QPoint(130, 170);
        mouse(QEvent::MouseButtonPress, inside, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, inside, Qt::NoButton);
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(view, &del);
        pump(1500);
        check(freeText().id == 0, "Delete removes the selected annotation");
        QMetaObject::invokeMethod(worker, "undo", Qt::QueuedConnection);
        pump(2000);
        QMetaObject::invokeMethod(worker, "listAnnotations", Qt::QueuedConnection);
        pump(800);
        check(freeText().contents == QStringLiteral("Changed words"),
              "undo brings it back, moved, resized and rewritten");
        (void)view->setTool(PageView::Tool::Select);

        // Watermark with options, one page; a crop box and per-edge margins.
        leht::ops::WatermarkOptions mark;
        mark.text = "SMOKEMARK";
        mark.angle = 0;
        mark.font_size = 30;
        mark.opacity = 0.5F;
        mark.color[0] = 0.8F;
        QMetaObject::invokeMethod(worker, [=] { worker->addWatermark(QStringLiteral("2"), mark); },
                                  Qt::QueuedConnection);
        pump(1500);
        const QSizeF page3 = view->pageSizePoints(2);
        QMetaObject::invokeMethod(worker,
                                  [=] { worker->cropBox(QStringLiteral("3"),
                                                        QRectF(50, 60, 300, 400)); },
                                  Qt::QueuedConnection);
        pump(1500);
        check(view->pageSizePoints(2) == QSizeF(300, 400) && page3 != QSizeF(300, 400),
              "a crop box on page 3 makes it that size");
        const QSizeF page4 = view->pageSizePoints(3);
        QMetaObject::invokeMethod(worker,
                                  [=] { worker->cropMargins(QStringLiteral("4"),
                                                            leht::ops::Margins{10, 20, 30, 40}); },
                                  Qt::QueuedConnection);
        pump(1500);
        check(view->pageSizePoints(3) == QSizeF(page4.width() - 40, page4.height() - 60),
              "per-edge margins on page 4 trim each edge");
        check(view->pageSizePoints(0) == view->pageSizePoints(4), "other pages are untouched");

        check(window.save(), "the M1 edits save");
        pump(2000);
        try {
            leht::Context ctx;
            leht::Document saved = leht::Document::open(ctx, copy.toStdString());
            check(leht::TextPage(ctx, saved, 1).search("SMOKEMARK").size() == 1 &&
                      leht::TextPage(ctx, saved, 0).search("SMOKEMARK").empty(),
                  "the watermark is on page 2 only");
            bool rewritten = false;
            for (const auto& a : leht::ops::list_annotations(ctx, saved)) {
                rewritten = rewritten || (a.type == "FreeText" && a.contents == "Changed words");
            }
            check(rewritten, "the saved file has the moved, rewritten free text");
        } catch (const leht::Error&) {
            check(false, "the M1 file opens");
        }

        // The dialogs build, preview, and give back what they show.
        {
            WatermarkDialog wd(&window, view->pageImage(0), view->pageSizePoints(0),
                               view->pageCount());
            check(!wd.options().text.empty() && wd.options().opacity > 0,
                  "the watermark dialog offers a watermark");
            wd.show();
            pump(200);
            shot(&wd, "m1-watermark-dialog");
            CropMarginsDialog cd(&window, view->pageCount());
            cd.show();
            pump(200);
            shot(&cd, "m1-crop-dialog");
            check(cd.margins().left == 36 && cd.margins().bottom == 36 && cd.pages().isEmpty(),
                  "the crop dialog starts at half an inch on every page");
        }
        QObject::disconnect(listed);
    }

    // --- OCR (M2) -----------------------------------------------------------
#ifdef LEHT_HAVE_OCR
    {
        const auto langs = leht::ocr::installed_languages(leht::ocr::default_datadir());
        if (std::find(langs.begin(), langs.end(), "eng") == langs.end()) {
            std::printf("  skip  OCR (no Tesseract English data)\n");
        } else {
            QTemporaryDir tmp;
            const QString scan = tmp.filePath(QStringLiteral("scan.pdf"));
            {
                // A picture of page 1: no text at all until OCR reads it.
                leht::Context ctx;
                leht::Document d = leht::Document::open(ctx, doc);
                const auto bmp = leht::Renderer(ctx, d).render(0, 2.0F);
                const std::string png = tmp.filePath(QStringLiteral("scan.png")).toStdString();
                leht::write_png(ctx, *bmp, png);
                (void)leht::ops::merge(ctx, {png, png}, scan.toStdString());  // two pages
            }
            window.openPath(scan);
            pump(1500);
            {
                OcrDialog dialog(&window, view->pageCount(),
                                 {QStringLiteral("eng"), QStringLiteral("est")});
                check(dialog.pages().isEmpty() && dialog.skipPagesWithText() &&
                          dialog.dpi() >= 150,
                      "the OCR dialog starts at every page without text");
                dialog.show();
                pump(200);
                shot(&dialog, "m2-ocr-dialog");
            }
            int progressCalls = 0;
            int readWords = -1;
            QString ocrError;
            const auto p1 = QObject::connect(worker, &RenderWorker::ocrProgress, &window,
                                             [&](int, int, int) { ++progressCalls; });
            const auto p2 = QObject::connect(
                worker, &RenderWorker::ocrFinished, &window,
                [&](int words, int, bool, const QString& error) {
                    readWords = words;
                    ocrError = error;
                });
            QMetaObject::invokeMethod(
                worker, [=] { worker->recognizeText(QString(), QStringLiteral("eng"), 150, true); },
                Qt::QueuedConnection);
            for (int i = 0; i < 120 && readWords < 0; ++i) {
                pump(500);
            }
            if (!ocrError.isEmpty()) {
                std::printf("      ocr error: %s\n", qPrintable(ocrError));
            }
            check(readWords > 600, "OCR through the viewer reads both scanned pages");
            check(progressCalls >= 3, "and reports its progress");
            check(window.isModified(), "the text layer is an edit");
            // One run, one undo step, however many pages it read.
            QMetaObject::invokeMethod(worker, "undo", Qt::QueuedConnection);
            // Undo reopens the file in the worker: under load that can take a
            // while, so wait for the answer rather than a fixed time.
            for (int i = 0; i < 40 && window.isModified(); ++i) {
                pump(250);
            }
            check(!window.isModified(), "one Undo takes the whole OCR run back");
            QMetaObject::invokeMethod(worker, "redo", Qt::QueuedConnection);
            pump(2500);
            check(window.isModified(), "and one Redo puts it all back");
            check(window.save(), "the OCR'd scan saves");
            pump(2000);
            try {
                leht::Context ctx;
                leht::Document saved = leht::Document::open(ctx, scan.toStdString());
                check(!leht::TextPage(ctx, saved, 0).search("quick brown").empty() &&
                          !leht::TextPage(ctx, saved, 1).search("quick brown").empty(),
                      "the saved scan is searchable on both pages");
            } catch (const leht::Error&) {
                check(false, "the OCR'd scan opens");
            }
            QObject::disconnect(p1);
            QObject::disconnect(p2);
        }
    }
#endif

    // --- The window's structure: welcome, menus, modes, close ---------------
    {
        MainWindow fresh;
        fresh.resize(1000, 800);
        fresh.show();
        pump(200);
        check(fresh.isShowingWelcome(), "a window with no document shows the welcome view");
        check(fresh.findChild<WelcomeView*>() != nullptr &&
                  fresh.findChild<QWidget*>(QStringLiteral("task_sign")) != nullptr,
              "the welcome view offers task cards");
        QStringList menus;
        for (QAction* m : fresh.menuBar()->actions()) {
            menus << m->text().remove(QLatin1Char('&'));
        }
        check(menus == QStringList{QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("View"),
                                   QStringLiteral("Pages"), QStringLiteral("Comment"), QStringLiteral("Sign"),
                                   QStringLiteral("Tools"), QStringLiteral("Help")},
              "the menu bar has File, Edit, View, Pages, Comment, Sign, Tools, Help");
        // Every command is somewhere in the menus, not only on a toolbar.
        QSet<QAction*> inMenus;
        const std::function<void(QMenu*)> collect = [&](QMenu* menu) {
            for (QAction* a : menu->actions()) {
                inMenus.insert(a);
                if (a->menu() != nullptr) {
                    collect(a->menu());
                }
            }
        };
        for (QAction* m : fresh.menuBar()->actions()) {
            collect(m->menu());
        }
        for (const char* id : {"open", "save", "saveAs", "print", "combineFiles", "reduceFileSize",
                               "splitDocument", "undo", "find", "preferences", "zoomIn", "fitWidth",
                               "toolHighlight", "toolSign", "signInvisibly", "addLongTermValidation",
                               "toolRedact", "redactText", "watermark", "cropMargins", "shortcuts"}) {
            QAction* a = fresh.actions()->find(QLatin1String(id));
            check(a != nullptr && inMenus.contains(a), (std::string("the menus hold ") + id).c_str());
        }
        // One key, one command: a shortcut given twice fires neither.
        QHash<QString, QString> keys;
        bool unique = true;
        for (QAction* a : fresh.findChildren<QAction*>()) {
            for (const QKeySequence& k : a->shortcuts()) {
                const QString key = k.toString();
                if (keys.contains(key) && keys.value(key) != a->objectName()) {
                    std::printf("      shortcut %s on both %s and %s\n", qPrintable(key),
                                qPrintable(keys.value(key)), qPrintable(a->objectName()));
                    unique = false;
                }
                keys.insert(key, a->objectName());
            }
        }
        check(unique, "no shortcut is given to two commands");
        check(!fresh.actions()->find(QStringLiteral("save"))->isEnabled() &&
                  !fresh.actions()->find(QStringLiteral("toolHighlight"))->isEnabled() &&
                  fresh.actions()->find(QStringLiteral("combineFiles"))->isEnabled(),
              "with nothing open, only what needs no document is enabled");
        {
            PreferencesDialog prefs(&fresh, {QStringLiteral("est"), QStringLiteral("eng")});
            prefs.show();
            pump(50);
            check(prefs.findChild<QListWidget*>(QStringLiteral("preferenceSections"))->count() == 5,
                  "Preferences has five sections");
            shot(&prefs, "ux-preferences");
        }
        shot(&fresh, "ux-welcome");

        fresh.handleDroppedFiles({QString::fromStdString(doc)});
        pump(1500);
        check(!fresh.isShowingWelcome() && fresh.view()->pageCount() == 10,
              "a file dropped on the window opens");
        check(recent::files().value(0) == QFileInfo(QString::fromStdString(doc)).absoluteFilePath(),
              "an opened file heads the recent list");
        {
            const auto windows = [] {
                int n = 0;
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    n += qobject_cast<MainWindow*>(w) != nullptr && w->isVisible() ? 1 : 0;
                }
                return n;
            };
            const int before = windows();
            fresh.openDocument(QStringLiteral(LEHT_CORPUS_DIR "/outlined.pdf"));
            pump(1500);
            check(windows() == before + 1 && fresh.view()->pageCount() == 10,
                  "another file opens in a window of its own, leaving this one as it was");
            fresh.openDocument(QStringLiteral(LEHT_CORPUS_DIR "/outlined.pdf"));
            pump(300);
            check(windows() == before + 1, "opening it again brings that window forward instead");
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* other = qobject_cast<MainWindow*>(w);
                if (other != nullptr && other != &fresh && other != &window && other->isVisible()) {
                    other->close();  // unmodified: closes without asking
                }
            }
            pump(300);
            check(windows() == before, "and it closes");
        }
        check(fresh.actions()->find(QStringLiteral("save"))->isEnabled() &&
                  fresh.actions()->find(QStringLiteral("toolHighlight"))->isEnabled(),
              "an open document enables the tools");

        // Modes: the tool row follows the mode, and a tool picked from a
        // menu brings its mode along.
        ModeBar* modes = fresh.modeBar();
        check(modes->mode() == QStringLiteral("read"), "a document opens in Read mode");
        auto* modeTools = fresh.findChild<QToolBar*>(QStringLiteral("modeTools"));
        modes->setMode(QStringLiteral("comment"));
        pump(50);
        check(modeTools->actions().contains(fresh.actions()->find(QStringLiteral("toolHighlight"))),
              "Comment mode shows the highlighter");
        fresh.actions()->find(QStringLiteral("toolRedact"))->trigger();
        pump(50);
        check(modes->mode() == QStringLiteral("redact"), "picking Mark for Redaction switches to Redact mode");
        modes->setMode(QStringLiteral("read"));
        pump(50);
        check(fresh.actions()->find(QStringLiteral("toolSelect"))->isChecked(),
              "leaving a mode puts its tool down");
        // Colours: the swatch strip follows the comment tool, and remembers.
        {
            QSettings().remove(QStringLiteral("toolColors"));
            check(fresh.toolColor(QStringLiteral("toolHighlight")) == QColor(255, 220, 0),
                  "the highlighter starts yellow");
            modes->setMode(QStringLiteral("comment"));
            fresh.actions()->find(QStringLiteral("toolHighlight"))->trigger();
            pump(50);
            auto* swatches = fresh.findChild<ColorSwatches*>();
            check(swatches != nullptr && swatches->isVisible() && swatches->isEnabled(),
                  "Comment mode shows the colour swatches");
            shot(&fresh, "ux-comment-colour");
            emit swatches->colorChosen(QColor(120, 220, 120));
            check(fresh.toolColor(QStringLiteral("toolHighlight")) == QColor(120, 220, 120) &&
                      fresh.toolColor(QStringLiteral("toolDraw")) == QColor(20, 90, 200),
                  "a colour chosen for the highlighter is its own");
            fresh.actions()->find(QStringLiteral("toolMove"))->trigger();
            pump(50);
            check(!swatches->isEnabled(), "and the swatches rest for a tool without colour");
            QSettings().remove(QStringLiteral("toolColors"));
            modes->setMode(QStringLiteral("read"));
            pump(50);
        }

        // The first-run tour: shows once, and Escape ends it.
        {
            const QString key = QStringLiteral("test/tourShown");
            QSettings().remove(key);
            auto* tour = FirstRunHints::showOnce(&fresh, {{modes, QStringLiteral("One"), QStringLiteral("…")},
                                                          {fresh.sidebar(), QStringLiteral("Two"), QStringLiteral("…")}},
                                                 key);
            check(tour != nullptr && tour->isVisible(), "the first-run tour shows");
            pump(50);
            shot(&fresh, "ux-tour");
            QPointer<FirstRunHints> alive(tour);
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(tour, &esc);
            pump(100);
            check(alive.isNull(), "Escape ends it");
            check(FirstRunHints::showOnce(&fresh, {{modes, QStringLiteral("One"), QStringLiteral("…")}}, key) == nullptr,
                  "and it never shows again");
            QSettings().remove(key);
        }

        fresh.sidebar()->showPanel(QStringLiteral("comments"));
        pump(100);
        check(fresh.sidebar()->currentPanel() == QStringLiteral("comments"), "the sidebar opens a tab");
        shot(&fresh, "ux-document");

        fresh.closeDocument();
        pump(200);
        check(fresh.isShowingWelcome(), "Close returns to the welcome view");
        check(!fresh.actions()->find(QStringLiteral("save"))->isEnabled(), "and disables Save again");
    }

    // --- Pages mode: turn, delete, move, insert (protocol 11) ----------------
    {
        QTemporaryDir tmp;
        const QString copy = tmp.filePath(QStringLiteral("organise.pdf"));
        QFile::copy(QString::fromStdString(doc), copy);
        window.openPath(copy);
        pump(1500);
        window.modeBar()->setMode(QStringLiteral("pages"));
        pump(300);
        auto* grid = window.findChild<PageGrid*>();
        check(grid != nullptr && grid->isVisible() && grid->count() == 10,
              "Pages mode shows a grid of the 10 pages");
        check(window.findChild<Sidebar*>()->width() < 80 && grid->x() + grid->parentWidget()->x() < 100,
              "the sidebar folds to its rail beside the grid, leaving no empty column");
        shot(&window, "ux-page-grid");
        const auto act = [&](const char* id) { window.actions()->find(QLatin1String(id))->trigger(); };

        emit grid->moveRequested({9}, 0);  // the last page to the front, as a drag would
        pump(1500);
        check(window.isModified() && grid->selectedPages() == QVector<int>{0},
              "a page dragged to the front moves, and stays selected");
        grid->selectPages({1});
        act("pageDelete");
        pump(1500);
        check(view->pageCount() == 9 && grid->count() == 9, "Delete Pages removes the selected page");
        grid->selectPages({0});
        act("pageRotateRight");
        pump(1500);
        check(view->pageSizePoints(0).width() > view->pageSizePoints(0).height(),
              "Rotate Right turns the selected page sideways");
        emit grid->filesDropped({QStringLiteral(LEHT_CORPUS_DIR "/outlined.pdf")}, 9);
        pump(2000);
        check(view->pageCount() == 12 && grid->count() == 12, "a PDF dropped on the grid inserts its pages");
        grid->selectPages({11});
        act("pageInsertBlank");
        pump(1500);
        check(view->pageCount() == 13, "Insert Blank Page adds a page");
        act("undo");
        pump(2000);
        check(view->pageCount() == 12 && grid->count() == 12, "undo takes the blank page back out");

        check(window.save(), "the organised document saves");
        pump(2500);
        try {
            leht::Context ctx;
            leht::Document saved = leht::Document::open(ctx, copy.toStdString());
            check(saved.page_count() == 12, "the saved file has 12 pages");
            const auto has = [&](int page, const char* what) {
                return leht::TextPage(ctx, saved, page).text().find(what) != std::string::npos;
            };
            check(has(0, "page 10 -") && has(1, "page 2 -") && has(8, "page 9 -"),
                  "the pages are saved in their new order");
        } catch (const leht::Error&) {
            check(false, "the organised file opens");
        }
        window.modeBar()->setMode(QStringLiteral("read"));
        pump(200);
        check(!grid->isVisible(), "leaving Pages mode shows the pages again");
    }

    // --- Password Protect and Remove Password -----------------------------
    {
        ProtectDialog dialog(&window);
        QPushButton* ok = nullptr;
        for (QPushButton* b : dialog.findChildren<QPushButton*>()) {
            ok = b->text() == QStringLiteral("Protect…") ? b : ok;
        }
        const auto field = [&](const char* name) { return dialog.findChild<QLineEdit*>(QLatin1String(name)); };
        check(ok != nullptr && !ok->isEnabled(), "Protect waits for a password");
        field("openPassword")->setText(QStringLiteral("salajane"));
        check(!ok->isEnabled(), "and for it to be typed twice");
        field("repeatPassword")->setText(QStringLiteral("salajane"));
        check(ok->isEnabled(), "then it may go ahead");
        dialog.findChild<QGroupBox*>(QStringLiteral("restrict"))->setChecked(true);
        check(!ok->isEnabled(), "restrictions need a permissions password of their own");
        field("permissionsPassword")->setText(QStringLiteral("salajane"));
        check(!ok->isEnabled(), "one that differs from the password to open");
        field("permissionsPassword")->setText(QStringLiteral("omanik"));
        check(ok->isEnabled() && (dialog.permissions() & 1) != 0 && (dialog.permissions() & 4) == 0,
              "by default printing stays allowed and copying does not");

        // The job, on a FileTools of the test's own (the window's would show
        // its result in a modal box).
        QTemporaryDir tmp;
        const QString locked = tmp.filePath(QStringLiteral("locked copy.pdf"));
        const QString plain = tmp.filePath(QStringLiteral("plain copy.pdf"));
        QThread thread;
        auto* tools = new FileTools();
        tools->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, tools, &QObject::deleteLater);
        thread.start();
        bool done = false;
        QString failure;
        QObject::connect(tools, &FileTools::finished, &window, [&](const QString&, const QStringList&) { done = true; });
        QObject::connect(tools, &FileTools::failed, &window, [&](const QString& why) {
            failure = why;
            done = true;
        });
        const auto run = [&](auto job) {
            done = false;
            failure.clear();
            QMetaObject::invokeMethod(tools, [tools, job] { job(tools); }, Qt::QueuedConnection);
            QElapsedTimer t;
            t.start();
            while (!done && t.elapsed() < 20000) {
                pump(50);
            }
            return done && failure.isEmpty();
        };
        const QString source = QString::fromStdString(doc);
        check(run([=](FileTools* t) { t->protect(source, {}, locked, true, QStringLiteral("salajane"), {}, 2, 0x7F); }),
              "Password Protect writes a copy");
        check(run([=](FileTools* t) { t->protect(locked, QStringLiteral("salajane"), plain, false, {}, {}, 2, 0x7F); }),
              "Remove Password writes it back without one");
        thread.quit();
        thread.wait();
        leht::Context ctx;
        try {
            leht::Document a = leht::Document::open(ctx, locked.toStdString());
            check(a.needs_password() && a.authenticate("salajane"), "the copy opens only with its password");
            leht::Document b = leht::Document::open(ctx, plain.toStdString());
            check(!b.needs_password() && b.page_count() == 10, "and the unprotected one without");
        } catch (const leht::Error&) {
            check(false, "the protected files open");
        }
    }

    // --- Document Properties ------------------------------------------------
    {
        check(parsePdfDate(QStringLiteral("D:20260928143000+03'00'")) ==
                  QDateTime(QDate(2026, 9, 28), QTime(11, 30), QTimeZone::UTC),
              "a PDF date with a zone reads right");
        check(parsePdfDate(QStringLiteral("D:2026")).date() == QDate(2026, 1, 1) &&
                  !parsePdfDate(QStringLiteral("yesterday")).isValid(),
              "a bare year reads, and nonsense does not");
        check(describePageSize(QSizeF(595, 842)) == QStringLiteral("A4, 210 × 297 mm") &&
                  describePageSize(QSizeF(842, 595)).startsWith(QStringLiteral("A4 landscape")),
              "page sizes have their names");

        QTemporaryDir tmp;
        const QString copy = tmp.filePath(QStringLiteral("described.pdf"));
        QFile::copy(QString::fromStdString(doc), copy);
        window.openPath(copy);
        pump(1500);
        QStringList keys;
        QStringList values;
        const auto got = QObject::connect(worker, &RenderWorker::infoReady, &window,
                                          [&](const QStringList& k, const QStringList& v) {
                                              keys = k;
                                              values = v;
                                          });
        QMetaObject::invokeMethod(worker, "requestInfo", Qt::QueuedConnection);
        pump(600);
        check(keys.contains(QStringLiteral("format")), "the worker describes the document");
        QMetaObject::invokeMethod(worker, "setInfo", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("Title")),
                                  Q_ARG(QString, QStringLiteral("Üürileping")));
        pump(800);
        check(window.isModified(), "setting the title is an edit");
        QMetaObject::invokeMethod(worker, "requestInfo", Qt::QueuedConnection);
        pump(600);
        check(values.value(keys.indexOf(QStringLiteral("info:Title"))) == QStringLiteral("Üürileping"),
              "and the worker reports the new title");
        QObject::disconnect(got);
        (void)window.save();
        pump(2000);
        leht::Context ctx;
        leht::Document saved = leht::Document::open(ctx, copy.toStdString());
        check(saved.metadata("info:Title").value_or("") == "Üürileping", "the title is saved");

        // Export: pages as pictures, and the text.
        const QStringList pictures =
            window.exportImages(QStringLiteral("1-2"), 72, tmp.filePath(QStringLiteral("page %1.png")));
        check(pictures.size() == 2 && QFile::exists(tmp.filePath(QStringLiteral("page 002.png"))),
              "Export Pages as Images writes a picture per page");
        const QImage picture(pictures.value(0));
        check(!picture.isNull() && std::abs(picture.width() - qRound(view->pageSizePoints(0).width())) <= 1,
              "at 72 dpi a page is as many pixels wide as it is points");
        const QString textFile = tmp.filePath(QStringLiteral("text.txt"));
        window.exportText(QStringLiteral("1-2"), textFile);
        pump(1000);
        QFile exported(textFile);
        const QString text = exported.open(QIODevice::ReadOnly) ? QString::fromUtf8(exported.readAll()) : QString();
        check(text.contains(QStringLiteral("page 1 -")) && text.contains(QStringLiteral("page 2 -")) &&
                  text.count(QLatin1Char('\f')) == 1,
              "Export Text writes both pages, a page break between them");
    }

    // --- Signatures in words ---------------------------------------------------
    {
        SigRow ok;
        ok.rangeOk = ok.intact = true;
        ok.trust = static_cast<int>(leht::crypto::Trust::Trusted);
        check(judgeSignature(ok).level == SignatureVerdict::Level::Valid, "an intact, trusted signature is Valid");
        SigRow later = ok;
        later.changedAfterSigning = true;
        check(judgeSignature(later).level == SignatureVerdict::Level::Warning,
              "one the document was added to after is a warning");
        later.onlyValidationDataAfter = true;
        check(judgeSignature(later).level == SignatureVerdict::Level::Valid,
              "unless all that came after was validation data");
        SigRow broken = ok;
        broken.intact = false;
        check(judgeSignature(broken).level == SignatureVerdict::Level::Broken, "a broken one is Broken");
        SigRow stranger = ok;
        stranger.trust = static_cast<int>(leht::crypto::Trust::Untrusted);
        check(judgeSignature(stranger).headline == QStringLiteral("Signer not trusted"),
              "an unknown signer is said to be not trusted");
    }

    // --- The Sign dialog, in steps -------------------------------------------
    {
        FieldRow field;
        field.name = QStringLiteral("Buyer");
        field.type = static_cast<int>(leht::ops::FieldType::Signature);
        field.page = 1;
        field.rect = QRectF(300, 700, 180, 50);
        SignDialog dialog(&window, 0, QRectF(72, 600, 200, 60), QString(), true, {field});
        dialog.show();
        pump(50);
        auto* steps = dialog.findChild<QStackedWidget*>(QStringLiteral("signSteps"));
        auto* next = dialog.findChild<QPushButton*>(QStringLiteral("nextButton"));
        auto* back = dialog.findChild<QPushButton*>(QStringLiteral("backButton"));
        auto* sign = dialog.findChild<QPushButton*>(QStringLiteral("signButton"));
        check(steps != nullptr && steps->count() == 4 && steps->currentIndex() == 0,
              "signing is four steps, starting with where the signature goes");
        check(next != nullptr && next->isVisible() && sign != nullptr && !sign->isVisible() &&
                  back != nullptr && !back->isVisible(),
              "the first step offers Next, not Sign");
        const auto radio = [&dialog](const QString& text) -> QRadioButton* {
            for (auto* b : dialog.findChildren<QRadioButton*>()) {
                if (b->text().startsWith(text)) {
                    return b;
                }
            }
            return nullptr;
        };
        check(radio(QStringLiteral("In the box you drew on page 1")) != nullptr &&
                  radio(QStringLiteral("In the box you drew"))->isChecked() &&
                  radio(QStringLiteral("In a signature field")) != nullptr &&
                  radio(QStringLiteral("In a box I draw")) == nullptr,
              "Where starts on the box drawn, and offers the form's empty signature field");
        next->click();
        pump(50);
        check(steps->currentIndex() == 1, "Next goes on to the key");
        if (auto* file = radio(QStringLiteral("Key file"))) {
            file->setChecked(true);
        }
        for (auto* e : dialog.findChildren<QLineEdit*>()) {
            if (e->placeholderText() == QStringLiteral("a .p12 or .pfx file")) {
                e->setText(QStringLiteral("/tmp/some.p12"));
            }
        }
        next->click();
        pump(50);
        check(steps->currentIndex() == 2 && dialog.findChild<SignaturePreview*>() != nullptr &&
                  dialog.findChild<SignaturePreview*>()->isVisible(),
              "then how it looks, with a preview");
        next->click();
        pump(50);
        check(steps->currentIndex() == 3 && sign->isVisible() && !next->isVisible() && back->isVisible(),
              "the last step signs");
        back->click();
        pump(50);
        check(steps->currentIndex() == 2, "and Back goes back");
        shot(&dialog, "ux-sign-wizard");
        SignSpec drawn = dialog.spec();
        check(drawn.field.isEmpty() && drawn.rect == QRectF(72, 600, 200, 60) && drawn.page == 0,
              "the box drawn is where it signs");

        // The field instead: its page and box, and its name for the engine.
        back->click();
        back->click();
        pump(50);
        radio(QStringLiteral("In a signature field"))->setChecked(true);
        const SignSpec inField = dialog.spec();
        check(inField.field == QStringLiteral("Buyer") && inField.page == 1 &&
                  inField.rect == QRectF(300, 700, 180, 50),
              "choosing the empty field signs in it");

        // Invisible: Look is skipped both ways.
        radio(QStringLiteral("Invisible"))->setChecked(true);
        next->click();
        pump(50);
        next->click();
        pump(50);
        check(steps->currentIndex() == 3 && dialog.spec().rect.isEmpty(),
              "an invisible signature skips the look");
        back->click();
        pump(50);
        check(steps->currentIndex() == 1, "and so does Back");
        dialog.reject();

        // From the menu, with no box yet: drawing one is offered, and closes
        // the dialog so it can be drawn.
        SignDialog fromMenu(&window, 0, QRectF(), QString());
        fromMenu.show();
        pump(50);
        QRadioButton* newBox = nullptr;
        for (auto* b : fromMenu.findChildren<QRadioButton*>()) {
            if (b->text().startsWith(QStringLiteral("In a box I draw"))) {
                newBox = b;
            }
        }
        check(newBox != nullptr, "from the menu, drawing a box is offered");
        if (newBox != nullptr) {
            newBox->setChecked(true);
            fromMenu.findChild<QPushButton*>(QStringLiteral("nextButton"))->click();
            pump(50);
        }
        check(fromMenu.result() == SignDialog::PlaceBox, "and choosing it hands back to the page");
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
        QSettings().setValue(QLatin1String(prefs::kAuthor), QStringLiteral("Mari Maasikas"));
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
        QSettings().remove(QLatin1String(prefs::kAuthor));
        check(window.findChild<CommentsPanel*>() != nullptr && window.findChild<CommentsPanel*>()->count() == 2,
              "the Comments tab lists the highlight and the drawing");
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
            check(std::all_of(annots.begin(), annots.end(),
                              [](const auto& a) { return a.author == "Mari Maasikas"; }),
                  "comments carry the name from Preferences");
        } catch (const leht::Error& e) {
            check(false, "the saved file opens");
            std::printf("      open error: %s\n", e.what());
        }
        check(QFile::exists(copy) && QDir(tmp.path()).entryList(QDir::Hidden | QDir::Files).size() == 1,
              "no temporary file is left beside the saved one");

        // The Comments tab's properties: the drawing gets a wider line.
        {
            window.findChild<Sidebar*>()->showPanel(QStringLiteral("comments"));
            pump(50);
            auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("commentsTree"));
            auto* props = window.findChild<AnnotationProperties*>();
            auto* width = props != nullptr ? props->findChild<QDoubleSpinBox*>(QStringLiteral("propertyLineWidth"))
                                           : nullptr;
            const auto found = tree != nullptr ? tree->findItems(QStringLiteral("Drawing"),
                                                                 Qt::MatchExactly | Qt::MatchRecursive)
                                               : QList<QTreeWidgetItem*>{};
            check(props != nullptr && props->annotationId() == 0 && width != nullptr &&
                      !width->isVisibleTo(props),
                  "with no comment chosen, the properties offer nothing to change");
            check(found.size() == 1, "the drawing is listed by what it is");
            if (props != nullptr && width != nullptr && found.size() == 1) {
                tree->setCurrentItem(found.first());
                pump(50);
                const int id = props->annotationId();
                check(id != 0 && width->isVisibleTo(props) && width->isEnabled() &&
                          std::abs(width->value() - 1.5) < 0.01,
                      "choosing the drawing shows its line width");
                shot(&window, "ux-comment-properties");
                width->setValue(4);
                pump(1200);
                check(window.isModified(), "a new line width is an edit");
                check(props->annotationId() == id && std::abs(width->value() - 4) < 0.01,
                      "the drawing stays chosen when the list comes back");
                QMetaObject::invokeMethod(worker, "undo", Qt::QueuedConnection);
                pump(1200);
                check(!window.isModified() && std::abs(width->value() - 1.5) < 0.01,
                      "undo puts the old width back, in the panel too");
                QMetaObject::invokeMethod(worker, "redo", Qt::QueuedConnection);
                pump(1200);
                check(window.save(), "save starts");
                pump(2000);
                leht::Document saved = leht::Document::open(ctx, copy.toStdString());
                bool wide = false;
                for (const auto& a : leht::ops::list_annotations(ctx, saved)) {
                    wide = wide || (a.type == "Ink" && std::abs(a.line_width - 4) < 0.01);
                }
                check(wide, "the saved drawing has the new width");
            }
        }

        // Redact a box over the first line, then save and look for the text.
        emit view->redactRequested(0, boxes.first().adjusted(-1, -1, 1, 1));
        pump(300);
        check(window.pendingRedactions() == 1 && !window.isModified(),
              "the Redact tool marks an area and removes nothing yet");
        check(window.actions()->find(QStringLiteral("applyRedactions"))->isEnabled(),
              "Apply Redactions is offered once something is marked");
        shot(&window, "ux-redaction-mark");
        check(window.applyRedactions(/*confirm=*/false), "the marks are applied");
        pump(1500);
        check(window.isModified() && window.pendingRedactions() == 0,
              "applying marks the document modified and clears the marks");
        (void)window.save();
        pump(2000);
        try {
            leht::Document saved = leht::Document::open(ctx, copy.toStdString());
            const auto left = leht::TextPage(ctx, saved, 0).search("quick brown");
            check(left.size() == 49, "the redacted line's text is gone from the file (49 of 50 left)");
        } catch (const leht::Error&) {
            check(false, "the redacted file opens");
        }

        // Underline, strike-out and a stamp, then saved: the next file opens without a prompt.
        {
            leht::Document d = leht::Document::open(ctx, copy.toStdString());
            const auto hit = leht::TextPage(ctx, d, 0).search("lazy dog").front();
            QVector<QRectF> dog;
            for (const auto& q : hit.quads) {
                dog.push_back(QRectF(QPointF(q.min_x(), q.min_y()), QPointF(q.max_x(), q.max_y())));
            }
            emit view->markupRequested(0, dog, /*strikeOut=*/true);
            emit view->markupRequested(0, dog, /*strikeOut=*/false);
        }
        QMetaObject::invokeMethod(worker, "addStamp", Qt::QueuedConnection, Q_ARG(int, 1),
                                  Q_ARG(QRectF, QRectF(100, 100, 180, 50)),
                                  Q_ARG(QString, QStringLiteral("Approved")));
        pump(2000);
        (void)window.save();
        pump(2000);
        try {
            leht::Document marked = leht::Document::open(ctx, copy.toStdString());
            QStringList kinds;
            for (const auto& a : leht::ops::list_annotations(ctx, marked)) {
                kinds << QString::fromStdString(a.type);
            }
            check(kinds.contains(QStringLiteral("StrikeOut")) && kinds.contains(QStringLiteral("Underline")) &&
                      kinds.contains(QStringLiteral("Stamp")),
                  "underline, strike-out and stamp are saved");
        } catch (const leht::Error&) {
            check(false, "the marked-up file opens");
        }

        // Forms: the panel lists the fields, and a value set there is saved.
        const QString form = tmp.filePath(QStringLiteral("form.pdf"));
        if (QFile::copy(QStringLiteral(LEHT_CORPUS_DIR "/form.pdf"), form)) {
            window.openPath(form);
            pump(1500);
            auto* formPanel = window.findChild<FormPanel*>(QStringLiteral("formPanel"));
            check(formPanel != nullptr && formPanel->count() == 2, "the form panel lists two fields");
            auto* view = window.findChild<PageView*>();
            check(view != nullptr && view->fieldsShown(), "the fields are outlined on the page");
            QLineEdit* nameEdit = nullptr;
            if (formPanel != nullptr) {
                for (QLineEdit* e : formPanel->findChildren<QLineEdit*>()) {
                    if (e->accessibleName() == QStringLiteral("Name")) {
                        nameEdit = e;
                    }
                }
            }
            check(nameEdit != nullptr && nameEdit->maxLength() == 20,
                  "the name field is a text box that keeps to its 20 characters");
            if (nameEdit != nullptr) {
                formPanel->focusField(QStringLiteral("name"));
                pump(50);
                check(view != nullptr && view->currentField() == QStringLiteral("name"),
                      "the field being filled in is framed on the page");
                nameEdit->setText(QStringLiteral("Marlon"));
                emit nameEdit->editingFinished();
            }
            pump(1200);
            check(window.isModified(), "filling a field marks the document modified");
            check(nameEdit != nullptr && nameEdit->text() == QStringLiteral("Marlon"),
                  "the worker's new list leaves the typed value in place");
            shot(&window, "ux-form-panel");
            (void)window.save();
            pump(2000);
            leht::Document saved = leht::Document::open(ctx, form.toStdString());
            bool filled = false;
            for (const auto& f : leht::ops::list_fields(ctx, saved)) {
                filled = filled || (f.name == "name" && f.value == "Marlon");
            }
            check(filled, "the filled value is in the saved file");

            // Flatten: one confirmation, no fields left, and Undo brings them back.
            auto* flatten = window.findChild<QAction*>(QStringLiteral("flattenForm"));
            check(flatten != nullptr && flatten->isEnabled(), "Flatten Form is offered for a form");
            QTimer yes;
            QObject::connect(&yes, &QTimer::timeout, [] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    if (QAbstractButton* b = box->button(QMessageBox::Yes)) {
                        b->click();
                    } else {
                        box->accept();
                    }
                }
            });
            yes.start(50);
            if (flatten != nullptr) {
                flatten->trigger();
            }
            pump(1500);
            yes.stop();
            check(formPanel != nullptr && formPanel->count() == 0 && flatten != nullptr && !flatten->isEnabled(),
                  "flattening leaves no fields to fill");
            auto* undo = window.findChild<QAction*>(QStringLiteral("undo"));
            if (undo != nullptr) {
                undo->trigger();
            }
            pump(1500);
            check(formPanel != nullptr && formPanel->count() == 2, "Undo brings the fields back");
            (void)window.save();  // whatever Undo left counts as saved: the next open asks nothing
            pump(1500);
        } else {
            std::printf("  skip  forms (tests/corpus/form.pdf not generated)\n");
        }

        // Signing, the whole way through the viewer: the worker leaves a hole,
        // this process fills it, and the panel reports what the worker made of
        // the result. The dialog is skipped -- it only fills in a SignSpec.
        const QString toSign = tmp.filePath(QStringLiteral("to_sign.pdf"));
        QFile::remove(toSign);
        if (QFile::copy(QStringLiteral(LEHT_CORPUS_DIR "/text_10p.pdf"), toSign)) {
            static const leht::test::Pki pki;
            const QString p12 = tmp.filePath(QStringLiteral("signer.p12"));
            const QString ca = tmp.filePath(QStringLiteral("ca.pem"));
            {
                const auto bytes = leht::test::pkcs12(pki.rsa, pki.rsa_cert, {&pki.ca}, "pw");
                QFile f(p12);
                check(f.open(QIODevice::WriteOnly), "the test key file opens for writing");
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<qint64>(bytes.size()));
                QFile c(ca);
                check(c.open(QIODevice::WriteOnly), "the test CA file opens for writing");
                c.write(pki.ca.pem().c_str());
            }
            // Trust our test CA, and nothing else the user may have added.
            QSettings settings;
            settings.setValue(QStringLiteral("trustedCertificates"), QStringList{ca});

            window.openPath(toSign);
            pump(1500);
            QVector<SigRow> reported;
            // Disconnected before `reported` goes: a connection left behind
            // writes the next document's list into a dead QVector, and that
            // heap corruption surfaced far later, as a crash in unrelated code.
            const auto reportedConnection =
                QObject::connect(worker, &RenderWorker::signaturesReady, &window,
                                 [&reported](const QVector<SigRow>& rows) { reported = rows; });

            SignSpec spec;
            spec.p12Path = p12;
            spec.password = QStringLiteral("pw");
            spec.page = 0;
            spec.rect = QRectF(300, 650, 240, 80);
            spec.name = QStringLiteral("Mari Maasikas");
            spec.reason = QStringLiteral("Smoke test");
            spec.strokes = {QPolygonF({QPointF(0, 30), QPointF(30, 5), QPointF(60, 35)})};
            spec.strokesCanvas = QSizeF(80, 40);
            spec.lines = QStringList{QStringLiteral("Mari Maasikas")};
            QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                      Q_ARG(QString, toSign), Q_ARG(SignSpec, spec));
            pump(3000);

            check(!reported.isEmpty(), "the signature panel was told about a signature");
            if (!reported.isEmpty()) {
                const SigRow& row = reported.first();
                check(row.intact, "the viewer's signature verifies");
                check(row.trust == static_cast<int>(leht::crypto::Trust::Trusted),
                      "the added certificate makes the signer trusted");
                check(row.signerCommonName == QStringLiteral("Mari Maasikas"),
                      "the panel names the signer");
                check(!row.changedAfterSigning, "nothing was added after signing");
                check(row.page == 0, "the signature is on the page it was drawn on");
            }
            check(!window.isModified(), "signing leaves nothing unsaved");
            auto* banner = window.findChild<QToolBar*>(QStringLiteral("signatureBanner"));
            check(banner != nullptr && banner->isVisibleTo(&window),
                  "the signed-document banner is shown");
            auto* cards = window.findChild<SignatureCards*>();
            check(cards != nullptr && cards->count() == 1, "the Signatures tab has one card for the signature");
            window.sidebar()->showPanel(QStringLiteral("signatures"));
            pump(200);
            shot(&window, "ux-signature-card");
            try {
                leht::Document signedDoc = leht::Document::open(ctx, toSign.toStdString());
                check(signedDoc.signature_count() == 1, "the file on disk carries one signature");
            } catch (const leht::Error&) {
                check(false, "the signed file opens");
            }

            // The EU trusted lists (queue M5): with a cached list that names the
            // test CA as qualified, the same signature is a QES, and the Sign
            // menu offers the update. The list is written where the viewer
            // looks, under a temporary XDG_CACHE_HOME.
            {
                const QByteArray oldCache = qgetenv("XDG_CACHE_HOME");
                qputenv("XDG_CACHE_HOME", tmp.filePath(QStringLiteral("cache")).toUtf8());
                leht::trustlist::TrustedList tl;
                leht::trustlist::Service svc;
                svc.type = leht::trustlist::Service::Type::CaQc;
                svc.territory = "EE";
                svc.name = "Test qualified CA";
                unsigned char* caDer = nullptr;
                const int caLen = i2d_X509(pki.ca.p, &caDer);
                svc.certs = {std::vector<std::uint8_t>(caDer, caDer + caLen)};
                OPENSSL_free(caDer);
                leht::trustlist::Phase phase;
                phase.granted = true;
                leht::trustlist::Qualification q;
                q.qualifiers = leht::trustlist::QcStatement | leht::trustlist::QcForEsig |
                               leht::trustlist::QcWithQscd;
                q.criteria.assert = leht::trustlist::Criteria::Assert::AtLeastOne;
                q.criteria.key_usage = {{{"nonRepudiation", true}}};
                phase.qualifications = {q};
                svc.phases = {phase};
                tl.services = {svc};
                const QString cachePath = QString::fromStdString(leht::trustlist::default_cache_path());
                QDir().mkpath(QFileInfo(cachePath).absolutePath());
                {
                    QFile f(cachePath);
                    check(f.open(QIODevice::WriteOnly), "the trusted-list cache opens for writing");
                    const auto blob = leht::trustlist::encode(tl);
                    f.write(reinterpret_cast<const char*>(blob.data()), static_cast<qint64>(blob.size()));
                }
                reported.clear();
                QMetaObject::invokeMethod(worker, "listSignatures", Qt::QueuedConnection);
                pump(1500);
                check(reported.size() == 1 && reported.first().qualified == 2,
                      "with the EU trusted list, the signature is a qualified electronic signature");
                check(RenderWorker::trustedListState().present, "the viewer sees the cached list");
                auto* update = window.findChild<QAction*>(QStringLiteral("updateTrustedList"));
                check(update != nullptr && update->isEnabled(),
                      "the Sign menu offers to update the EU trusted lists");
                QFile::remove(cachePath);
                if (oldCache.isNull()) {
                    qunsetenv("XDG_CACHE_HOME");
                } else {
                    qputenv("XDG_CACHE_HOME", oldCache);
                }
                reported.clear();
                QMetaObject::invokeMethod(worker, "listSignatures", Qt::QueuedConnection);
                pump(1500);
                check(reported.size() == 1 && reported.first().qualified == 0,
                      "without it, nothing is said about qualified");
            }

            // A certification (M3): made through the same path, and afterwards
            // the tools it forbids are off, the ones it allows are on.
            {
                const QString certified = tmp.filePath(QStringLiteral("to_certify.pdf"));
                QFile::remove(certified);
                QFile::copy(QStringLiteral(LEHT_CORPUS_DIR "/text_10p.pdf"), certified);
                window.openPath(certified);
                pump(1500);
                {
                    SignDialog fresh(&window, 0, QRectF(), QString(), true);
                    auto* combo = fresh.findChildren<QComboBox*>().value(0);
                    bool found = false;
                    for (auto* c : fresh.findChildren<QComboBox*>()) {
                        found = found || c->count() == 4;
                    }
                    check(found && combo != nullptr, "the sign dialog offers to certify");
                }
                QVector<SigRow> certRows;
                const auto got = QObject::connect(
                    worker, &RenderWorker::signaturesReady, &window,
                    [&certRows](const QVector<SigRow>& rows) { certRows = rows; });
                SignSpec certify = spec;
                certify.rect = QRectF();
                certify.strokes.clear();
                certify.lines.clear();
                certify.password = QStringLiteral("pw");
                certify.certify = 2;
                QString certFailure;
                const auto failed = QObject::connect(
                    worker, &RenderWorker::saveFailed, &window,
                    [&certFailure](const QString& m) { certFailure = m; });
                QTimer closer;
                QObject::connect(&closer, &QTimer::timeout, [] {
                    if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                        box->accept();
                    }
                });
                closer.start(50);
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, certified), Q_ARG(SignSpec, certify));
                pump(3000);
                closer.stop();
                QObject::disconnect(failed);
                if (!certFailure.isEmpty()) {
                    std::printf("      certify failed: %s\n", qPrintable(certFailure));
                }
                check(certRows.size() == 1 && certRows.first().certification == 2,
                      "the signature certifies the document for form filling");
                QAction* highlight = nullptr;
                QAction* signTool = nullptr;
                for (QAction* a : window.findChildren<QAction*>()) {
                    if (a->text() == QStringLiteral("Highlight")) {
                        highlight = a;
                    } else if (a->text() == QStringLiteral("Sign")) {
                        signTool = a;
                    }
                }
                check(highlight != nullptr && !highlight->isEnabled(),
                      "a form-filling certification turns annotating off");
                check(signTool != nullptr && signTool->isEnabled(),
                      "and leaves signing on");
                shot(&window, "m3-certified");
                QObject::disconnect(got);
            }

            // An encrypted document is signed as it is: the worker holds it
            // authenticated, and the hole it leaves is plaintext, so this side
            // signs it exactly as it would an unencrypted one.
            const QString lockedSrc = QStringLiteral(LEHT_CORPUS_DIR "/locked.pdf");
            if (QFile::exists(lockedSrc)) {
                const QString encrypted = tmp.filePath(QStringLiteral("to_sign_locked.pdf"));
                QFile::remove(encrypted);
                QFile::copy(lockedSrc, encrypted);
                QTimer answer;
                QObject::connect(&answer, &QTimer::timeout, [] {
                    QWidget* modal = QApplication::activeModalWidget();
                    if (auto* ask = qobject_cast<QInputDialog*>(modal)) {
                        ask->setTextValue(QStringLiteral("s3cret"));
                        ask->accept();
                    } else if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                        box->accept();
                    }
                });
                answer.start(50);
                window.openPath(encrypted);
                pump(1500);
                QVector<SigRow> encRows;
                const auto got = QObject::connect(
                    worker, &RenderWorker::signaturesReady, &window,
                    [&encRows](const QVector<SigRow>& rows) { encRows = rows; });
                SignSpec invisible = spec;
                invisible.rect = QRectF();
                invisible.strokes.clear();
                invisible.lines.clear();
                invisible.password = QStringLiteral("pw");
                QString encFailure;
                const auto failed = QObject::connect(
                    worker, &RenderWorker::saveFailed, &window,
                    [&encFailure](const QString& m) { encFailure = m; });
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, encrypted), Q_ARG(SignSpec, invisible));
                pump(3000);
                answer.stop();
                QObject::disconnect(failed);
                QObject::disconnect(got);
                if (!encFailure.isEmpty()) {
                    std::printf("      encrypted signing failed: %s\n", qPrintable(encFailure));
                }
                check(encRows.size() == 1 && encRows.first().intact,
                      "the viewer signs an encrypted document, and the signature verifies");
                try {
                    leht::Document onDisk = leht::Document::open(ctx, encrypted.toStdString());
                    check(onDisk.needs_password() && onDisk.authenticate("s3cret") &&
                              onDisk.signature_count() == 1,
                          "the signed file is still encrypted, and carries the signature");
                } catch (const leht::Error&) {
                    check(false, "the signed encrypted file opens");
                }
            } else {
                std::printf("      SKIP encrypted signing: locked.pdf missing\n");
            }

            // Long-term validation (M4): sign with a timestamp and LTV through
            // the viewer, against a TSA and OCSP/CRL servers on localhost.
            {
                leht::test::LtvPki ltv;
                const QString ltvCa = tmp.filePath(QStringLiteral("ltv_ca.pem"));
                const QString ltvP12 = tmp.filePath(QStringLiteral("ltv_signer.p12"));
                {
                    QFile c(ltvCa);
                    check(c.open(QIODevice::WriteOnly), "the LTV CA file opens for writing");
                    c.write(ltv.ca.pem().c_str());
                    const auto bytes = leht::test::pkcs12(ltv.signer_key, ltv.signer, {&ltv.ca}, "pw");
                    QFile k(ltvP12);
                    check(k.open(QIODevice::WriteOnly), "the LTV key file opens for writing");
                    k.write(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<qint64>(bytes.size()));
                }
                QSettings().setValue(QStringLiteral("trustedCertificates"), QStringList{ca, ltvCa});
                const QString lta = tmp.filePath(QStringLiteral("to_sign_lta.pdf"));
                QFile::remove(lta);
                QFile::copy(QStringLiteral(LEHT_CORPUS_DIR "/text_10p.pdf"), lta);
                window.openPath(lta);
                pump(1500);
                QVector<SigRow> ltvRows;
                QStringList hosts;
                const auto got = QObject::connect(
                    worker, &RenderWorker::signaturesReady, &window,
                    [&ltvRows](const QVector<SigRow>& rows) { ltvRows = rows; });
                const auto net = QObject::connect(
                    worker, &RenderWorker::networkUsed, &window,
                    [&hosts](const QString& h) { hosts << h; });
                QString ltvFailure;
                const auto failed = QObject::connect(
                    worker, &RenderWorker::saveFailed, &window,
                    [&ltvFailure](const QString& m) { ltvFailure = m; });
                SignSpec b;
                b.p12Path = ltvP12;
                b.password = QStringLiteral("pw");
                b.tsaUrl = QString::fromStdString(ltv.tsa.url());
                b.ltv = true;
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, lta), Q_ARG(SignSpec, b));
                pump(5000);
                QObject::disconnect(failed);
                if (!ltvFailure.isEmpty()) {
                    std::printf("      LTV signing failed: %s\n", qPrintable(ltvFailure));
                }
                check(!hosts.isEmpty() && hosts.first() == QStringLiteral("127.0.0.1"),
                      "the viewer says where the network goes");
                check(ltvRows.size() == 2, "a signature and a document timestamp");
                if (ltvRows.size() == 2) {
                    const SigRow& sig = ltvRows[0];
                    check(sig.intact && sig.onlyValidationDataAfter && !sig.revoked,
                          "the signature is intact, and only validation data came after");
                    check(!sig.revocation.isEmpty() &&
                              sig.revocation.first().contains(QStringLiteral("good (OCSP, embedded")),
                          "the panel reports revocation from the embedded data");
                    check(ltvRows[1].documentTimestamp && ltvRows[1].intact,
                          "the document timestamp verifies");
                }
                auto* addLtv = window.findChild<QAction*>(QStringLiteral("addLongTermValidation"));
                check(addLtv != nullptr && addLtv->isEnabled(),
                      "Add Long-Term Validation is offered on a signed document");
                check(window.findChild<QPushButton*>(QStringLiteral("checkRevocationOnline")) !=
                          nullptr,
                      "the panel offers Check Revocation Online");
                shot(&window, "m4-ltv");

                // Revoked since, checked online now: after the timestamp, so
                // the signature still stands, and the panel says so.
                ltv.revocation.revoke(ltv.signer, static_cast<std::int64_t>(std::time(nullptr)) + 60);
                ltvRows.clear();
                QMetaObject::invokeMethod(worker, "checkRevocationOnline", Qt::QueuedConnection);
                pump(3000);
                check(ltvRows.size() == 2 && ltvRows[0].intact && !ltvRows[0].revoked &&
                          ltvRows[0].revocation.join(QStringLiteral(" ")).contains(
                              QStringLiteral("revoked later")),
                      "a revocation after the timestamp is reported, and does not undo it");
                QObject::disconnect(got);
                QObject::disconnect(net);
                QSettings().setValue(QStringLiteral("trustedCertificates"), QStringList{ca});
            }

            // Back to the signed copy for the card.
            window.openPath(toSign);
            pump(1500);

            // The same through a card: SoftHSM standing in for an ID card.
            if (leht::test::SoftHsm::available()) {
                leht::test::SoftHsm hsm;
                hsm.add(pki.ec, pki.ec_cert, "\x03", "Signature", true);
                qputenv("LEHT_PKCS11_MODULE", leht::test::SoftHsm::module().c_str());

                // The dialog, in card mode, finds the key on its own.
                {
                    SignDialog dialog(&window, 0, QRectF(), QString());
                    QRadioButton* card = nullptr;
                    for (auto* b : dialog.findChildren<QRadioButton*>()) {
                        if (b->text() == QStringLiteral("ID card or token")) {
                            card = b;
                        }
                    }
                    check(card != nullptr, "the sign dialog offers an ID card");
                    if (card != nullptr) {
                        card->setChecked(true);
                    }
                    bool listed = false;  // the key list, not the Certify choice
                    for (auto* combo : dialog.findChildren<QComboBox*>()) {
                        listed = listed || (combo->count() == 1 &&
                                            combo->itemText(0).contains(QStringLiteral("Jaan Tamm")));
                    }
                    check(listed, "the dialog lists the card's signing key");
                }

                QString failure;
                const auto failed = QObject::connect(
                    worker, &RenderWorker::saveFailed, &window,
                    [&failure](const QString& message) { failure = message; });
                const auto keys = leht::crypto::list_token_keys(leht::test::SoftHsm::module());
                check(keys.size() == 1, "the card holds one key");
                SignSpec cardSpec;
                cardSpec.pkcs11Uri = QString::fromStdString(keys.empty() ? "" : keys[0].uri);
                cardSpec.password = QStringLiteral("9999");
                // The window shows the failure in a modal box; close it, as
                // the person would, or the test waits forever.
                QTimer closer;
                QObject::connect(&closer, &QTimer::timeout, [] {
                    if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                        box->accept();
                    }
                });
                closer.start(50);
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, toSign), Q_ARG(SignSpec, cardSpec));
                pump(2000);
                closer.stop();
                check(failure.startsWith(QStringLiteral("The PIN is wrong.")),
                      "a wrong card PIN is reported as such");

                reported.clear();
                cardSpec.password = QString::fromLatin1(leht::test::SoftHsm::kPin);
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, toSign), Q_ARG(SignSpec, cardSpec));
                pump(3000);
                check(reported.size() == 2, "the card's signature is added as the second");
                if (reported.size() == 2) {
                    check(reported[0].intact && reported[1].intact,
                          "both signatures verify, the file one and the card one");
                    check(reported[1].signerCommonName == QStringLiteral("Jaan Tamm"),
                          "the panel names the card's signer");
                    check(!reported[0].changedAfterSigning ||
                              reported[0].laterSignatureCoversChanges,
                          "the card signature covers what came after the first");
                }
                QObject::disconnect(failed);
                qunsetenv("LEHT_PKCS11_MODULE");
            } else {
                std::printf("  skip  card signing (SoftHSM2 not installed)\n");
            }

            // And through a phone: SK's services imitated on localhost.
            {
                leht::test::LocalSmartId sid(pki);
                leht::test::LocalMobileId mid(pki, true);
                // A failure shows a modal box: note it and close it, or the
                // test waits forever.
                QStringList boxes;
                QTimer closer;
                QObject::connect(&closer, &QTimer::timeout, [&boxes] {
                    if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                        boxes << box->text();
                        std::printf("  (box: %s)\n", qPrintable(box->text()));
                        box->accept();
                    }
                });
                closer.start(50);
                qputenv("LEHT_SMARTID_URL", sid.service().base_url.c_str());
                qputenv("LEHT_MOBILEID_URL", mid.service().base_url.c_str());

                // The dialog offers both, and says where the digest goes.
                {
                    SignDialog dialog(&window, 0, QRectF(), QString());
                    int phones = 0;
                    for (auto* b : dialog.findChildren<QRadioButton*>()) {
                        if (b->text() == QStringLiteral("Smart-ID") ||
                            b->text() == QStringLiteral("Mobile-ID")) {
                            ++phones;
                            if (b->text() == QStringLiteral("Mobile-ID")) {
                                b->setChecked(true);
                            }
                        }
                    }
                    check(phones == 2, "the sign dialog offers Smart-ID and Mobile-ID");
                }

                // Smart-ID by QR code: the window draws the codes; the "phone"
                // scans the link the worker sent.
                int links = 0;
                int drawnSeen = 0;
                const auto linked = QObject::connect(
                    worker, &RenderWorker::phoneLink, &window, [&](const QString& link) {
                        ++links;
                        if (links == 2) {
                            (void)sid.scan(link.toStdString());
                        }
                        if (auto* d = window.findChild<PhoneSignDialog*>()) {
                            drawnSeen = std::max(drawnSeen, d->codesDrawn());
                        }
                    });
                const qsizetype before = reported.size();
                SignSpec qrSpec;
                qrSpec.phoneMethod = QStringLiteral("smart-id-qr");
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, toSign), Q_ARG(SignSpec, qrSpec));
                for (int i = 0; i < 100 && reported.size() == before; ++i) {
                    pump(100);
                }
                check(links >= 2, "the Smart-ID QR code is renewed while nobody scans");
                check(drawnSeen >= 1, "the phone window draws the QR code");
                check(reported.size() == before + 1, "the Smart-ID signature is added");
                if (reported.size() == before + 1) {
                    check(reported.last().intact, "the Smart-ID signature (RSA-PSS) verifies");
                    check(reported.last().signerCommonName ==
                              QStringLiteral("MAASIKAS,MARI,PNOEE-40504040001"),
                          "the panel names the Smart-ID signer");
                }
                check(window.findChild<PhoneSignDialog*>() == nullptr ||
                          !window.findChild<PhoneSignDialog*>()->isVisible(),
                      "the phone window closes when signing is done");
                QObject::disconnect(linked);

                // Mobile-ID: a verification code, and the phone's refusal.
                QString shown;
                const auto coded = QObject::connect(
                    worker, &RenderWorker::phoneCode, &window,
                    [&shown](const QString& c) { shown = c; });
                bool cancelled = false;
                const auto ended = QObject::connect(
                    worker, &RenderWorker::phoneSigningEnded, &window,
                    [&cancelled](bool c) { cancelled = c; });
                SignSpec midSpec;
                midSpec.phoneMethod = QStringLiteral("mobile-id");
                midSpec.phoneNumber = QString::fromLatin1(leht::test::LocalMobileId::kPhone);
                midSpec.phonePerson = QString::fromLatin1(leht::test::LocalMobileId::kId);
                const qsizetype beforeMid = reported.size();
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, toSign), Q_ARG(SignSpec, midSpec));
                for (int i = 0; i < 100 && reported.size() == beforeMid; ++i) {
                    pump(100);
                }
                check(reported.size() == beforeMid + 1, "the Mobile-ID signature is added");
                check(shown.toStdString() == leht::crypto::sk::mobile_id_code(mid.last_hash()),
                      "the window shows Mobile-ID's verification code");
                const QByteArray signedBytes = [&] {
                    QFile f(toSign);
                    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
                }();
                mid.outcome = leht::test::LocalMobileId::Outcome::UserCancelled;
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, toSign), Q_ARG(SignSpec, midSpec));
                for (int i = 0; i < 50 && !cancelled; ++i) {
                    pump(100);
                }
                check(cancelled, "declining on the phone ends as a cancel, not an error");
                QFile after(toSign);
                check(after.open(QIODevice::ReadOnly) && after.readAll() == signedBytes,
                      "a declined signature writes nothing");

                // Cancel in the window, while the phone keeps silent.
                mid.outcome = leht::test::LocalMobileId::Outcome::Ok;
                mid.polls_before_done = 100000;
                cancelled = false;
                QMetaObject::invokeMethod(worker, "signDocument", Qt::QueuedConnection,
                                          Q_ARG(QString, toSign), Q_ARG(SignSpec, midSpec));
                for (int i = 0; i < 50 && window.findChild<PhoneSignDialog*>() == nullptr; ++i) {
                    pump(50);
                }
                pump(500);
                if (auto* d = window.findChild<PhoneSignDialog*>()) {
                    d->reject();  // Cancel
                }
                for (int i = 0; i < 50 && !cancelled; ++i) {
                    pump(100);
                }
                check(cancelled, "Cancel in the phone window stops the signing");
                check(sid.complaints.none() && mid.complaints.none(),
                      "every request to SK was as SK documents it");
                check(boxes.isEmpty(), "phone signing put up no error box");
                closer.stop();
                QObject::disconnect(coded);
                QObject::disconnect(ended);
                qunsetenv("LEHT_SMARTID_URL");
                qunsetenv("LEHT_MOBILEID_URL");
            }
            QObject::disconnect(reportedConnection);
            settings.remove(QStringLiteral("trustedCertificates"));
        } else {
            std::printf("  skip  signing (could not copy the corpus file)\n");
        }
    }

    if (g_failures > 0) {
        std::printf("%d smoke check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("viewer smoke test passed\n");
    return 0;
}
