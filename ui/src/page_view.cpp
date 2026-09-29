// SPDX-License-Identifier: AGPL-3.0-or-later
#include "page_view.hpp"

#include "comment_popup.hpp"
#include "comments_panel.hpp"
#include "leht/ops/forms.hpp"

#include <QApplication>
#include <QClipboard>
#include <QHelpEvent>
#include <QComboBox>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollBar>
#include <QToolTip>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace {

/// Something to read on the page: a note (even an empty one, to be written
/// in), or any other comment with text. Links and form widgets are not.
bool readable(const AnnotRow& a) {
    if (a.type == QLatin1String("Link") || a.type == QLatin1String("Widget") ||
        a.type == QLatin1String("Popup")) {
        return false;
    }
    return a.type == QLatin1String("Text") || !a.contents.trimmed().isEmpty();
}

/// The hover text for a comment: its type, author and words. All of it comes
/// from the PDF, so all of it is escaped before Qt reads it as rich text.
QString commentToolTip(const AnnotRow& a) {
    constexpr int kMaxChars = 300;
    QString text = a.contents.trimmed();
    if (text.size() > kMaxChars) {
        int n = kMaxChars - 1;
        if (text.at(n - 1).isHighSurrogate()) {
            --n;  // never half a character
        }
        text = text.left(n) + QChar(0x2026);
    }
    QString html = QStringLiteral("<b>%1</b>").arg(annotationTypeName(a.type).toHtmlEscaped());
    if (!a.author.trimmed().isEmpty()) {
        html += QStringLiteral(" — ") + a.author.trimmed().toHtmlEscaped();
    }
    if (!text.isEmpty()) {
        html += QStringLiteral("<br>") +
                text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    }
    return QStringLiteral("<qt>") + html + QStringLiteral("</qt>");
}
using leht::ops::FieldType;

FieldType typeOf(const FieldRow& f) { return static_cast<FieldType>(f.type); }

/// Filled in through an editor over the field, rather than by a click.
bool typedIn(const FieldRow& f) {
    return typeOf(f) == FieldType::Text || typeOf(f) == FieldType::Choice;
}

/// A combo box that says when its list closes, chosen from or not.
class FieldCombo final : public QComboBox {
public:
    using QComboBox::QComboBox;
    std::function<void()> popupHidden;

    void hidePopup() override {
        QComboBox::hidePopup();
        if (popupHidden) {
            popupHidden();
        }
    }
};

}  // namespace

PageView::PageView(QWidget* parent) : QAbstractScrollArea(parent) {
    setFrameShape(QFrame::NoFrame);
    viewport()->setBackgroundRole(QPalette::Dark);
    verticalScrollBar()->setSingleStep(40);
    setFocusPolicy(Qt::StrongFocus);  // so PageUp/Down, Home/End, arrows arrive
    setAccessibleName(tr("Document"));
    setAccessibleDescription(tr("No document open"));
    viewport()->setMouseTracking(true);  // hover: the cursor and comment tooltips
}

void PageView::setPages(const QVector<QSize>& baseSizes) {
    baseSizes_ = baseSizes;
    rendered_.clear();
    requested_.clear();
    failed_.clear();
    lastReportedPage_ = -1;
    relayout();
    requestVisible();
}

void PageView::clear() {
    redactionMarks_.clear();
    fields_.clear();
    currentField_.clear();
    cancelFieldEditor();
    cancelEditor();
    closePopup(false);
    selectedAnnot_ = 0;
    grip_ = Grip::None;
    baseSizes_.clear();
    rendered_.clear();
    requested_.clear();
    failed_.clear();
    clearMatches();
    relayout();
    viewport()->update();
}

void PageView::setRedactionMarks(const QVector<QPair<int, QRectF>>& marks) {
    redactionMarks_ = marks;
    viewport()->update();
}

void PageView::setFormFields(const QVector<FieldRow>& fields) {
    fields_.clear();
    for (FieldRow f : fields) {
        // A button does nothing here and a signature field has its own tool;
        // a field without a widget box has nowhere to be drawn.
        const FieldType type = typeOf(f);
        if (type == FieldType::PushButton || type == FieldType::Signature ||
            type == FieldType::Unknown) {
            continue;
        }
        if (f.widgets.isEmpty()) {
            f.widgets.push_back(FieldWidget{f.page, f.rect, f.options.value(0)});
        }
        f.widgets.erase(std::remove_if(f.widgets.begin(), f.widgets.end(),
                                       [](const FieldWidget& w) { return w.rect.isEmpty(); }),
                        f.widgets.end());
        if (!f.widgets.isEmpty()) {
            fields_.push_back(f);
        }
    }
    // Tab on the page walks the fields as the Form panel lists them.
    std::stable_sort(fields_.begin(), fields_.end(), fieldReadsBefore);
    if (fieldEditor_ != nullptr) {
        const FieldRow* open = fieldByName(fieldEditorName_);
        if (open == nullptr || fieldEditorWidget_ >= open->widgets.size() || open->readOnly) {
            cancelFieldEditor();  // gone, or changed under the editor (an undo)
        } else {
            placeFieldEditor();
        }
    }
    viewport()->update();
}

void PageView::setFieldsShown(bool shown) {
    fieldsShown_ = shown;
    viewport()->update();
}

void PageView::setFieldsEditable(bool editable) {
    fieldsEditable_ = editable;
    if (!editable) {
        cancelFieldEditor();
    }
}

void PageView::setCurrentField(const QString& name) {
    currentField_ = name;
    const FieldRow* f = fieldByName(name);
    if (f != nullptr && f->page < baseSizes_.size()) {
        const QRectF shown = baseRectToViewport(f->page, f->rect);
        if (!viewport()->rect().contains(shown.toAlignedRect())) {
            // A third of the way down, as a find match is.
            const int y = pageTop(f->page) + int(f->rect.center().y() * zoom_) - viewport()->height() / 3;
            verticalScrollBar()->setValue(std::clamp(y, 0, verticalScrollBar()->maximum()));
            requestVisible();
        }
    }
    viewport()->update();
}

PageView::FieldHit PageView::fieldAt(int page, QPointF base) const {
    // Whether the outlines are shown or not: they only say where fields are.
    for (const FieldRow& f : fields_) {
        for (int i = 0; i < f.widgets.size(); ++i) {
            if (f.widgets[i].page == page && f.widgets[i].rect.contains(base)) {
                return FieldHit{&f, i};
            }
        }
    }
    return {};
}

QRectF PageView::fieldWidgetRect(const QString& name, int widget) const {
    const FieldRow* f = fieldByName(name);
    if (f == nullptr || widget < 0 || widget >= f->widgets.size()) {
        return {};
    }
    return baseRectToViewport(f->widgets[widget].page, f->widgets[widget].rect);
}

const FieldRow* PageView::fieldByName(const QString& name) const {
    const auto it = std::find_if(fields_.cbegin(), fields_.cend(),
                                 [&](const FieldRow& f) { return f.name == name; });
    return it != fields_.cend() ? &*it : nullptr;
}

void PageView::forgetPages() {
    redactionMarks_.clear();
    commitFieldEditor(false);  // by name: still right after pages move
    cancelEditor();
    closePopup(false);  // its annotation may be another one now
    selectedAnnot_ = 0;
    grip_ = Grip::None;
    rendered_.clear();
    requested_.clear();
    failed_.clear();
    selectionPage_ = -1;
    selectionBoxes_.clear();
    selectionText_.clear();
    clearMatches();
}

QSize PageView::scaledSize(int page) const {
    QSize base = baseSizes_.value(page);
    if (rotation_ == 90 || rotation_ == 270) {
        base.transpose();  // a quarter-turn swaps width and height
    }
    return QSize(std::lround(base.width() * zoom_),
                 std::lround(base.height() * zoom_));
}

