// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QFrame>
#include <QRectF>
#include <QString>

#include "edit_model.hpp"

class QLabel;
class QPlainTextEdit;
class QPushButton;

/// A small card over the page that shows one comment: its type and author,
/// its whole text, and Edit, Delete and Close. Edit turns the text editable
/// in place, with Save and Cancel (Ctrl+Enter saves). The PageView owns it,
/// places it next to the annotation and closes it; the card edits nothing
/// itself, it asks (textRequested, deleteRequested).
class CommentPopup : public QFrame {
    Q_OBJECT

public:
    explicit CommentPopup(QWidget* parent = nullptr);

    /// Shows `row`; with `edit`, straight in edit mode (if `editable`
    /// allows). Read-only when `editable` is false: Edit and Delete go.
    void showFor(const AnnotRow& row, bool editable, bool edit);
    /// The annotation changed under the card (a refresh after an edit): the
    /// text shown follows, unless it is being edited.
    void refresh(const AnnotRow& row);
    /// Closes the card. Unsaved words are saved if `keepEdits`, else dropped.
    void dismiss(bool keepEdits);
    /// Next to `anchor` (the annotation, in parent coordinates), on its right
    /// if there is room, else on its left; kept inside the parent while the
    /// annotation is in view.
    void place(const QRectF& anchor);

    [[nodiscard]] int annotationId() const { return id_; }
    [[nodiscard]] bool editing() const { return editing_; }
    /// The text box, for tests.
    [[nodiscard]] QPlainTextEdit* textEdit() const { return text_; }

signals:
    void textRequested(int annotId, QString text);
    void deleteRequested(int annotId);
    /// The card closed (Esc, Close, Delete, Save or the view).
    void closed();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setEditing(bool editing);
    void save();
    void updateHeader();

    QLabel* header_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* delete_ = nullptr;
    QPushButton* close_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* cancel_ = nullptr;
    AnnotRow row_;
    int id_ = 0;
    bool editable_ = false;
    bool editing_ = false;
};
