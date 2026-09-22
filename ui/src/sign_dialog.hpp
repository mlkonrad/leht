// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QPolygonF>
#include <QVector>
#include <QWidget>

#include "edit_model.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QStackedWidget;

/// A small canvas for drawing a signature with the mouse or a stylus. Strokes
/// come out in the widget's own coordinates; the caller passes its size along,
/// and the engine scales them into the signature box.
class DrawPad : public QWidget {
    Q_OBJECT

public:
    explicit DrawPad(QWidget* parent = nullptr);

    [[nodiscard]] QVector<QPolygonF> strokes() const { return strokes_; }
    [[nodiscard]] bool isEmpty() const { return strokes_.isEmpty(); }
    void clear();

    [[nodiscard]] QSize sizeHint() const override { return {340, 120}; }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<QPolygonF> strokes_;
    bool drawing_ = false;
};

/// Collects everything a signature needs: the key, what it should look like,
/// and the optional timestamp authority.
///
/// The password is held only until the request is handed to the engine, which
/// wipes it. Nothing here touches the PDF, and nothing here signs.
class SignDialog : public QDialog {
    Q_OBJECT

public:
    /// `rect` empty means an invisible signature: the appearance controls are
    /// then hidden, because there is nothing to show.
    SignDialog(QWidget* parent, int page, QRectF rect, QString suggestedField);

    /// The collected request. Only valid after exec() returned Accepted.
    [[nodiscard]] SignSpec spec() const;

private:
    void browseForKey();
    void browseForImage();

    int page_;
    QRectF rect_;
    QString field_;
    QByteArray image_;

    QLineEdit* keyPath_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* reason_ = nullptr;
    QLineEdit* location_ = nullptr;
    QLineEdit* tsa_ = nullptr;
    QCheckBox* useTsa_ = nullptr;
    QRadioButton* textOnly_ = nullptr;
    QRadioButton* drawn_ = nullptr;
    QRadioButton* imported_ = nullptr;
    QStackedWidget* appearance_ = nullptr;
    DrawPad* pad_ = nullptr;
    QLabel* imageLabel_ = nullptr;
};