int PageView::columnWidth() const {
    int w = 0;
    for (int p = 0; p < baseSizes_.size(); ++p) {
        w = std::max(w, scaledSize(p).width());
    }
    return w;
}

int PageView::totalHeight() const {
    int h = kMargin;
    for (int p = 0; p < baseSizes_.size(); ++p) {
        h += scaledSize(p).height() + kGap;
    }
    return h - kGap + kMargin;  // no trailing gap
}

int PageView::pageTop(int page) const {
    int y = kMargin;
    for (int p = 0; p < page; ++p) {
        y += scaledSize(p).height() + kGap;
    }
    return y;
}

void PageView::relayout() {
    const int contentW = columnWidth();
    const int contentH = totalHeight();
    const QSize vp = viewport()->size();

    horizontalScrollBar()->setRange(0, std::max(0, contentW - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setRange(0, std::max(0, contentH - vp.height()));
    verticalScrollBar()->setPageStep(vp.height());
    placeEditor();
    placePopup();
    placeFieldEditor();
}

void PageView::setZoom(double zoom) {
    zoom = std::clamp(zoom, 0.1, 12.0);
    if (std::abs(zoom - zoom_) < 1e-6) {
        return;
    }

    // Keep the page under the viewport centre roughly in place across a zoom.
    const int anchor = currentPage();
    const double intoPage =
        anchor >= 0 && scaledSize(anchor).height() > 0
            ? double(verticalScrollBar()->value() - pageTop(anchor)) /
                  scaledSize(anchor).height()
            : 0.0;

    zoom_ = zoom;
    relayout();
    if (anchor >= 0) {
        verticalScrollBar()->setValue(
            pageTop(anchor) + int(intoPage * scaledSize(anchor).height()));
    }

    // Every cached image is now the wrong size; keep them as stretched proxies
    // until sharp ones arrive, but request fresh renders.
    requested_.clear();
    bumpGeneration();
    requestVisible();
    viewport()->update();
}

void PageView::zoomBy(double factor) { setZoom(zoom_ * factor); }

void PageView::fitWidth() {
    if (baseSizes_.isEmpty()) {
        return;
    }
    const bool turned = rotation_ == 90 || rotation_ == 270;
    int widest = 1;
    for (const QSize& s : baseSizes_) {
        widest = std::max(widest, turned ? s.height() : s.width());
    }
    const int avail = viewport()->width() - 2 * kMargin;
    setZoom(double(avail) / widest);
}

void PageView::fitPage() {
    const int page = currentPage();
    if (page < 0) {
        return;
    }
    const bool turned = rotation_ == 90 || rotation_ == 270;
    const QSize base = baseSizes_.value(page);
    const double w = turned ? base.height() : base.width();
    const double h = turned ? base.width() : base.height();
    const double zx = (viewport()->width() - 2 * kMargin) / w;
    const double zy = (viewport()->height() - 2 * kMargin) / h;
    setZoom(std::min(zx, zy));
}

void PageView::rotateBy(int degrees) {
    commitFieldEditor();  // it sits over the unrotated page
    rotation_ = (((rotation_ + degrees) % 360) + 360) % 360;
    if (rotation_ != 0 && editing()) {
        tool_ = Tool::Select;
        emit toolRefused(tr("Editing tools need the view unrotated; back to Select."));
    }
    // Every cached image is now the wrong orientation; drop them and re-render.
    rendered_.clear();
    requested_.clear();
    relayout();
    bumpGeneration();
    requestVisible();
    viewport()->update();
}

void PageView::nextPage() { goToPage(std::min(currentPage() + 1, pageCount() - 1)); }
void PageView::previousPage() { goToPage(std::max(currentPage() - 1, 0)); }
void PageView::firstPage() { goToPage(0); }
void PageView::lastPage() { goToPage(pageCount() - 1); }

void PageView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && commentPopup() != nullptr) {
        closePopup(false);
        event->accept();
        return;
    }
    // The Move tool's selection: Delete removes it, arrows nudge it.
    if (tool_ == Tool::Move && selectedAnnot_ != 0) {
        const AnnotRow* a = annotById(selectedAnnot_);
        const double step = (event->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
        QPointF by;
        switch (event->key()) {
            case Qt::Key_Delete:
            case Qt::Key_Backspace:
                emit eraseRequested(std::exchange(selectedAnnot_, 0));
                viewport()->update();
                event->accept();
                return;
            case Qt::Key_Escape:
                selectedAnnot_ = 0;
                viewport()->update();
                event->accept();
                return;
            case Qt::Key_Left:  by = {-step, 0}; break;
            case Qt::Key_Right: by = {step, 0}; break;
            case Qt::Key_Up:    by = {0, -step}; break;
            case Qt::Key_Down:  by = {0, step}; break;
            default: break;
        }
        if (a != nullptr && !by.isNull()) {
            const QRectF to = a->rect.normalized().translated(by);
            for (AnnotRow& row : annotations_) {
                if (row.id == a->id) {
                    row.rect = to;  // shown there at once; the worker confirms
                }
            }
            emit moveRequested(selectedAnnot_, to);
            viewport()->update();
            event->accept();
            return;
        }
    }
    switch (event->key()) {
        case Qt::Key_PageDown:
        case Qt::Key_Space:
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepAdd);
            break;
        case Qt::Key_PageUp:
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
            break;
        case Qt::Key_Home:
            firstPage();
            break;
        case Qt::Key_End:
            lastPage();
            break;
        case Qt::Key_Down:
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd);
            break;
        case Qt::Key_Up:
            verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
            break;
        default:
            QAbstractScrollArea::keyPressEvent(event);
            return;
    }
    event->accept();
}

int PageView::currentPage() const {
    if (baseSizes_.isEmpty()) {
        return -1;
    }
    const int mid = verticalScrollBar()->value() + viewport()->height() / 2;
    for (int p = 0; p < baseSizes_.size(); ++p) {
        if (mid < pageTop(p) + scaledSize(p).height() + kGap) {
            return p;
        }
    }
    return baseSizes_.size() - 1;
}

void PageView::goToPage(int page, double yBase) {
    if (page < 0 || page >= baseSizes_.size()) {
        return;
    }
    const int y = pageTop(page) + int(yBase * zoom_) - kMargin;
    verticalScrollBar()->setValue(std::clamp(y, 0, verticalScrollBar()->maximum()));
    requestVisible();
    viewport()->update();
}

void PageView::bumpGeneration() {
    ++generation_;
    emit generationChanged(generation_);
}

void PageView::requestVisible() {
    if (baseSizes_.isEmpty()) {
        return;
    }
    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();

    for (int p = 0; p < baseSizes_.size(); ++p) {
        const int y0 = pageTop(p);
        const int y1 = y0 + scaledSize(p).height();
        // A one-viewport margin above and below, so scrolling stays ahead.
        const bool near = y1 >= top - viewport()->height() &&
                          y0 <= bottom + viewport()->height();
        if (!near) {
            continue;
        }
        const auto it = rendered_.constFind(p);
        const bool sharp =
            it != rendered_.constEnd() && std::abs(it->zoom - renderZoom()) < 1e-6;
        if (!sharp && !requested_.contains(p)) {
            requested_.insert(p);
            emit needRender(p, renderZoom(), rotation_, generation_);
        }
    }
}

void PageView::onRendered(int page, double zoom, int rotation,
                          quint64 generation, QImage image) {
    if (generation < editFloor_) {
        return;  // rendered from the document as it was before an edit
    }
    requested_.remove(page);
    // Accept it even if zoom/rotation moved on: a scaled stale image beats a
    // blank placeholder, and the fresh one will replace it.
    rendered_.insert(page, Rendered{image, zoom, rotation});
    viewport()->update();
}

