// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <QWidget>

#include "edit_model.hpp"

class QCheckBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

/// The Form tab: every field as a labelled editor, in reading order (page,
/// then top to bottom), so Tab and Shift+Tab walk the form the way the eye
/// does. Text is sent when the field is left, a choice when it is made: one
/// undo step per field.
///
/// Like the Comments tab it edits nothing itself: it asks, and the window
/// sends the request to the worker. When the worker's list comes back after
/// an edit, the editors are updated in place, so the field being typed in
/// keeps its focus.
class FormPanel : public QWidget {
    Q_OBJECT

public:
    explicit FormPanel(QWidget* parent = nullptr);

    void setFields(const QVector<FieldRow>& rows);
    /// Whether values may be changed (not in a certification that forbids it).
    void setEditable(bool editable);
    /// Puts the keyboard in field `name`'s editor (a click on the page does).
    void focusField(const QString& name);
    [[nodiscard]] int count() const { return static_cast<int>(rows_.size()); }
    /// Required fields still without a value, in reading order.
    [[nodiscard]] QStringList emptyRequired() const;
    /// A reader's label for a field: the last part of its name, spaced out.
    [[nodiscard]] static QString fieldLabel(const QString& name);

signals:
    void valueEdited(const QString& name, const QString& value);
    /// The keyboard moved into field `name`'s editor.
    void currentFieldChanged(const QString& name);
    void highlightChanged(bool on);
    void flattenRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry {
        FieldRow row;
        QWidget* editor = nullptr;
        QLabel* error = nullptr;
        bool touched = false;  ///< left once: from then on its problems show
    };

    void rebuild();
    /// Shows the new values without replacing the editors.
    void refreshValues();
    void setEditorValue(Entry& e);
    void commit(const QString& name, const QString& value);
    void validate(Entry& e);
    void updateSummary();
    [[nodiscard]] static bool sameShape(const QVector<FieldRow>& a, const QVector<FieldRow>& b);
    [[nodiscard]] static bool isEmptyValue(const FieldRow& row);

    QVector<FieldRow> rows_;
    QVector<Entry> entries_;
    QHash<QString, int> byName_;
    QScrollArea* scroll_ = nullptr;
    QWidget* list_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
    QLabel* summary_ = nullptr;
    QCheckBox* highlight_ = nullptr;
    QPushButton* flatten_ = nullptr;
    bool editable_ = true;
    bool updating_ = false;
};
