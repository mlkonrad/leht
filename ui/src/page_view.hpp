// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QAbstractScrollArea>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QPair>
#include <QPolygonF>
#include <QSet>
#include <QVector>

#include "edit_model.hpp"

class QPlainTextEdit;

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
    void fitPage();

    /// View rotation, a multiple of 90 degrees applied to every page.
    [[nodiscard]] int rotation() const { return rotation_; }
    void rotateBy(int degrees);

    // Keyboard navigation.
    void nextPage();
    void previousPage();
    void firstPage();
    void lastPage();

    [[nodiscard]] int pageCount() const { return baseSizes_.size(); }
    /// `page`'s size in points (base coordinates), and its latest rendered
    /// image (null if none yet): for previews.
    [[nodiscard]] QSizeF pageSizePoints(int page) const { return baseSizes_.value(page); }
    [[nodiscard]] QImage pageImage(int page) const { return rendered_.value(page).image; }
    /// The page currently nearest the top of the viewport, 0-based.
    [[nodiscard]] int currentPage() const;

    /// Scrolls so `page` is at the top of the viewport, offset by `yBase`
    /// (unscaled points down the page). Used by the outline and go-to-page.
    void goToPage(int page, double yBase = 0.0);

    // --- Find ---------------------------------------------------------------
    /// Drops all current matches and the selection; call before a new search.
    void clearMatches();
    /// Match boxes for one page, in base (zoom-1.0) coordinates.
    void addMatches(int page, const QVector<QRectF>& boxes);
    /// Called when a search completes; moves to the first match.
    void finishMatches(int total);
    void nextMatch();
    void prevMatch();
    [[nodiscard]] int matchCount() const { return matchOrder_.size(); }
    [[nodiscard]] int currentMatchIndex() const { return currentMatch_; }

    // --- Selection ----------------------------------------------------------
    /// Selection boxes + text for one page, in base coordinates, from the worker.
    void setSelection(int page, const QVector<QRectF>& boxes, const QString& text);
    [[nodiscard]] QString selectedText() const { return selectionText_; }
    void copySelection() const;

    /// Whether `page` has been marked as failed (see markPageFailed).
    [[nodiscard]] bool isPageFailed(int page) const { return failed_.contains(page); }

    // --- Editing tools ------------------------------------------------------
    /// What a left-button drag or click does. Every tool but Select works in
    /// base coordinates, so needs the view unrotated (see setTool).
    enum class Tool { Select, Highlight, Note, Ink, Redact, Erase, Sign, Move, Text, Crop };
    /// Returns false (and keeps Select) if `tool` needs an unrotated view and
    /// the view is rotated.
    bool setTool(Tool tool);
    [[nodiscard]] Tool tool() const { return tool_; }

    /// The annotations currently on the document, for the Erase and Move
    /// tools' hit tests and outlines.
    void setAnnotations(const QVector<AnnotRow>& rows);
    /// The annotation the Move tool has selected; 0 for none.
    [[nodiscard]] int selectedAnnotation() const { return selectedAnnot_; }
    /// Opens the in-place text editor on free-text annotation `id` (as a
    /// double-click does). Returns false if there is no such free text.
    bool editFreeText(int id);
    /// The text editor, while it is open; for tests.
    [[nodiscard]] QPlainTextEdit* textEditor() const;

public slots:
    /// A finished render from the worker. Ignored if the zoom has since changed.
    void onRendered(int page, double zoom, int rotation, quint64 generation, QImage image);

    /// `page` crashed the document worker and will not be rendered. Drawn as
    /// a labelled placeholder rather than a blank sheet, so a missing page is
    /// visibly missing instead of looking empty.
    void markPageFailed(int page);

    /// The document changed (see RenderWorker::documentEdited): re-render the
    /// pages named, taking `baseSizes` as the sizes now. The old images stay
    /// on screen until the new ones land, so an edit never flashes blank.
    void onDocumentEdited(QVector<int> pages, bool allPages, QVector<QSize> baseSizes);