QRect PageView::pageRectInViewport(int page) const {
    const QSize size = scaledSize(page);
    const int x = kMargin + (columnWidth() - size.width()) / 2 -
                  horizontalScrollBar()->value();
    const int y = pageTop(page) - verticalScrollBar()->value();
    return QRect(x, y, size.width(), size.height());
}

QRectF PageView::baseRectToViewport(int page, const QRectF& base) const {
    const QRect pr = pageRectInViewport(page);
    return QRectF(pr.x() + base.x() * zoom_, pr.y() + base.y() * zoom_,
                  base.width() * zoom_, base.height() * zoom_);
}

void PageView::viewportToPage(QPoint pos, int& page, QPointF& base) const {
    page = -1;
    for (int p = 0; p < baseSizes_.size(); ++p) {
        const QRect pr = pageRectInViewport(p);
        if (pr.top() > pos.y()) {
            break;  // pages are top-to-bottom; past the cursor already
        }
        if (pr.contains(pos)) {
            page = p;
            base = QPointF((pos.x() - pr.x()) / zoom_, (pos.y() - pr.y()) / zoom_);
            return;
        }
    }
}

// --- Find -------------------------------------------------------------------

void PageView::clearMatches() {
    matches_.clear();
    matchOrder_.clear();
    currentMatch_ = -1;
    selectionPage_ = -1;
    selectionBoxes_.clear();
    selectionText_.clear();
    viewport()->update();
}

void PageView::addMatches(int page, const QVector<QRectF>& boxes) {
    matches_[page] = boxes;
    viewport()->update();
}

void PageView::finishMatches(int /*total*/) {
    // Build a page-ordered flat list for next/prev.
    matchOrder_.clear();
    QList<int> pages = matches_.keys();
    std::sort(pages.begin(), pages.end());
    for (int p : pages) {
        for (int i = 0; i < matches_[p].size(); ++i) {
            matchOrder_.push_back({p, i});
        }
    }
    currentMatch_ = matchOrder_.isEmpty() ? -1 : 0;
    if (currentMatch_ >= 0) {
        scrollToCurrentMatch();
    }
    emit matchNavigated(currentMatch_, matchOrder_.size());
    viewport()->update();
}

void PageView::nextMatch() {
    if (matchOrder_.isEmpty()) {
        return;
    }
    currentMatch_ = (currentMatch_ + 1) % matchOrder_.size();
    scrollToCurrentMatch();
    emit matchNavigated(currentMatch_, matchOrder_.size());
    viewport()->update();
}

void PageView::prevMatch() {
    if (matchOrder_.isEmpty()) {
        return;
    }
    currentMatch_ =
        (currentMatch_ - 1 + matchOrder_.size()) % matchOrder_.size();
    scrollToCurrentMatch();
    emit matchNavigated(currentMatch_, matchOrder_.size());
    viewport()->update();
}

void PageView::scrollToCurrentMatch() {
    if (currentMatch_ < 0 || currentMatch_ >= matchOrder_.size()) {
        return;
    }
    const auto [page, idx] = matchOrder_.at(currentMatch_);
    const QRectF box = matches_.value(page).value(idx);
    // Put the match a third of the way down the viewport.
    const int targetY = pageTop(page) + int(box.center().y() * zoom_) -
                        viewport()->height() / 3;
    verticalScrollBar()->setValue(targetY);
    requestVisible();
}

// --- Selection --------------------------------------------------------------

void PageView::emitSelect(int page, QPointF a, QPointF b, int mode) {
    ++selectsSent_;
    emit selectRequested(page, a, b, mode);
}

void PageView::setSelection(int page, const QVector<QRectF>& boxes,
                            const QString& text) {
    ++selectsReceived_;
    selectionPage_ = page;
    selectionBoxes_ = boxes;
    selectionText_ = text;
    // The highlight tool acts on the selection once the reply to the last
    // drag position is in: replies arrive in request order.
    if (highlightWhenSettled_ && selectsReceived_ == selectsSent_) {
        finishHighlight();
    }
    viewport()->update();
}

void PageView::finishHighlight() {
    highlightWhenSettled_ = false;
    if (selectionPage_ >= 0 && !selectionBoxes_.isEmpty()) {
        if (tool_ == Tool::Underline || tool_ == Tool::StrikeOut) {
            emit markupRequested(selectionPage_, selectionBoxes_, tool_ == Tool::StrikeOut);
        } else {
            emit highlightRequested(selectionPage_, selectionBoxes_);
        }
    }
    selectionPage_ = -1;
    selectionBoxes_.clear();
    selectionText_.clear();
    viewport()->update();
}

bool PageView::setTool(Tool tool) {
    if (tool != Tool::Select && rotation_ != 0) {
        emit toolRefused(tr("Editing tools need the view unrotated (Ctrl+R to rotate back)."));
        tool_ = Tool::Select;
        return false;
    }
    commitEditor();
    closePopup(true);
    commitFieldEditor();
    tool_ = tool;
    highlightWhenSettled_ = false;
    dragPage_ = -1;
    stroke_.clear();
    selectedAnnot_ = 0;
    grip_ = Grip::None;
    pressComment_ = 0;
    viewport()->setCursor(toolCursor());
    viewport()->update();
    return true;
}

Qt::CursorShape PageView::toolCursor() const {
    return tool_ == Tool::Select ? Qt::IBeamCursor
           : tool_ == Tool::Erase ? Qt::PointingHandCursor
           : tool_ == Tool::Move  ? Qt::ArrowCursor
                                  : Qt::CrossCursor;
}

PageView::HoverTarget PageView::hoverTargetAt(QPoint pos) const {
    HoverTarget t;
    if (rotation_ != 0) {
        return t;  // the hit tests work in unrotated page coordinates
    }
    viewportToPage(pos, t.page, t.base);
    if (t.page < 0) {
        return t;
    }
    const FieldHit hit = fieldAt(t.page, t.base);
    t.field = hit.field;
    t.fieldWidget = hit.widget;
    // Topmost first, as annotAt: the last one drawn is on top.
    for (auto it = annotations_.crbegin(); it != annotations_.crend(); ++it) {
        if (it->page == t.page && readable(*it) &&
            it->rect.normalized().adjusted(-2, -2, 2, 2).contains(t.base)) {
            t.annot = &*it;
            break;
        }
    }
    return t;
}

void PageView::updateHoverCursor(QPoint pos) {
    Qt::CursorShape shape = toolCursor();
    if (tool_ == Tool::Select || tool_ == Tool::Note) {
        const HoverTarget t = hoverTargetAt(pos);
        const bool opens = tool_ == Tool::Select
                               ? t.field != nullptr || t.annot != nullptr
                               : t.annot != nullptr && t.annot->type == QLatin1String("Text");
        if (opens) {
            shape = Qt::PointingHandCursor;
        }
    }
    if (viewport()->cursor().shape() != shape) {
        viewport()->setCursor(shape);
    }
}

void PageView::setCommentsEditable(bool editable) {
    commentsEditable_ = editable;
    if (const CommentPopup* p = commentPopup()) {
        // Reopened read-only (or with Edit back), same comment.
        const int id = p->annotationId();
        closePopup(false);
        (void)showComment(id, false);
    }
}

CommentPopup* PageView::commentPopup() const {
    return popup_ != nullptr && popup_->isVisible() ? popup_ : nullptr;
}

bool PageView::showComment(int id, bool edit) {
    const AnnotRow* a = annotById(id);
    if (a == nullptr || a->page < 0 || a->page >= baseSizes_.size()) {
        return false;
    }
    commitEditor();  // one thing typed into at a time
    commitFieldEditor(false);
    const QRectF shown = baseRectToViewport(a->page, a->rect.normalized());
    if (!viewport()->rect().intersects(shown.toAlignedRect())) {
        // A third of the way down, as a find match is.
        const int y = pageTop(a->page) + int(a->rect.normalized().top() * zoom_) -
                      viewport()->height() / 3;
        verticalScrollBar()->setValue(std::clamp(y, 0, verticalScrollBar()->maximum()));
        requestVisible();
    }
    if (popup_ == nullptr) {
        popup_ = new CommentPopup(viewport());
        connect(popup_, &CommentPopup::textRequested, this, &PageView::annotationTextRequested);
        connect(popup_, &CommentPopup::deleteRequested, this, &PageView::eraseRequested);
    }
    QToolTip::hideText();
    popup_->showFor(*a, commentsEditable_, edit);
    placePopup();
    return true;
}

