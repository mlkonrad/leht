// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QPolygonF>
#include <QVector>
#include <QWidget>

#include "edit_model.hpp"

#include "leht/crypto/crypto.hpp"

#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QRadioButton;
class QPushButton;
class QStackedWidget;

/// What the signature box will look like: the drawing or picture on the
/// left, the lines of text beside it, in the box's own proportions.
class SignaturePreview : public QWidget {
    Q_OBJECT

public:
    explicit SignaturePreview(QWidget* parent = nullptr);
    void show(QSizeF box, const QVector<QPolygonF>& strokes, QSizeF canvas, const QImage& image,
              const QStringList& lines);
    [[nodiscard]] QSize sizeHint() const override { return {360, 110}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QSizeF box_;
    QVector<QPolygonF> strokes_;
    QSizeF canvas_;
    QImage image_;
    QStringList lines_;
};

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

signals:
    /// A stroke was finished, or the pad cleared.
    void changed();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<QPolygonF> strokes_;
    bool drawing_ = false;
};

/// Collects everything a signature needs, in steps: 1 Where (the box drawn on
/// the page, an empty signature field of the form, or invisible), 2 How (the
/// key: a file or a card), 3 Look
/// (what a visible signature shows, with a preview; skipped for an invisible
/// one), 4 Details (name, reason, location, certification, timestamp). Next
/// checks a step before moving on.
///
/// The password is held only until the request is handed to the engine, which
/// wipes it. Nothing here touches the PDF, and nothing here signs.
class SignDialog : public QDialog {
    Q_OBJECT

public:
    /// exec()'s answer when the person chose to draw a box on the page first:
    /// the window then switches to the Sign tool, and the box drawn opens
    /// this dialog again.
    static constexpr int PlaceBox = 2;

    /// `rect`: the box drawn with the Sign tool on `page`; empty when the
    /// dialog was opened from a menu. `emptyFields`: the document's unsigned
    /// signature fields, offered as places to sign (`suggestedField` is
    /// chosen at first, if it is one of them). `canCertify`: the document has
    /// no signature yet, so this one may be its certification.
    SignDialog(QWidget* parent, int page, QRectF rect, QString suggestedField,
               bool canCertify = true, QVector<FieldRow> emptyFields = {});

    /// The collected request. Only valid after exec() returned Accepted.
    [[nodiscard]] SignSpec spec() const;

private:
    enum Step { Where, How, Look, Details };
    /// Step `index` (a Step): the pages, the header and the buttons follow.
    void goToStep(int index);
    /// The step after (`delta` 1) or before (-1) `from`, past Look when the
    /// signature is invisible.
    [[nodiscard]] int stepFrom(int from, int delta) const;
    /// What the Where step chose: the page, the box (empty: invisible) and
    /// the field (empty: a new one).
    [[nodiscard]] int chosenPage() const;
    [[nodiscard]] QRectF chosenRect() const;
    [[nodiscard]] QString chosenField() const;
    void showPlace();
    /// The key step's checks; false after telling the user what is missing.
    bool keyIsReady();
    void updatePreview();
    void browseForKey();
    void browseForImage();
    void showSource();
    /// Asks the system's PKCS#11 modules which keys the cards in the readers
    /// hold. Blocks for as long as the readers take, under a busy cursor.
    void refreshCardKeys();
    void showCardKey();

    int page_;
    QRectF rect_;
    QVector<FieldRow> emptyFields_;
    QByteArray image_;

    QFormLayout* form_ = nullptr;
    QRadioButton* fromFile_ = nullptr;
    QRadioButton* fromCard_ = nullptr;
    QWidget* keyFileRow_ = nullptr;
    QLineEdit* keyPath_ = nullptr;
    QLineEdit* password_ = nullptr;
    QWidget* cardRow_ = nullptr;
    QComboBox* cardKeys_ = nullptr;
    QLineEdit* pin_ = nullptr;
    QLabel* cardStatus_ = nullptr;
    std::vector<leht::crypto::TokenKey> tokenKeys_;
    bool cardKeysLoaded_ = false;
    QLineEdit* name_ = nullptr;
    QLineEdit* reason_ = nullptr;
    QLineEdit* location_ = nullptr;
    QLineEdit* tsa_ = nullptr;
    QCheckBox* useTsa_ = nullptr;
    QCheckBox* ltv_ = nullptr;
    QComboBox* certify_ = nullptr;
    QRadioButton* textOnly_ = nullptr;
    QRadioButton* drawn_ = nullptr;
    QRadioButton* imported_ = nullptr;
    QStackedWidget* appearance_ = nullptr;
    DrawPad* pad_ = nullptr;
    QLabel* imageLabel_ = nullptr;
    QRadioButton* placeDrawn_ = nullptr;
    QRadioButton* placeField_ = nullptr;
    QComboBox* fields_ = nullptr;
    QRadioButton* placeNewBox_ = nullptr;
    QRadioButton* placeInvisible_ = nullptr;
    QStackedWidget* steps_ = nullptr;
    QVector<QLabel*> stepLabels_;
    QVector<QLabel*> stepArrows_;
    QPushButton* back_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton* sign_ = nullptr;
    SignaturePreview* preview_ = nullptr;
};
