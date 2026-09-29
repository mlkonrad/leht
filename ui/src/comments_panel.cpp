// SPDX-License-Identifier: AGPL-3.0-or-later
#include "comments_panel.hpp"

#include "annotation_properties.hpp"
#include "icons.hpp"

#include <QCoreApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

QString annotationTypeName(const QString& type) {
    // MuPDF's names (pdf_string_from_annot_type), as a reader would say them.
    static const QHash<QString, const char*> names = {
        {QStringLiteral("Text"), QT_TRANSLATE_NOOP("CommentsPanel", "Note")},
        {QStringLiteral("FreeText"), QT_TRANSLATE_NOOP("CommentsPanel", "Text box")},
        {QStringLiteral("Highlight"), QT_TRANSLATE_NOOP("CommentsPanel", "Highlight")},
        {QStringLiteral("Underline"), QT_TRANSLATE_NOOP("CommentsPanel", "Underline")},
        {QStringLiteral("StrikeOut"), QT_TRANSLATE_NOOP("CommentsPanel", "Strike-out")},
        {QStringLiteral("Squiggly"), QT_TRANSLATE_NOOP("CommentsPanel", "Squiggly underline")},
        {QStringLiteral("Ink"), QT_TRANSLATE_NOOP("CommentsPanel", "Drawing")},
        {QStringLiteral("Stamp"), QT_TRANSLATE_NOOP("CommentsPanel", "Stamp")},
        {QStringLiteral("Square"), QT_TRANSLATE_NOOP("CommentsPanel", "Rectangle")},
        {QStringLiteral("Circle"), QT_TRANSLATE_NOOP("CommentsPanel", "Ellipse")},
        {QStringLiteral("Line"), QT_TRANSLATE_NOOP("CommentsPanel", "Line")},
        {QStringLiteral("Polygon"), QT_TRANSLATE_NOOP("CommentsPanel", "Polygon")},
        {QStringLiteral("PolyLine"), QT_TRANSLATE_NOOP("CommentsPanel", "Lines")},
        {QStringLiteral("Caret"), QT_TRANSLATE_NOOP("CommentsPanel", "Insertion mark")},
        {QStringLiteral("FileAttachment"), QT_TRANSLATE_NOOP("CommentsPanel", "Attached file")},
        {QStringLiteral("Redact"), QT_TRANSLATE_NOOP("CommentsPanel", "Redaction mark")},
    };
    const auto it = names.constFind(type);
    return it == names.constEnd() ? type : QCoreApplication::translate("CommentsPanel", *it);
}

QString annotationTypeIcon(const QString& type) {
    static const QHash<QString, QString> icons = {
        {QStringLiteral("Text"), QStringLiteral("sticky-note")},
        {QStringLiteral("FreeText"), QStringLiteral("type")},
        {QStringLiteral("Highlight"), QStringLiteral("highlighter")},
        {QStringLiteral("Underline"), QStringLiteral("underline")},
        {QStringLiteral("Squiggly"), QStringLiteral("underline")},
        {QStringLiteral("StrikeOut"), QStringLiteral("strikethrough")},
        {QStringLiteral("Ink"), QStringLiteral("pen-line")},
        {QStringLiteral("Stamp"), QStringLiteral("stamp")},
        {QStringLiteral("Redact"), QStringLiteral("square-dashed")},
    };
    return icons.value(type, QStringLiteral("message-square"));
}

namespace {

/// Types that are part of the page's machinery rather than comments.
bool isComment(const QString& type) {
    return type != QLatin1String("Link") && type != QLatin1String("Widget") &&
           type != QLatin1String("Popup");
}

bool hasEditableText(const QString& type) {
    return type == QLatin1String("Text") || type == QLatin1String("FreeText");
}

}  // namespace