void PageView::placePopup() {
    if (popup_ == nullptr || !popup_->isVisible()) {
        return;
    }
    const AnnotRow* a = annotById(popup_->annotationId());
    if (a == nullptr || a->page >= baseSizes_.size()) {
        return;
    }
    popup_->place(baseRectToViewport(a->page, a->rect.normalized()));
}

void PageView::closePopup(bool keepEdits) {
    if (popup_ != nullptr) {
        popup_->dismiss(keepEdits);
    }
}

void PageView::setAnnotations(const QVector<AnnotRow>& rows) {
    annotations_ = rows;
    if (annotById(selectedAnnot_) == nullptr) {
        selectedAnnot_ = 0;  // deleted, or undone out of existence
    }
    if (CommentPopup* p = commentPopup()) {
        if (const AnnotRow* a = annotById(p->annotationId())) {
            p->refresh(*a);
            placePopup();
        } else {
            closePopup(false);  // gone: deleted here, elsewhere, or undone
        }
    }
    viewport()->update();
}

const AnnotRow* PageView::annotAt(int page, QPointF base) const {
    // Topmost first: the last annotation drawn is the one on top.
    for (auto it = annotations_.crbegin(); it != annotations_.crend(); ++it) {
        if (it->page == page && it->rect.normalized().adjusted(-2, -2, 2, 2).contains(base)) {
            return &*it;
        }
    }
    return nullptr;
}

