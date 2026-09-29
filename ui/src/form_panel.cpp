// SPDX-License-Identifier: AGPL-3.0-or-later
#include "form_panel.hpp"

#include "leht/ops/forms.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

using leht::ops::FieldType;

namespace {

const char* const kFieldProperty = "lehtField";

FieldType typeOf(const FieldRow& row) {
    return static_cast<FieldType>(row.type);
}

/// Fields a reader fills in here. Push buttons do nothing in Leht, and a
/// signature field is filled by signing, so it is listed without an editor.
bool listed(const FieldRow& row) {
    return typeOf(row) != FieldType::PushButton && typeOf(row) != FieldType::Unknown;
}

QColor errorColour(const QWidget* w) {
    const bool dark = w->palette().color(QPalette::Window).lightness() < 128;
    return dark ? QColor(240, 120, 120) : QColor(180, 30, 30);
}

}  // namespace

FormPanel::FormPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("formPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll_ = scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    list_ = new QWidget(scroll);
    listLayout_ = new QVBoxLayout(list_);
    listLayout_->setContentsMargins(10, 8, 10, 8);
    listLayout_->setSpacing(2);
    scroll->setWidget(list_);
    layout->addWidget(scroll, 1);

    auto* footer = new QWidget(this);
    auto* fl = new QVBoxLayout(footer);
    fl->setContentsMargins(10, 6, 10, 8);
    summary_ = new QLabel(footer);
    summary_->setObjectName(QStringLiteral("formSummary"));
    summary_->setWordWrap(true);
    fl->addWidget(summary_);
    highlight_ = new QCheckBox(tr("Show fields on the page"), footer);
    highlight_->setObjectName(QStringLiteral("formHighlight"));
    highlight_->setChecked(true);
    connect(highlight_, &QCheckBox::toggled, this, &FormPanel::highlightChanged);
    fl->addWidget(highlight_);
    flatten_ = new QPushButton(tr("Flatten Form…"), footer);
    flatten_->setObjectName(QStringLiteral("formFlatten"));
    flatten_->setToolTip(tr("Make the filled-in values part of the page, so they can no longer be changed"));
    connect(flatten_, &QPushButton::clicked, this, &FormPanel::flattenRequested);
    fl->addWidget(flatten_, 0, Qt::AlignLeft);
    layout->addWidget(footer);
}

QString FormPanel::fieldLabel(const QString& name) {
    // "applicant.first_name" reads as "first name"; a name that is only
    // punctuation or digits is kept as it is.
    QString last = name.section(QLatin1Char('.'), -1);
    QString spaced = last;
    spaced.replace(QRegularExpression(QStringLiteral("[_\\-]+")), QStringLiteral(" "));
    spaced.replace(QRegularExpression(QStringLiteral("([a-z])([A-Z])")), QStringLiteral("\\1 \\2"));
    spaced = spaced.simplified();
    if (spaced.isEmpty()) {
        return name;
    }
    spaced[0] = spaced[0].toUpper();
    return spaced;
}

bool FormPanel::isEmptyValue(const FieldRow& row) {
    const FieldType t = typeOf(row);
    if (t == FieldType::Checkbox || t == FieldType::Radio) {
        return row.value.isEmpty() || row.value == QLatin1String("Off");
    }
    return row.value.trimmed().isEmpty();
}

bool FormPanel::sameShape(const QVector<FieldRow>& a, const QVector<FieldRow>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].type != b[i].type || a[i].options != b[i].options ||
            a[i].readOnly != b[i].readOnly || a[i].required != b[i].required ||
            a[i].maxLength != b[i].maxLength) {
            return false;
        }
    }
    return true;
}

