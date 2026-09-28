// SPDX-License-Identifier: AGPL-3.0-or-later
#include "page_view_accessible.hpp"

#include "page_view.hpp"

#include <QAccessibleWidget>
#include <QCoreApplication>
#include <QVariant>

#include <algorithm>

namespace {

constexpr const char* kTextProperty = "lehtPageText";
constexpr const char* kPageProperty = "lehtPageTextPage";

/// A read-only text: no selection of its own, a caret that only moves.
class PageViewAccessible : public QAccessibleWidget, public QAccessibleTextInterface {
public:
    explicit PageViewAccessible(PageView* view) : QAccessibleWidget(view, QAccessible::Document) {}

    void* interface_cast(QAccessible::InterfaceType t) override {
        if (t == QAccessible::TextInterface) {
            return static_cast<QAccessibleTextInterface*>(this);
        }
        return QAccessibleWidget::interface_cast(t);
    }

    QString text(QAccessible::Text t) const override {
        if (t == QAccessible::Description) {
            const int count = view()->pageCount();
            const int page = view()->currentPage();
            if (count == 0 || page < 0) {
                return QCoreApplication::translate("PageView", "No document open");
            }
            QString where = QCoreApplication::translate("PageView", "Page %1 of %2").arg(page + 1).arg(count);
            if (pageText().trimmed().isEmpty() && shownPage() == page) {
                where += QCoreApplication::translate("PageView", ", no text on it (a scan?)");
            }
            return where;
        }
        return QAccessibleWidget::text(t);
    }

    // QAccessibleTextInterface
    void selection(int, int* start, int* end) const override { *start = *end = 0; }
    int selectionCount() const override { return 0; }
    void addSelection(int, int) override {}
    void removeSelection(int) override {}
    void setSelection(int, int, int) override {}
    int cursorPosition() const override { return std::min(cursor_, characterCount()); }
    void setCursorPosition(int position) override {
        cursor_ = std::clamp(position, 0, characterCount());
    }
    QString text(int start, int end) const override {
        const QString all = pageText();
        start = std::clamp(start, 0, static_cast<int>(all.size()));
        end = std::clamp(end, start, static_cast<int>(all.size()));
        return all.mid(start, end - start);
    }
    int characterCount() const override { return static_cast<int>(pageText().size()); }
    // Where each letter is drawn is not known here; the page as a whole is.
    QRect characterRect(int) const override { return rect(); }
    int offsetAtPoint(const QPoint&) const override { return -1; }
    void scrollToSubstring(int, int) override {}
    QString attributes(int offset, int* start, int* end) const override {
        *start = offset;
        *end = offset;
        return {};
    }

private:
    PageView* view() const { return static_cast<PageView*>(widget()); }
    int shownPage() const {
        const QVariant p = view()->property(kPageProperty);
        return p.isValid() ? p.toInt() : -1;
    }
    QString pageText() const {
        return shownPage() == view()->currentPage() ? view()->property(kTextProperty).toString() : QString();
    }

    int cursor_ = 0;
};

QAccessibleInterface* factory(const QString& className, QObject* object) {
    if (className == QLatin1String("PageView") && object != nullptr && object->isWidgetType()) {
        return new PageViewAccessible(static_cast<PageView*>(object));
    }
    return nullptr;
}

}  // namespace

namespace pagereading {

void install() {
    static const bool once = [] {
        QAccessible::installFactory(factory);
        return true;
    }();
    (void)once;
}

void setPageText(PageView* view, int page, const QString& text) {
    const QString old = view->property(kPageProperty).toInt() == page && view->property(kPageProperty).isValid()
                            ? view->property(kTextProperty).toString()
                            : QString();
    view->setProperty(kPageProperty, page);
    view->setProperty(kTextProperty, text);
    if (QAccessible::isActive() && old != text) {
        QAccessibleTextUpdateEvent changed(view, 0, old, text);
        QAccessible::updateAccessibility(&changed);
    }
}

}  // namespace pagereading