const AnnotRow* PageView::annotById(int id) const {
    for (const AnnotRow& a : annotations_) {
        if (id != 0 && a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

QPointF PageView::toBase(int page, QPoint pos) const {
    const QRect pr = pageRectInViewport(page);
    return QPointF((pos.x() - pr.x()) / zoom_, (pos.y() - pr.y()) / zoom_);
}

PageView::Grip PageView::gripAt(QPoint pos) const {
    const AnnotRow* a = annotById(selectedAnnot_);
    if (a == nullptr) {
        return Grip::None;
    }
    const QRectF r = baseRectToViewport(a->page, a->rect.normalized());
    if (a->resizable) {
        const struct {
            QPointF at;
            Grip grip;
        } handles[] = {
            {r.topLeft(), Grip::NW},     {r.topRight(), Grip::NE},
            {r.bottomLeft(), Grip::SW},  {r.bottomRight(), Grip::SE},
            {QPointF(r.center().x(), r.top()), Grip::N},
            {QPointF(r.center().x(), r.bottom()), Grip::S},
            {QPointF(r.left(), r.center().y()), Grip::W},
            {QPointF(r.right(), r.center().y()), Grip::E},
        };
        for (const auto& h : handles) {
            if (std::abs(h.at.x() - pos.x()) <= 6 && std::abs(h.at.y() - pos.y()) <= 6) {
                return h.grip;
            }
        }
    }
    return r.adjusted(-2, -2, 2, 2).contains(pos) ? Grip::Body : Grip::None;
}

QRectF PageView::dragged(QPointF base, bool keepAspect) const {
    const QPointF d = base - grabBase_;
    if (grip_ == Grip::Body) {
        return moveFrom_.translated(d);
    }
    QRectF r = moveFrom_;
    const bool left = grip_ == Grip::W || grip_ == Grip::NW || grip_ == Grip::SW;
    const bool right = grip_ == Grip::E || grip_ == Grip::NE || grip_ == Grip::SE;
    const bool top = grip_ == Grip::N || grip_ == Grip::NW || grip_ == Grip::NE;
    const bool bottom = grip_ == Grip::S || grip_ == Grip::SW || grip_ == Grip::SE;
    constexpr double kMin = 4;  // points: never collapse to nothing
    if (left) {
        r.setLeft(std::min(r.left() + d.x(), r.right() - kMin));
    }
    if (right) {
        r.setRight(std::max(r.right() + d.x(), r.left() + kMin));
    }
    if (top) {
        r.setTop(std::min(r.top() + d.y(), r.bottom() - kMin));
    }
    if (bottom) {
        r.setBottom(std::max(r.bottom() + d.y(), r.top() + kMin));
    }
    if (keepAspect && (left || right) && (top || bottom) && moveFrom_.height() > 0) {
        // A corner of a picture: follow the wider change, keep the shape.
        const double aspect = moveFrom_.width() / moveFrom_.height();
        const double w = std::max(r.width(), r.height() * aspect);
        const double h = w / aspect;
        if (left) {
            r.setLeft(r.right() - w);
        } else {
            r.setRight(r.left() + w);
        }
        if (top) {
            r.setTop(r.bottom() - h);
        } else {
            r.setBottom(r.top() + h);
        }
    }
    return r;
}

bool PageView::editFreeText(int id) {
    const AnnotRow* a = annotById(id);
    if (a == nullptr || a->type != QStringLiteral("FreeText")) {
        return false;
    }
    openEditor(a->page, a->rect.normalized(), a->id, a->contents,
               a->fontSize > 0 ? a->fontSize : 12, a->color.isValid() ? a->color : Qt::black);
    return true;
}

QPlainTextEdit* PageView::textEditor() const {
    return editor_ != nullptr && editor_->isVisible() ? editor_ : nullptr;
}

void PageView::openEditor(int page, QRectF box, int annotId, const QString& text, double size,
                          QColor color) {
    commitEditor();  // one at a time
    closePopup(true);
    if (editor_ == nullptr) {
        editor_ = new QPlainTextEdit(viewport());
        editor_->setObjectName(QStringLiteral("freeTextEditor"));
        editor_->installEventFilter(this);
        editor_->setFrameStyle(QFrame::Box | QFrame::Plain);
        editor_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        editor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        editor_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    editorPage_ = page;
    editorBox_ = box.normalized();
    editorAnnot_ = annotId;
    editorOriginal_ = text;
    editorSize_ = size;
    editorColor_ = color;
    QPalette pal = editor_->palette();
    pal.setColor(QPalette::Base, Qt::white);
    pal.setColor(QPalette::Text, color);
    editor_->setPalette(pal);
    editor_->setPlainText(text);
    editor_->moveCursor(QTextCursor::End);
    placeEditor();
    editor_->show();
    editor_->setFocus();
}

void PageView::placeEditor() {
    if (editor_ == nullptr || editorPage_ < 0 || editorPage_ >= baseSizes_.size()) {
        return;
    }
    // Helvetica is what the annotation is drawn in; the editor shows the text
    // at the size it will have on the page, at this zoom.
    QFont font(QStringLiteral("Helvetica"));
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(std::max(6, static_cast<int>(std::lround(editorSize_ * zoom_))));
    editor_->setFont(font);
    editor_->setGeometry(baseRectToViewport(editorPage_, editorBox_).toAlignedRect());
}

void PageView::commitEditor() {
    if (editor_ == nullptr || !editor_->isVisible()) {
        return;
    }
    const QString text = editor_->toPlainText();
    editor_->hide();
    setFocus();
    if (editorAnnot_ == 0) {
        if (!text.trimmed().isEmpty()) {
            emit freeTextRequested(editorPage_, editorBox_, text, editorSize_, editorColor_);
        }
    } else if (text != editorOriginal_) {
        if (text.trimmed().isEmpty()) {
            emit eraseRequested(editorAnnot_);  // emptied: it goes
        } else {
            emit annotationTextRequested(editorAnnot_, text);
        }
    }
    editorAnnot_ = 0;
}

void PageView::cancelEditor() {
    if (editor_ != nullptr && editor_->isVisible()) {
        editor_->hide();
        setFocus();
    }
    editorAnnot_ = 0;
}

bool PageView::eventFilter(QObject* watched, QEvent* event) {
    if (fieldEditorEvent(watched, event)) {
        return true;
    }
    if (watched == editor_) {
        if (event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                cancelEditor();
                return true;
            }
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                (key->modifiers() & Qt::ControlModifier)) {
                commitEditor();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            commitEditor();
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void PageView::onDocumentEdited(QVector<int> pages, bool allPages, QVector<QSize> baseSizes) {
    if (!baseSizes.isEmpty() && baseSizes != baseSizes_) {
        baseSizes_ = baseSizes;  // a crop: the layout moves
        relayout();
    }
    const auto stale = [this](int p) {
        const auto it = rendered_.find(p);
        if (it != rendered_.end()) {
            it->zoom = -1.0;  // keep the image as a proxy, but ask for a new one
        }
        requested_.remove(p);
    };
    if (allPages) {
        for (auto it = rendered_.begin(); it != rendered_.end(); ++it) {
            stale(it.key());
        }
        requested_.clear();
    } else {
        for (const int p : pages) {
            stale(p);
        }
    }
    // Anything already in flight shows the document before the edit.
    bumpGeneration();
    editFloor_ = generation_;
    requestVisible();
    viewport()->update();
}

void PageView::copySelection() const {
    if (!selectionText_.isEmpty()) {
        QApplication::clipboard()->setText(selectionText_);
    }
}

void PageView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    int page = -1;
    QPointF base;
    viewportToPage(event->pos(), page, base);
    pressComment_ = 0;
    if (commentPopup() != nullptr) {
        // A click elsewhere closes the card and does nothing more, unless it
        // opens another comment.
        closePopup(true);
        const HoverTarget t = hoverTargetAt(event->pos());
        const bool opensAnother =
            t.annot != nullptr &&
            ((tool_ == Tool::Select && t.field == nullptr) ||
             (tool_ == Tool::Note && t.annot->type == QLatin1String("Text")));
        if (!opensAnother) {
            return;
        }
    }
    if (page < 0) {
        return;
    }
    switch (tool_) {
    case Tool::Note:
        // On a note already there: read it, rather than stack another on it.
        if (const HoverTarget t = hoverTargetAt(event->pos());
            t.annot != nullptr && t.annot->type == QLatin1String("Text")) {
            (void)showComment(t.annot->id, false);
            return;
        }
        emit noteRequested(page, base);
        return;
    case Tool::Erase:
        if (const AnnotRow* a = annotAt(page, base)) {
            emit eraseRequested(a->id);
        }
        return;
    case Tool::Move: {
        Grip grip = gripAt(event->pos());
        const AnnotRow* a = nullptr;
        if (grip == Grip::None || grip == Grip::Body) {
            a = annotAt(page, base);
            if (a == nullptr) {
                selectedAnnot_ = 0;
                viewport()->update();
                return;
            }
            if (!a->movable) {
                const bool markup = a->type == QStringLiteral("Highlight") ||
                                    a->type == QStringLiteral("Underline") ||
                                    a->type == QStringLiteral("StrikeOut") ||
                                    a->type == QStringLiteral("Squiggly");
                emit toolRefused(markup ? tr("Highlights follow their text: delete this one "
                                             "and highlight the text again instead.")
                                        : tr("This kind of annotation cannot be moved."));
                selectedAnnot_ = 0;
                viewport()->update();
                return;
            }
            selectedAnnot_ = a->id;
            grip = Grip::Body;
        } else {
            a = annotById(selectedAnnot_);
        }
        grip_ = grip;
        dragPage_ = a->page;
        moveFrom_ = moveTo_ = a->rect.normalized();
        grabBase_ = toBase(a->page, event->pos());
        viewport()->update();
        return;
    }
    case Tool::Text:
        if (const AnnotRow* a = annotAt(page, base);
            a != nullptr && a->type == QStringLiteral("FreeText")) {
            (void)editFreeText(a->id);
            return;
        }
        commitEditor();
        dragPage_ = page;
        dragStart_ = dragNow_ = base;
        viewport()->update();
        return;
    case Tool::Ink:
        dragPage_ = page;
        stroke_ = QPolygonF{base};
        viewport()->update();
        return;
    case Tool::Redact:
    case Tool::Sign:
    case Tool::Crop:
        dragPage_ = page;
        dragStart_ = dragNow_ = base;
        viewport()->update();
        return;
    case Tool::Stamp:
        emit stampRequested(page, base);
        return;
    case Tool::Select: {
        // A field first, then a comment: a click (not a drag) opens its card
        // on release, and a drag still selects the text under it.
        const HoverTarget t = hoverTargetAt(event->pos());
        if (t.field != nullptr) {
            clickField(*t.field, t.fieldWidget);
            return;
        }
        if (t.annot != nullptr) {
            pressComment_ = t.annot->id;
            pressPos_ = event->pos();
        }
        break;
    }
    case Tool::Highlight:
    case Tool::Underline:
    case Tool::StrikeOut:
        break;
    }
    selecting_ = true;
    selectAnchorPage_ = page;
    selectAnchorBase_ = base;
    selectionPage_ = -1;
    selectionBoxes_.clear();
    selectionText_.clear();
    viewport()->update();
}

void PageView::mouseMoveEvent(QMouseEvent* event) {
    int page = -1;
    QPointF base;
    viewportToPage(event->pos(), page, base);
    if (dragPage_ >= 0 && tool_ == Tool::Move && grip_ != Grip::None) {
        // A move may leave the page it is on; the worker keeps what it is given.
        const AnnotRow* a = annotById(selectedAnnot_);
        const bool keepAspect = a != nullptr && a->type == QStringLiteral("Stamp") &&
                                !(event->modifiers() & Qt::ShiftModifier);
        moveTo_ = dragged(toBase(dragPage_, event->pos()), keepAspect);
        viewport()->update();
        return;
    }
    if (dragPage_ >= 0) {
        // Ink and redaction stay on the page they started on.
        if (page != dragPage_) {
            return;
        }
        if (tool_ == Tool::Ink) {
            stroke_.push_back(base);
        } else {
            dragNow_ = base;
        }
        viewport()->update();
        return;
    }
    if (!selecting_) {
        if (!(event->buttons() & Qt::LeftButton)) {
            updateHoverCursor(event->pos());
        }
        return;
    }
    // Selection stays on the anchor page; clamp the far point to it.
    if (page != selectAnchorPage_) {
        return;
    }
    emitSelect(selectAnchorPage_, selectAnchorBase_, base, /*Chars=*/0);
}

void PageView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (dragPage_ >= 0 && tool_ == Tool::Move && grip_ != Grip::None) {
        dragPage_ = -1;
        grip_ = Grip::None;
        const QRectF d(moveTo_.topLeft() - moveFrom_.topLeft(),
                       moveTo_.bottomRight() - moveFrom_.bottomRight());
        if (std::abs(d.left()) + std::abs(d.top()) + std::abs(d.width()) +
                std::abs(d.height()) > 0.2) {
            for (AnnotRow& row : annotations_) {
                if (row.id == selectedAnnot_) {
                    row.rect = moveTo_;  // shown there at once; the worker confirms
                }
            }
            emit moveRequested(selectedAnnot_, moveTo_);
        }
        viewport()->update();
        return;
    }
    if (dragPage_ >= 0) {
        const int page = std::exchange(dragPage_, -1);
        if (tool_ == Tool::Ink && stroke_.size() >= 2) {
            emit inkRequested(page, {stroke_});
        } else if (tool_ == Tool::Text) {
            QRectF box = QRectF(dragStart_, dragNow_).normalized();
            if (box.width() < 12 || box.height() < 12) {
                box = QRectF(dragStart_, QSizeF(220, 12 * 2.4));  // a click: a line's worth
            }
            openEditor(page, box, 0, QString(), 12, newTextColor_);
        } else if (tool_ == Tool::Redact || tool_ == Tool::Sign || tool_ == Tool::Crop) {
            const QRectF box = QRectF(dragStart_, dragNow_).normalized();
            if (box.width() >= 2 && box.height() >= 2) {
                if (tool_ == Tool::Redact) {
                    emit redactRequested(page, box);
                } else if (tool_ == Tool::Crop) {
                    emit cropBoxRequested(page, box);
                } else {
                    emit signRequested(page, box);
                }
            }
        }
        stroke_.clear();
        viewport()->update();
        return;
    }
    if (selecting_ && markupTool()) {
        highlightWhenSettled_ = true;
        if (selectsReceived_ == selectsSent_) {
            finishHighlight();  // every reply is already in
        }
    }
    const int comment = std::exchange(pressComment_, 0);
    if (selecting_ && tool_ == Tool::Select && comment != 0 &&
        (event->pos() - pressPos_).manhattanLength() < 4) {
        // A click, not a drag: whatever the few pixels selected goes.
        selectionPage_ = -1;
        selectionBoxes_.clear();
        selectionText_.clear();
        (void)showComment(comment, false);
    }
    selecting_ = false;
}

void PageView::mouseDoubleClickEvent(QMouseEvent* event) {
    int page = -1;
    QPointF base;
    viewportToPage(event->pos(), page, base);
    if (page < 0) {
        return;
    }
    // Double-clicking text on the page edits it: free text in place, a note
    // in its card.
    if (tool_ == Tool::Select || tool_ == Tool::Move || tool_ == Tool::Text ||
        tool_ == Tool::Note) {
        if (const AnnotRow* a = annotAt(page, base)) {
            if (a->type == QStringLiteral("FreeText")) {
                selecting_ = false;
                (void)editFreeText(a->id);
                return;
            }
            if (a->type == QStringLiteral("Text")) {
                selecting_ = false;
                pressComment_ = 0;
                (void)showComment(a->id, true);
                return;
            }
        }
    }
    if (tool_ != Tool::Select && !markupTool()) {
        return;
    }
    // Word select: same point twice, Words mode.
    emitSelect(page, base, base, /*Words=*/1);
    if (markupTool()) {
        highlightWhenSettled_ = true;
    }
}

void PageView::markPageFailed(int page) {
    if (page < 0 || page >= baseSizes_.size()) {
        return;
    }
    failed_.insert(page);
    requested_.remove(page);
    viewport()->update();
}

void PageView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), palette().dark());

    if (baseSizes_.isEmpty()) {
        painter.setPen(palette().light().color());
        painter.drawText(viewport()->rect(), Qt::AlignCenter,
                         tr("Open a PDF to begin  (Ctrl+O)"));
        return;
    }

    const int scrollX = horizontalScrollBar()->value();
    const int scrollY = verticalScrollBar()->value();
    const int colW = columnWidth();
    const int top = scrollY;
    const int bottom = scrollY + viewport()->height();

    for (int p = 0; p < baseSizes_.size(); ++p) {
        const QSize size = scaledSize(p);
        const int y0 = pageTop(p);
        if (y0 + size.height() < top || y0 > bottom) {
            continue;  // not visible
        }

        // Centre each page in the column.
        const int x = kMargin + (colW - size.width()) / 2 - scrollX;
        const QRect pageRect(x, y0 - scrollY, size.width(), size.height());

        const auto it = rendered_.constFind(p);
        if (failed_.contains(p)) {
            painter.fillRect(pageRect, QColor(236, 236, 236));
            painter.setPen(QColor(90, 90, 90));
            painter.drawText(pageRect.adjusted(24, 24, -24, -24),
                             Qt::AlignCenter | Qt::TextWordWrap,
                             tr("This page could not be displayed safely.\n"
                                "It crashed or stalled the document parser; the rest "
                                "of the document is unaffected."));
        } else if (it != rendered_.constEnd() && !it->image.isNull()) {
            // Scale a stale-zoom image to the current page rect; Qt does this
            // fast, and it is replaced the moment the sharp render lands.
            painter.drawImage(pageRect, it->image);
        } else {
            painter.fillRect(pageRect, Qt::white);
        }
        painter.setPen(QColor(0, 0, 0, 60));
        painter.drawRect(pageRect);

        // Search matches on this page: translucent yellow, current one orange.
        const auto mit = matches_.constFind(p);
        if (mit != matches_.constEnd()) {
            for (int i = 0; i < mit->size(); ++i) {
                const QRectF box = baseRectToViewport(p, mit->at(i));
                const bool isCurrent =
                    currentMatch_ >= 0 &&
                    matchOrder_.value(currentMatch_) == qMakePair(p, i);
                painter.fillRect(box, isCurrent ? QColor(255, 150, 0, 160)
                                                : QColor(255, 235, 0, 110));
            }
        }

        // Selection on this page: translucent blue.
        if (selectionPage_ == p) {
            for (const QRectF& b : selectionBoxes_) {
                painter.fillRect(baseRectToViewport(p, b), QColor(60, 120, 220, 90));
            }
        }

        // Erase tool: outline what can be erased.
        if (tool_ == Tool::Erase) {
            painter.setPen(QPen(QColor(200, 40, 40), 1, Qt::DashLine));
            for (const AnnotRow& a : annotations_) {
                if (a.page == p) {
                    painter.drawRect(baseRectToViewport(p, a.rect.normalized()));
                }
            }
        }

        // Move tool: what can move, faintly; the selection, with its handles;
        // and during a drag, the annotation's own pixels where it is going.
        if (tool_ == Tool::Move) {
            for (const AnnotRow& a : annotations_) {
                if (a.page == p && a.movable && a.id != selectedAnnot_) {
                    painter.setPen(QPen(QColor(90, 90, 90, 120), 1, Qt::DotLine));
                    painter.drawRect(baseRectToViewport(p, a.rect.normalized()));
                }
            }
            const AnnotRow* sel = annotById(selectedAnnot_);
            if (sel != nullptr && sel->page == p) {
                const bool dragging = grip_ != Grip::None && dragPage_ == p;
                const QRectF shown =
                    baseRectToViewport(p, dragging ? moveTo_ : sel->rect.normalized());
                if (dragging && it != rendered_.constEnd() && !it->image.isNull() &&
                    it->zoom > 0) {
                    const QRectF source(moveFrom_.x() * it->zoom, moveFrom_.y() * it->zoom,
                                        moveFrom_.width() * it->zoom,
                                        moveFrom_.height() * it->zoom);
                    painter.setOpacity(0.85);
                    painter.drawImage(shown, it->image, source);
                    painter.setOpacity(1.0);
                    painter.setPen(QPen(QColor(90, 90, 90), 1, Qt::DashLine));
                    painter.drawRect(baseRectToViewport(p, moveFrom_));
                }
                painter.setPen(QPen(QColor(30, 90, 200), 1.5));
                painter.drawRect(shown);
                if (sel->resizable) {
                    painter.setBrush(Qt::white);
                    for (const QPointF& h :
                         {shown.topLeft(), shown.topRight(), shown.bottomLeft(),
                          shown.bottomRight(), QPointF(shown.center().x(), shown.top()),
                          QPointF(shown.center().x(), shown.bottom()),
                          QPointF(shown.left(), shown.center().y()),
                          QPointF(shown.right(), shown.center().y())}) {
                        painter.drawRect(QRectF(h - QPointF(3.5, 3.5), QSizeF(7, 7)));
                    }
                    painter.setBrush(Qt::NoBrush);
                }
            }
        }

        // A stroke or redaction box being drawn.
        if (dragPage_ == p && tool_ == Tool::Ink && stroke_.size() >= 2) {
            QPolygonF shown;
            const QRect pr = pageRectInViewport(p);
            for (const QPointF& pt : stroke_) {
                shown.push_back(QPointF(pr.x() + pt.x() * zoom_, pr.y() + pt.y() * zoom_));
            }
            painter.setPen(QPen(QColor(30, 60, 200), std::max(1.0, 1.5 * zoom_)));
            painter.drawPolyline(shown);
        }
        if (dragPage_ == p && (tool_ == Tool::Text || tool_ == Tool::Crop)) {
            const QRectF box = baseRectToViewport(p, QRectF(dragStart_, dragNow_).normalized());
            if (tool_ == Tool::Crop) {
                // What will be hidden is dimmed; what is kept stays clear.
                QPainterPath outside;
                outside.addRect(pageRect);
                QPainterPath kept;
                kept.addRect(box);
                painter.fillPath(outside.subtracted(kept), QColor(0, 0, 0, 70));
            }
            painter.setPen(QPen(QColor(40, 40, 40), 1, Qt::DashLine));
            painter.drawRect(box);
        }
        // Form fields: a light wash says where to type; the one being
        // filled in gets a frame.
        for (const FieldRow& f : fields_) {
            const bool current = f.name == currentField_;
            if (!fieldsShown_ && !(current && fieldEditor_ != nullptr)) {
                continue;  // hidden, except the one being typed in
            }
            for (const FieldWidget& w : f.widgets) {
                if (w.page != p) {
                    continue;
                }
                const QRectF box = baseRectToViewport(p, w.rect);
                painter.fillRect(box, f.readOnly ? QColor(128, 128, 128, 30) : QColor(40, 110, 230, current ? 45 : 28));
                if (current) {
                    painter.setPen(QPen(QColor(40, 110, 230), 2));
                    painter.drawRect(box.adjusted(-1, -1, 1, 1));
                } else if (f.required && !f.readOnly) {
                    painter.setPen(QPen(QColor(200, 60, 40, 160), 1));
                    painter.drawRect(box);
                }
            }
        }
        // Marked for redaction, not yet applied: a red frame over a light
        // wash, so the reader still sees what would go.
        for (const auto& [markPage, markBox] : redactionMarks_) {
            if (markPage != p) {
                continue;
            }
            const QRectF box = baseRectToViewport(p, markBox);
            painter.fillRect(box, QColor(200, 30, 30, 45));
            painter.setPen(QPen(QColor(200, 30, 30), 2, Qt::DashLine));
            painter.drawRect(box);
        }
        if (dragPage_ == p && (tool_ == Tool::Redact || tool_ == Tool::Sign)) {
            const QRectF box = baseRectToViewport(p, QRectF(dragStart_, dragNow_).normalized());
            const bool signing = tool_ == Tool::Sign;
            // A redaction is drawn as the mark it makes (red, to be reviewed
            // and applied); a signature box is only a frame.
            painter.fillRect(box, signing ? QColor(30, 90, 200, 40) : QColor(200, 30, 30, 45));
            painter.setPen(QPen(signing ? QColor(30, 90, 200) : QColor(200, 30, 30), 1, Qt::DashLine));
            painter.drawRect(box);
        }
    }

    const int page = currentPage();
    if (page != lastReportedPage_) {
        lastReportedPage_ = page;
        // What a screen reader says on focus: where the reader is.
        setAccessibleDescription(page >= 0 ? tr("Page %1 of %2").arg(page + 1).arg(pageCount())
                                           : tr("No document open"));
        emit currentPageChanged(page);
    }
}

bool PageView::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        // A comment's words on hover; nothing while a drag or an editor is on.
        const auto* help = static_cast<QHelpEvent*>(event);
        const bool busy = selecting_ || dragPage_ >= 0 || textEditor() != nullptr ||
                          fieldEditor_ != nullptr ||
                          (commentPopup() != nullptr && popup_->editing());
        const HoverTarget t = busy ? HoverTarget{} : hoverTargetAt(help->pos());
        const AnnotRow* a = t.field == nullptr ? t.annot : nullptr;
        if (a != nullptr && !(commentPopup() != nullptr && popup_->annotationId() == a->id)) {
            QToolTip::showText(help->globalPos(), commentToolTip(*a), viewport(),
                               baseRectToViewport(a->page, a->rect.normalized()).toAlignedRect());
        } else {
            QToolTip::hideText();
            event->ignore();
        }
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

bool PageView::event(QEvent* event) {
    // Moved to a screen with a different scale: every image is now the wrong
    // resolution. Asking again is enough -- none of them is "sharp" any more.
    if (event->type() == QEvent::DevicePixelRatioChange) {
        requestVisible();
        viewport()->update();
    }
    return QAbstractScrollArea::event(event);
}

void PageView::resizeEvent(QResizeEvent* /*event*/) {
    relayout();
    requestVisible();
}

void PageView::scrollContentsBy(int /*dx*/, int /*dy*/) {
    placeEditor();
    placePopup();
    placeFieldEditor();
    requestVisible();
    viewport()->update();
}

void PageView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const double steps = event->angleDelta().y() / 120.0;
        zoomBy(std::pow(1.15, steps));
        event->accept();
    } else {
        QAbstractScrollArea::wheelEvent(event);
    }
}