signals:
    /// The view wants `page` rendered at `zoom`. `generation` lets the worker
    /// drop this request if a newer one has superseded it.
    void needRender(int page, double zoom, int rotation, quint64 generation);
    /// The generation advanced (scroll or zoom); the worker should catch up.
    void generationChanged(quint64 generation);
    void currentPageChanged(int page);
    /// A drag or double-click wants text selected on `page`, in base coords.
    void selectRequested(int page, QPointF aBase, QPointF bBase, int mode);
    void matchNavigated(int index, int total);

    // Editing requests, all in base coordinates on one page.
    void highlightRequested(int page, QVector<QRectF> boxes);
    void noteRequested(int page, QPointF at);
    void inkRequested(int page, QVector<QPolygonF> strokes);
    void redactRequested(int page, QRectF box);
    /// A box was dragged with the Sign tool: where a visible signature goes.
    void signRequested(int page, QRectF box);
    void eraseRequested(int annotId);
    /// The Move tool dropped annotation `annotId` with its bounds now `to`.
    void moveRequested(int annotId, QRectF to);
    /// New free text typed with the Text tool.
    void freeTextRequested(int page, QRectF box, QString text, double size, QColor color);
    /// Different words for an existing free-text annotation.
    void annotationTextRequested(int annotId, QString text);
    /// A sticky note was double-clicked: its text should be edited.
    void noteEditRequested(int annotId, QString current);
    /// A box was dragged with the Crop tool: the part of the page to keep.
    void cropBoxRequested(int page, QRectF box);
    /// A tool could not be used, with the reason, for the status bar.
    void toolRefused(QString reason);

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

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

    /// Where page `page` is drawn right now, in viewport coordinates.
    [[nodiscard]] QRect pageRectInViewport(int page) const;
    /// A base-coord rect on `page` mapped to viewport coordinates.
    [[nodiscard]] QRectF baseRectToViewport(int page, const QRectF& base) const;
    /// A viewport point mapped to (page, base coordinates). Returns page -1 if
    /// the point is not over any page.
    void viewportToPage(QPoint pos, int& page, QPointF& base) const;
    void scrollToCurrentMatch();

    QVector<QSize> baseSizes_;             // at zoom 1.0
    double zoom_ = 1.0;
    int rotation_ = 0;  // 0, 90, 180 or 270
    quint64 generation_ = 0;
    int lastReportedPage_ = -1;

    /// The zoom pages are rendered at: the view's zoom times the screen's
    /// device pixel ratio, so a scaled display (2x, or a fractional 1.33x) gets
    /// one image pixel per device pixel instead of an upscaled, blurry page.
    double renderZoom() const { return zoom_ * devicePixelRatioF(); }

    /// Rendered pages, keyed by page index. Each entry remembers the render
    /// zoom it was made at so a stale image can be shown (scaled) until the
    /// sharp one arrives, rather than flashing a placeholder.
    struct Rendered {
        QImage image;
        double zoom = 0.0;
        int rotation = 0;
    };
    QHash<int, Rendered> rendered_;
    QSet<int> requested_;                  // in flight at the current generation
    QSet<int> failed_;                     // pages that crashed the worker

    // Find: match boxes per page (base coords), plus a flat page-ordered list
    // for next/prev navigation.
    QHash<int, QVector<QRectF>> matches_;
    QVector<QPair<int, int>> matchOrder_;  // (page, index-within-page)
    int currentMatch_ = -1;

    // Selection: boxes on one page (base coords) plus the covered text.
    int selectionPage_ = -1;
    QVector<QRectF> selectionBoxes_;
    QString selectionText_;
    bool selecting_ = false;
    int selectAnchorPage_ = -1;
    QPointF selectAnchorBase_;
    quint64 selectsSent_ = 0;      ///< selection requests emitted...
    quint64 selectsReceived_ = 0;  ///< ...and answered
    bool highlightWhenSettled_ = false;

    // Editing tools.
    Tool tool_ = Tool::Select;
    QVector<AnnotRow> annotations_;
    int dragPage_ = -1;            ///< page an ink stroke or redaction box is on
    QPointF dragStart_;            ///< base coordinates
    QPointF dragNow_;
    QPolygonF stroke_;             ///< the ink stroke being drawn, base coordinates
    /// Renders requested before the last edit show the old document; any that
    /// arrive late are dropped rather than shown as current.
    quint64 editFloor_ = 0;

    // Move tool: the selected annotation, and what a drag is doing to it.
    enum class Grip { None, Body, N, S, E, W, NE, NW, SE, SW };
    int selectedAnnot_ = 0;
    Grip grip_ = Grip::None;
    QRectF moveFrom_;   ///< the annotation's bounds when the drag began, base coords
    QRectF moveTo_;     ///< where it would go now
    QPointF grabBase_;  ///< where the drag began
    // Text tool: the in-place editor over a box on a page.
    QPlainTextEdit* editor_ = nullptr;
    int editorPage_ = -1;
    QRectF editorBox_;
    int editorAnnot_ = 0;  ///< 0 while typing new free text
    QString editorOriginal_;
    double editorSize_ = 12;
    QColor editorColor_ = Qt::black;

    [[nodiscard]] const AnnotRow* annotAt(int page, QPointF base) const;
    [[nodiscard]] const AnnotRow* annotById(int id) const;
    /// Which handle (or the body) of the selected annotation is under `pos`.
    [[nodiscard]] Grip gripAt(QPoint pos) const;
    /// `pos` in `page`'s base coordinates, even when it is off the page.
    [[nodiscard]] QPointF toBase(int page, QPoint pos) const;
    /// moveFrom_ changed by a drag to `base`.
    [[nodiscard]] QRectF dragged(QPointF base, bool keepAspect) const;
    void openEditor(int page, QRectF box, int annotId, const QString& text, double size,
                    QColor color);
    void placeEditor();
    void commitEditor();
    void cancelEditor();

    void emitSelect(int page, QPointF a, QPointF b, int mode);
    /// Turns the settled selection into a highlight request, then clears it.
    void finishHighlight();
    [[nodiscard]] bool editing() const { return tool_ != Tool::Select; }
};