void FormPanel::setFields(const QVector<FieldRow>& rows) {
    QVector<FieldRow> sorted;
    for (const FieldRow& r : rows) {
        if (listed(r)) {
            sorted.push_back(r);
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(), fieldReadsBefore);
    const bool same = sameShape(rows_, sorted);
    rows_ = std::move(sorted);
    if (same && !entries_.isEmpty()) {
        refreshValues();
    } else {
        rebuild();
    }
    updateSummary();
}

void FormPanel::rebuild() {
    // Everything in the list goes; the stretch at the end is added again.
    while (QLayoutItem* item = listLayout_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
    entries_.clear();
    byName_.clear();

    for (const FieldRow& row : rows_) {
        Entry e;
        e.row = row;
        auto* box = new QWidget(list_);
        e.box = box;
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 6, 0, 4);
        bl->setSpacing(3);

        auto* head = new QHBoxLayout;
        QString title = fieldLabel(row.name);
        if (row.required) {
            title += QStringLiteral(" *");
        }
        auto* label = new QLabel(title, box);
        label->setToolTip(row.required ? tr("%1 (required)").arg(row.name) : row.name);
        QFont f = label->font();
        f.setBold(true);
        label->setFont(f);
        head->addWidget(label, 1);
        auto* where = new QLabel(tr("page %1").arg(row.page + 1), box);
        where->setEnabled(false);  // the palette's quiet colour
        head->addWidget(where);
        bl->addLayout(head);

        const QString name = row.name;
        switch (typeOf(row)) {
        case FieldType::Text: {
            auto* edit = new QLineEdit(box);
            if (row.maxLength > 0) {
                edit->setMaxLength(row.maxLength);
                edit->setPlaceholderText(tr("At most %n character(s)", nullptr, row.maxLength));
            }
            connect(edit, &QLineEdit::editingFinished, this, [this, edit, name] {
                commit(name, edit->text());
            });
            e.editor = edit;
            break;
        }
        case FieldType::Checkbox:
            if (row.options.size() <= 1) {
                auto* check = new QCheckBox(tr("Yes"), box);
                const QString on = row.options.value(0, QStringLiteral("Yes"));
                connect(check, &QCheckBox::clicked, this, [this, name, on](bool checked) {
                    commit(name, checked ? on : QStringLiteral("Off"));
                });
                e.editor = check;
                break;
            }
            [[fallthrough]];  // a checkbox with several on-states picks like a list
        case FieldType::Choice: {
            auto* combo = new QComboBox(box);
            QStringList choices = row.options;
            if (typeOf(row) != FieldType::Choice) {
                choices.push_back(QStringLiteral("Off"));
            }
            combo->addItems(choices);
            connect(combo, &QComboBox::activated, this, [this, combo, name] {
                commit(name, combo->currentText());
            });
            e.editor = combo;
            break;
        }
        case FieldType::Radio: {
            auto* group = new QWidget(box);
            auto* gl = new QVBoxLayout(group);
            gl->setContentsMargins(0, 0, 0, 0);
            auto* buttons = new QButtonGroup(group);
            for (const QString& option : row.options) {
                auto* b = new QRadioButton(option, group);
                buttons->addButton(b);
                gl->addWidget(b);
                b->setProperty(kFieldProperty, name);
                b->installEventFilter(this);
                connect(b, &QRadioButton::clicked, this, [this, name, option] { commit(name, option); });
            }
            e.editor = group;
            break;
        }
        case FieldType::Signature: {
            auto* note = new QLabel(tr("Signature field: when you sign, choose it under Where."), box);
            note->setWordWrap(true);
            e.editor = note;
            break;
        }
        case FieldType::PushButton:
        case FieldType::Unknown:
            break;
        }
        if (e.editor != nullptr) {
            e.editor->setProperty(kFieldProperty, name);
            e.editor->setAccessibleName(fieldLabel(name));
            e.editor->installEventFilter(this);
            label->setBuddy(e.editor);
            bl->addWidget(e.editor);
        }
        e.error = new QLabel(box);
        e.error->setObjectName(QStringLiteral("formFieldError"));
        QPalette pal = e.error->palette();
        pal.setColor(QPalette::WindowText, errorColour(this));
        e.error->setPalette(pal);
        e.error->setWordWrap(true);
        e.error->hide();
        bl->addWidget(e.error);

        byName_.insert(name, static_cast<int>(entries_.size()));
        entries_.push_back(e);
        listLayout_->addWidget(box);
    }
    listLayout_->addStretch(1);
    for (Entry& e : entries_) {
        setEditorValue(e);
    }
    setEditable(editable_);
    markCurrent(std::exchange(current_, QString()));
}

void FormPanel::refreshValues() {
    for (int i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        e.row = rows_[i];
        setEditorValue(e);
        validate(e);
    }
}

void FormPanel::setEditorValue(Entry& e) {
    if (e.editor == nullptr) {
        return;
    }
    const QScopedValueRollback<bool> guard(updating_, true);
    const FieldRow& row = e.row;
    if (auto* edit = qobject_cast<QLineEdit*>(e.editor)) {
        // Not while it is being typed in: the list that comes back after an
        // edit elsewhere must not undo what is being typed here.
        if (!edit->hasFocus() || edit->text() == row.value) {
            edit->setText(row.value);
        }
    } else if (auto* check = qobject_cast<QCheckBox*>(e.editor)) {
        check->setChecked(!isEmptyValue(row));
    } else if (auto* combo = qobject_cast<QComboBox*>(e.editor)) {
        const int at = combo->findText(row.value);
        combo->setCurrentIndex(at);  // -1 shows nothing chosen
    } else if (typeOf(row) == FieldType::Radio) {
        for (QRadioButton* b : e.editor->findChildren<QRadioButton*>()) {
            b->setChecked(b->text() == row.value);
        }
    }
}

void FormPanel::setEditable(bool editable) {
    editable_ = editable;
    for (Entry& e : entries_) {
        if (e.editor != nullptr && typeOf(e.row) != FieldType::Signature) {
            e.editor->setEnabled(editable && !e.row.readOnly);
            if (e.row.readOnly) {
                e.editor->setToolTip(tr("This field cannot be changed"));
            }
        }
    }
    flatten_->setEnabled(editable && !entries_.isEmpty());
}

void FormPanel::commit(const QString& name, const QString& value) {
    if (updating_) {
        return;
    }
    const auto it = byName_.constFind(name);
    if (it == byName_.constEnd()) {
        return;
    }
    Entry& e = entries_[*it];
    e.touched = true;
    if (value == e.row.value) {
        validate(e);
        return;  // nothing changed: no edit, no undo step
    }
    e.row.value = value;  // shown at once; the worker's list confirms it
    validate(e);
    updateSummary();
    emit valueEdited(name, value);
}

void FormPanel::validate(Entry& e) {
    QString problem;
    if (e.touched && e.row.required && isEmptyValue(e.row)) {
        problem = tr("This field is required.");
    } else if (e.row.maxLength > 0 && e.row.value.size() > e.row.maxLength) {
        problem = tr("Too long: at most %n character(s).", nullptr, e.row.maxLength);
    }
    e.error->setText(problem);
    e.error->setVisible(!problem.isEmpty());
}

QStringList FormPanel::emptyRequired() const {
    QStringList names;
    for (const Entry& e : entries_) {
        if (e.row.required && !e.row.readOnly && typeOf(e.row) != FieldType::Signature &&
            isEmptyValue(e.row)) {
            names.push_back(e.row.name);
        }
    }
    return names;
}

void FormPanel::updateSummary() {
    int filled = 0;
    int fillable = 0;
    for (const Entry& e : entries_) {
        if (e.row.readOnly || typeOf(e.row) == FieldType::Signature) {
            continue;
        }
        ++fillable;
        filled += isEmptyValue(e.row) ? 0 : 1;
    }
    QString text = tr("%1 of %n field(s) filled in.", nullptr, fillable).arg(filled);
    if (const auto missing = emptyRequired().size(); missing > 0) {
        text += QLatin1Char(' ') + tr("%n required field(s) still empty (marked *).", nullptr,
                                     static_cast<int>(missing));
    }
    summary_->setText(text);
}

void FormPanel::focusField(const QString& name) {
    const auto it = byName_.constFind(name);
    if (it == byName_.constEnd()) {
        return;
    }
    QWidget* editor = entries_[*it].editor;
    if (editor == nullptr) {
        return;
    }
    // Said now, not left to the focus event: a window that is not active
    // gets that only when it is.
    markCurrent(name);
    emit currentFieldChanged(name);
    scroll_->ensureWidgetVisible(editor->parentWidget());
    if (typeOf(entries_[*it].row) == FieldType::Radio) {
        const auto buttons = editor->findChildren<QRadioButton*>();
        const auto checked = std::find_if(buttons.begin(), buttons.end(),
                                          [](const QRadioButton* b) { return b->isChecked(); });
        if (checked != buttons.end()) {
            (*checked)->setFocus(Qt::OtherFocusReason);
        } else if (!buttons.isEmpty()) {
            buttons.front()->setFocus(Qt::OtherFocusReason);
        }
        return;
    }
    editor->setFocus(Qt::OtherFocusReason);
    if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
        edit->selectAll();
    }
}

void FormPanel::revealField(const QString& name) {
    const auto it = byName_.constFind(name);
    if (it == byName_.constEnd()) {
        return;
    }
    markCurrent(name);
    scroll_->ensureWidgetVisible(entries_[*it].box);
}

void FormPanel::markCurrent(const QString& name) {
    const auto set = [this](const QString& which, bool on) {
        const auto it = byName_.constFind(which);
        if (it == byName_.constEnd()) {
            return;
        }
        // The palette's alternate base: a quiet band that follows the theme.
        QWidget* box = entries_[*it].box;
        box->setBackgroundRole(QPalette::AlternateBase);
        box->setAutoFillBackground(on);
    };
    set(current_, false);
    current_ = name;
    set(current_, true);
}

bool FormPanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::FocusIn) {
        const QString name = watched->property(kFieldProperty).toString();
        if (!name.isEmpty()) {
            markCurrent(name);
            emit currentFieldChanged(name);
        }
    } else if (event->type() == QEvent::FocusOut) {
        const auto it = byName_.constFind(watched->property(kFieldProperty).toString());
        if (it != byName_.constEnd()) {
            Entry& e = entries_[*it];
            e.touched = true;
            validate(e);
        }
    }
    return QWidget::eventFilter(watched, event);
}