// --- Form fields on the page ----------------------------------------------

void PageView::clickField(const FieldRow& f, int widget) {
    // Under a certification that forbids filling in, or on a turned view the
    // editors cannot sit on, the Form panel is where the field is.
    if (!fieldsEditable_ || rotation_ != 0) {
        emit fieldClicked(f.name, false);
        return;
    }
    if (f.readOnly) {
        emit toolRefused(tr("This field cannot be changed."));
        return;
    }
    const QString name = f.name;
    if (typedIn(f)) {
        openFieldEditor(f, widget, /*popup=*/true);
        emit fieldClicked(name, true);
        return;
    }
    // A checkbox or radio button: the click is the value.
    commitEditor();
    commitFieldEditor(false);
    const FieldRow* row = fieldByName(name);  // the commits may have changed the list
    if (row == nullptr || widget >= row->widgets.size()) {
        return;
    }
    QString on = row->widgets[widget].onState;
    if (on.isEmpty()) {
        on = row->options.value(0, QStringLiteral("Yes"));
    }
    QString value = on;
    if (typeOf(*row) == FieldType::Checkbox && row->value == on) {
        value = QStringLiteral("Off");  // a checkbox unticks; a radio stays chosen
    }
    setCurrentField(name);
    emit fieldClicked(name, true);
    if (value != row->value) {
        // Shown at once, so a quick second click toggles back; the worker's
        // list confirms it.
        fields_[static_cast<int>(row - fields_.constData())].value = value;
        emit fieldValueRequested(name, value);
    }
}