CommentsPanel::CommentsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("commentsPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 4);
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter comments"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("commentsTree"));
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(false);
    tree_->setIndentation(10);
    tree_->setWordWrap(true);
    tree_->setUniformRowHeights(false);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_, 1);
    empty_ = new QLabel(tr("No comments yet. Use the Comment tools to highlight text or add notes."),
                        this);
    empty_->setWordWrap(true);
    empty_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(empty_, 1);
    properties_ = new AnnotationProperties(this);
    layout->addWidget(properties_);
    connect(properties_, &AnnotationProperties::styleRequested, this,
            &CommentsPanel::styleRequested);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item) { properties_->setAnnotation(rowFor(item)); });

    connect(filter_, &QLineEdit::textChanged, this, &CommentsPanel::rebuild);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        if (const AnnotRow* row = rowFor(item)) {
            const int id = row->id;
            emit showRequested(row->page, row->rect);
            emit openRequested(id, false);
        }
    });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (const AnnotRow* row = rowFor(item)) {
            const int id = row->id;
            const bool edit = editable_ && hasEditableText(row->type);
            emit showRequested(row->page, row->rect);
            emit openRequested(id, edit);
        }
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        const AnnotRow* row = rowFor(tree_->itemAt(at));
        if (row == nullptr) {
            return;
        }
        const AnnotRow copy = *row;
        QMenu menu(this);
        menu.addAction(icons::named(QStringLiteral("eye")), tr("Show on Page"), this,
                       [this, copy] {
                           emit showRequested(copy.page, copy.rect);
                           emit openRequested(copy.id, false);
                       });
        if (hasEditableText(copy.type)) {
            QAction* edit = menu.addAction(icons::named(QStringLiteral("file-pen-line")), tr("Edit Text…"), this,
                                           [this, copy] {
                                               emit showRequested(copy.page, copy.rect);
                                               emit openRequested(copy.id, true);
                                           });
            edit->setEnabled(editable_);
        }
        menu.addSeparator();
        QAction* del = menu.addAction(icons::named(QStringLiteral("trash-2")), tr("Delete"), this,
                                      [this, copy] { emit deleteRequested(copy.id); });
        del->setEnabled(editable_);
        menu.exec(tree_->viewport()->mapToGlobal(at));
    });
}

void CommentsPanel::setAnnotations(const QVector<AnnotRow>& rows) {
    rows_.clear();
    for (const AnnotRow& r : rows) {
        if (isComment(r.type)) {
            rows_.push_back(r);
        }
    }
    std::stable_sort(rows_.begin(), rows_.end(), [](const AnnotRow& a, const AnnotRow& b) {
        if (a.page != b.page) {
            return a.page < b.page;
        }
        // Down the page, then across: reading order, near enough.
        return a.rect.top() < b.rect.top() || (a.rect.top() == b.rect.top() && a.rect.left() < b.rect.left());
    });
    rebuild();
}

void CommentsPanel::setEditable(bool editable) {
    editable_ = editable;
    properties_->setEditable(editable);
}

const AnnotRow* CommentsPanel::rowFor(const QTreeWidgetItem* item) const {
    if (item == nullptr || !item->data(0, Qt::UserRole).isValid()) {
        return nullptr;
    }
    const int index = item->data(0, Qt::UserRole).toInt();
    return index >= 0 && index < rows_.size() ? &rows_[index] : nullptr;
}

void CommentsPanel::rebuild() {
    // The chosen comment stays chosen across a refresh (after its own edit).
    const int chosen = properties_->annotationId();
    QTreeWidgetItem* again = nullptr;
    const QSignalBlocker quiet(tree_);
    tree_->clear();
    const QString needle = filter_->text().trimmed();
    QTreeWidgetItem* pageItem = nullptr;
    int lastPage = -1;
    int shown = 0;
    for (int i = 0; i < rows_.size(); ++i) {
        const AnnotRow& r = rows_[i];
        const QString typeName = annotationTypeName(r.type);
        if (!needle.isEmpty() && !r.contents.contains(needle, Qt::CaseInsensitive) &&
            !typeName.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        if (r.page != lastPage) {
            pageItem = new QTreeWidgetItem(tree_, {tr("Page %1").arg(r.page + 1)});
            QFont bold = pageItem->font(0);
            bold.setWeight(QFont::DemiBold);
            pageItem->setFont(0, bold);
            pageItem->setFlags(Qt::ItemIsEnabled);
            pageItem->setFirstColumnSpanned(true);
            lastPage = r.page;
        }
        QString text = r.contents.simplified();
        if (text.size() > 140) {
            text = text.left(139) + QChar(0x2026);
        }
        auto* item = new QTreeWidgetItem(pageItem, {text.isEmpty() ? typeName : text});
        // Read aloud as "Note on page 3: text", not just the text.
        item->setData(0, Qt::AccessibleTextRole,
                      text.isEmpty() ? tr("%1 on page %2").arg(typeName).arg(r.page + 1)
                                     : tr("%1 on page %2: %3").arg(typeName).arg(r.page + 1).arg(text));
        item->setIcon(0, icons::named(annotationTypeIcon(r.type)));
        item->setData(0, Qt::UserRole, i);
        if (r.id == chosen) {
            again = item;
        }
        item->setToolTip(0, text.isEmpty() ? typeName : typeName + QStringLiteral(": ") + r.contents);
        ++shown;
    }
    tree_->expandAll();
    tree_->setCurrentItem(again);
    properties_->setAnnotation(rowFor(again));
    empty_->setText(rows_.isEmpty() ? tr("No comments yet. Use the Comment tools to highlight text or add notes.")
                                    : tr("No comments match “%1”.").arg(needle));
    empty_->setVisible(shown == 0);
    tree_->setVisible(shown > 0);
    properties_->setVisible(shown > 0);
    filter_->setVisible(!rows_.isEmpty());
}
