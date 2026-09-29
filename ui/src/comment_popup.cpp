// SPDX-License-Identifier: AGPL-3.0-or-later
#include "comment_popup.hpp"

#include "comments_panel.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

bool hasEditableText(const QString& type) {
    return type == QLatin1String("Text") || type == QLatin1String("FreeText");
}

}  // namespace

CommentPopup::CommentPopup(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("commentPopup"));
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Comment"));
    setFixedWidth(300);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    header_ = new QLabel(this);
    header_->setObjectName(QStringLiteral("commentPopupHeader"));
    header_->setTextFormat(Qt::PlainText);
    header_->setWordWrap(true);
    QFont bold = header_->font();
    bold.setWeight(QFont::DemiBold);
    header_->setFont(bold);
    layout->addWidget(header_);

    text_ = new QPlainTextEdit(this);
    text_->setObjectName(QStringLiteral("commentPopupText"));
    text_->setAccessibleName(tr("Comment text"));
    text_->setTabChangesFocus(true);
    text_->installEventFilter(this);
    layout->addWidget(text_);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    edit_ = new QPushButton(tr("Edit"), this);
    delete_ = new QPushButton(tr("Delete"), this);
    close_ = new QPushButton(tr("Close"), this);
    save_ = new QPushButton(tr("Save"), this);
    cancel_ = new QPushButton(tr("Cancel"), this);
    edit_->setObjectName(QStringLiteral("commentPopupEdit"));
    delete_->setObjectName(QStringLiteral("commentPopupDelete"));
    close_->setObjectName(QStringLiteral("commentPopupClose"));
    save_->setObjectName(QStringLiteral("commentPopupSave"));
    cancel_->setObjectName(QStringLiteral("commentPopupCancel"));
    save_->setToolTip(tr("Save (Ctrl+Enter)"));
    for (QPushButton* b : {edit_, delete_, save_, cancel_, close_}) {
        b->setAutoDefault(false);
        buttons->addWidget(b);
    }
    buttons->insertStretch(2);
    layout->addLayout(buttons);

    connect(edit_, &QPushButton::clicked, this, [this] { setEditing(true); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        text_->setPlainText(row_.contents);
        setEditing(false);
    });
    connect(save_, &QPushButton::clicked, this, &CommentPopup::save);
    connect(close_, &QPushButton::clicked, this, [this] { dismiss(false); });
    connect(delete_, &QPushButton::clicked, this, [this] {
        const int id = id_;
        dismiss(false);
        emit deleteRequested(id);
    });
    hide();
}

void CommentPopup::showFor(const AnnotRow& row, bool editable, bool edit) {
    row_ = row;
    id_ = row.id;
    editable_ = editable;
    updateHeader();
    text_->setPlainText(row.contents);
    setEditing(edit && editable_ && hasEditableText(row.type));
    show();
    raise();
    if (!editing_) {
        text_->setFocus();  // read and copied from; Esc closes
    }
}

void CommentPopup::refresh(const AnnotRow& row) {
    if (row.id != id_) {
        return;
    }
    row_ = row;
    updateHeader();
    if (!editing_ && text_->toPlainText() != row.contents) {
        text_->setPlainText(row.contents);
    }
}

void CommentPopup::updateHeader() {
    const QString type = annotationTypeName(row_.type);
    header_->setText(row_.author.trimmed().isEmpty()
                         ? type
                         : tr("%1 — %2").arg(type, row_.author.trimmed()));
    setAccessibleDescription(header_->text());
}

void CommentPopup::setEditing(bool editing) {
    editing_ = editing;
    const bool canEdit = editable_ && hasEditableText(row_.type);
    text_->setReadOnly(!editing);
    edit_->setVisible(!editing && canEdit);
    delete_->setVisible(!editing && editable_);
    close_->setVisible(!editing);
    save_->setVisible(editing);
    cancel_->setVisible(editing);
    // Long text scrolls rather than growing the card past a third of a page.
    const int lines = std::clamp(text_->document()->lineCount() + (editing ? 2 : 0), 3, 12);
    text_->setFixedHeight(text_->fontMetrics().lineSpacing() * lines + 12);
    adjustSize();
    if (editing) {
        text_->setFocus();
        text_->moveCursor(QTextCursor::End);
    }
}

void CommentPopup::save() {
    const QString text = text_->toPlainText();
    const int id = id_;
    const bool changed = text != row_.contents;
    dismiss(false);
    if (!changed) {
        return;
    }
    if (text.trimmed().isEmpty()) {
        emit deleteRequested(id);  // emptied: it goes
    } else {
        emit textRequested(id, text);
    }
}

void CommentPopup::dismiss(bool keepEdits) {
    if (!isVisible() && id_ == 0) {
        return;
    }
    if (keepEdits && editing_) {
        save();  // closes again, with keepEdits false
        return;
    }
    // Focus goes back to the page (the viewport's scroll area, which takes the
    // keys), not on to whatever widget comes next.
    if (isAncestorOf(QApplication::focusWidget()) && parentWidget() != nullptr) {
        QWidget* page = parentWidget()->parentWidget();
        (page != nullptr ? page : parentWidget())->setFocus();
    }
    editing_ = false;
    id_ = 0;
    hide();
    emit closed();
}

void CommentPopup::place(const QRectF& anchor) {
    const QWidget* area = parentWidget();
    if (area == nullptr) {
        return;
    }
    constexpr int kGapPx = 8;
    const QRect a = anchor.toAlignedRect();
    const QSize s = sizeHint().expandedTo(minimumSizeHint()).boundedTo(QSize(width(), 10000));
    int x = a.right() + kGapPx;
    if (x + s.width() > area->width() - 4 && a.left() - kGapPx - s.width() >= 4) {
        x = a.left() - kGapPx - s.width();  // no room on the right: flip
    }
    x = std::clamp(x, 4, std::max(4, area->width() - s.width() - 4));
    int y = a.top();
    if (a.bottom() >= 0 && a.top() <= area->height()) {
        // In view: keep the whole card inside too.
        y = std::clamp(y, 4, std::max(4, area->height() - s.height() - 4));
    }
    setGeometry(x, y, width(), s.height());
}

void CommentPopup::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        dismiss(false);
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

bool CommentPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == text_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            dismiss(false);
            return true;
        }
        if (editing_ && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            (key->modifiers() & Qt::ControlModifier)) {
            save();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}