void PageView::openFieldEditor(const FieldRow& f, int widget, bool popup) {
    const QString name = f.name;  // `f` may not outlive the commits
    commitEditor();  // one editor at a time, of any kind
    commitFieldEditor(false);
    closePopup(true);
    const FieldRow* row = fieldByName(name);
    if (row == nullptr || widget >= row->widgets.size()) {
        return;
    }
    fieldEditorName_ = row->name;
    fieldEditorWidget_ = widget;
    fieldEditorOriginal_ = row->value;
    const QString label = row->name.section(QLatin1Char('.'), -1);

    QWidget* editor = nullptr;
    if (typeOf(*row) == FieldType::Choice) {
        if (fieldCombo_ == nullptr) {
            auto* combo = new FieldCombo(viewport());
            combo->setObjectName(QStringLiteral("fieldCombo"));
            combo->setInsertPolicy(QComboBox::NoInsert);
            combo->installEventFilter(this);
            // A list closed with nothing chosen leaves the field as it was;
            // one closed on a choice sends it. Either way the editor goes,
            // after the combo has finished with the click.
            combo->popupHidden = [this, combo] {
                if (!combo->isEditable()) {
                    QTimer::singleShot(0, this, [this, combo] {
                        if (fieldEditor_ == combo) {
                            commitFieldEditor();
                        }
                    });
                }
            };
            connect(combo, &QComboBox::activated, this, [this, combo] {
                if (fieldEditor_ == combo && !combo->isEditable()) {
                    QTimer::singleShot(0, this, [this, combo] {
                        if (fieldEditor_ == combo) {
                            commitFieldEditor();
                        }
                    });
                }
            });
            fieldCombo_ = combo;
        }
        fieldCombo_->clear();
        fieldCombo_->setEditable(row->editableChoice);
        if (QLineEdit* typed = fieldCombo_->lineEdit()) {
            typed->installEventFilter(this);
        }
        fieldCombo_->addItems(row->options);
        const int at = fieldCombo_->findText(row->value);
        fieldCombo_->setCurrentIndex(at);
        if (at < 0 && row->editableChoice) {
            fieldCombo_->setEditText(row->value);
        }
        editor = fieldCombo_;
    } else if (row->multiline) {
        if (fieldText_ == nullptr) {
            fieldText_ = new QPlainTextEdit(viewport());
            fieldText_->setObjectName(QStringLiteral("fieldTextEditor"));
            fieldText_->installEventFilter(this);
            fieldText_->setFrameStyle(QFrame::Box | QFrame::Plain);
            fieldText_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
            fieldText_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        }
        fieldText_->setPlainText(row->value);
        fieldText_->moveCursor(QTextCursor::End);
        editor = fieldText_;
    } else {
        if (fieldLine_ == nullptr) {
            fieldLine_ = new QLineEdit(viewport());
            fieldLine_->setObjectName(QStringLiteral("fieldLineEditor"));
            fieldLine_->installEventFilter(this);
            fieldLine_->setFrame(false);
            fieldLine_->setTextMargins(2, 0, 2, 0);
        }
        fieldLine_->setMaxLength(row->maxLength > 0 ? row->maxLength : 32767);
        fieldLine_->setText(row->value);
        fieldLine_->selectAll();
        editor = fieldLine_;
    }
    // White and dark whatever the theme: it stands on the paper.
    QPalette pal = editor->palette();
    pal.setColor(QPalette::Base, Qt::white);
    pal.setColor(QPalette::Text, Qt::black);
    editor->setPalette(pal);
    editor->setAccessibleName(label);
    fieldEditor_ = editor;
    setCurrentField(fieldEditorName_);  // may scroll: before placing
    placeFieldEditor();
    editor->show();
    editor->raise();
    editor->setFocus(Qt::OtherFocusReason);
    if (popup && editor == fieldCombo_ && !fieldCombo_->isEditable()) {
        fieldCombo_->showPopup();
    }
    viewport()->update();
}

