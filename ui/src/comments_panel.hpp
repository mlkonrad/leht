// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QRectF>
#include <QVector>
#include <QWidget>

#include "edit_model.hpp"

class AnnotationProperties;
class QColor;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

/// The Comments tab: every annotation in the document, by page, with a filter.
/// Clicking one shows it on the page with its card open, and its colour,
/// opacity and author below the list; notes and text boxes can be edited (in
/// the card) or deleted from here. It edits nothing itself: it asks, and the window sends
/// the request to the worker like any other edit.
class CommentsPanel : public QWidget {
    Q_OBJECT

public:
    explicit CommentsPanel(QWidget* parent = nullptr);

    void setAnnotations(const QVector<AnnotRow>& rows);
    /// Whether edits are allowed (not in a certification that forbids them).
    void setEditable(bool editable);
    [[nodiscard]] int count() const { return static_cast<int>(rows_.size()); }

signals:
    void showRequested(int page, QRectF rect);
    /// Open comment `id`'s card on the page; with `edit`, ready to edit.
    void openRequested(int id, bool edit);
    void deleteRequested(int id);
    void styleRequested(int id, QColor color, double opacity, double lineWidth, double fontSize,
                        QString author);

private:
    void rebuild();
    [[nodiscard]] const AnnotRow* rowFor(const QTreeWidgetItem* item) const;

    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* empty_ = nullptr;
    AnnotationProperties* properties_ = nullptr;
    QVector<AnnotRow> rows_;
    bool editable_ = true;
};

/// A reader's name for an annotation type ("Highlight", "Note", "Text box").
[[nodiscard]] QString annotationTypeName(const QString& type);
/// The Lucide icon for it.
[[nodiscard]] QString annotationTypeIcon(const QString& type);