void PageView::placeFieldEditor() {
    if (fieldEditor_ == nullptr) {
        return;
    }
    const FieldRow* row = fieldByName(fieldEditorName_);
    if (row == nullptr || fieldEditorWidget_ >= row->widgets.size()) {
        return;
    }
    const FieldWidget& w = row->widgets[fieldEditorWidget_];
    if (w.page < 0 || w.page >= baseSizes_.size()) {
        return;
    }
    // The size a reader would expect in a field this tall; several lines of
    // a multi-line field at an ordinary size.
    const double points = row->multiline ? 10.0 : std::clamp(w.rect.height() * 0.65, 6.0, 12.0);
    QFont font(QStringLiteral("Helvetica"));
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(std::max(6, static_cast<int>(std::lround(points * zoom_))));
    fieldEditor_->setFont(font);
    QRect box = baseRectToViewport(w.page, w.rect).toAlignedRect();
    if (fieldEditor_ == fieldCombo_) {
        // Room for the arrow and the text, however small the field is drawn.
        const QSize least = fieldCombo_->minimumSizeHint();
        box.setWidth(std::max(box.width(), least.width()));
        box.setHeight(std::max(box.height(), least.height()));
    } else {
        box.setHeight(std::max(box.height(), QFontMetrics(font).height() + 2));
    }
    fieldEditor_->setGeometry(box);
}

void PageView::commitFieldEditor(bool refocus) {
    QWidget* editor = std::exchange(fieldEditor_, nullptr);  // hiding sends a focus-out
    if (editor == nullptr) {
        return;
    }
    QString value;
    if (editor == fieldLine_) {
        value = fieldLine_->text();
    } else if (editor == fieldText_) {
        value = fieldText_->toPlainText();
    } else {
        value = fieldCombo_->currentText();
    }
    const QString name = fieldEditorName_;
    editor->hide();
    if (refocus) {
        setFocus();
    }
    auto it = std::find_if(fields_.begin(), fields_.end(),
                           [&](const FieldRow& f) { return f.name == name; });
    if (it != fields_.end() && it->maxLength > 0 && value.size() > it->maxLength) {
        value.truncate(it->maxLength);  // a pasted multi-line value
    }
    if (it != fields_.end() && value != fieldEditorOriginal_ &&
        !(typeOf(*it) == FieldType::Choice && value.isEmpty())) {
        it->value = value;  // shown at once; the worker's list confirms it
        emit fieldValueRequested(name, value);
    }
    viewport()->update();
}

void PageView::cancelFieldEditor() {
    QWidget* editor = std::exchange(fieldEditor_, nullptr);
    if (editor != nullptr) {
        const bool had = editor->hasFocus();
        editor->hide();
        if (had) {
            setFocus();
        }
        viewport()->update();
    }
}

void PageView::moveFieldEditor(bool forward) {
    const QString from = fieldEditorName_;
    commitFieldEditor(false);
    const auto at = std::find_if(fields_.cbegin(), fields_.cend(),
                                 [&](const FieldRow& f) { return f.name == from; });
    const int n = static_cast<int>(fields_.size());
    const int start = at != fields_.cend() ? static_cast<int>(at - fields_.cbegin()) : (forward ? -1 : n);
    for (int step = 1; step <= n; ++step) {
        const int i = (((start + (forward ? step : -step)) % n) + n) % n;  // round the form
        const FieldRow& f = fields_.at(i);
        if (typedIn(f) && !f.readOnly) {
            const QString name = f.name;
            openFieldEditor(f, 0, /*popup=*/false);
            emit fieldClicked(name, true);
            return;
        }
    }
    setFocus();
}

bool PageView::fieldEditorEvent(QObject* watched, QEvent* event) {
    if (fieldEditor_ == nullptr) {
        return false;
    }
    const bool typed = fieldCombo_ != nullptr && watched == fieldCombo_->lineEdit();
    if (watched != fieldEditor_ && !(typed && fieldEditor_ == fieldCombo_)) {
        return false;
    }
    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Escape:
            cancelFieldEditor();
            return true;
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            moveFieldEditor(key->key() == Qt::Key_Tab && !(key->modifiers() & Qt::ShiftModifier));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // A new line in a multi-line field; Ctrl+Enter sends it.
            if (fieldEditor_ != fieldText_ || (key->modifiers() & Qt::ControlModifier)) {
                commitFieldEditor();
                return true;
            }
            break;
        default:
            break;
        }
    } else if (event->type() == QEvent::FocusOut) {
        // Not for a context menu or a combo's own list, nor while another
        // window is in front: the field is still being filled in.
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        if (reason != Qt::PopupFocusReason && reason != Qt::ActiveWindowFocusReason) {
            commitFieldEditor(false);
        }
    }
    return false;
}

void PageView::hideEvent(QHideEvent* event) {
    commitFieldEditor(false);  // another tab came to the front
    QAbstractScrollArea::hideEvent(event);
}
