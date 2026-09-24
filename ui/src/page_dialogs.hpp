// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/ops/crop.hpp"
#include "leht/ops/watermark.hpp"

#include <QDialog>
#include <QImage>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;

/// Everything `leht watermark` can do: text, pages, size, opacity, angle,
/// colour, under or over -- with a preview on the current page.
class WatermarkDialog : public QDialog {
    Q_OBJECT

public:
    /// `page` is the current page as rendered, `pageSize` its size in points,
    /// for the preview; `pageCount` checks the page range.
    WatermarkDialog(QWidget* parent, QImage page, QSizeF pageSize, int pageCount);

    [[nodiscard]] leht::ops::WatermarkOptions options() const;
    /// A page-range spec; empty means every page.
    [[nodiscard]] QString pages() const;

private:
    void updatePreview();

    QImage page_;
    QSizeF pageSize_;
    int pageCount_;
    QLineEdit* text_ = nullptr;
    QLineEdit* pages_ = nullptr;
    QDoubleSpinBox* size_ = nullptr;
    QSlider* opacity_ = nullptr;
    QDoubleSpinBox* angle_ = nullptr;
    QPushButton* color_ = nullptr;
    QColor chosen_;
    QCheckBox* under_ = nullptr;
    QLabel* preview_ = nullptr;
};

/// Crop Margins: every edge alike, or each on its own, on some pages.
class CropMarginsDialog : public QDialog {
    Q_OBJECT

public:
    CropMarginsDialog(QWidget* parent, int pageCount);

    [[nodiscard]] leht::ops::Margins margins() const;
    [[nodiscard]] QString pages() const;

private:
    int pageCount_;
    QRadioButton* same_ = nullptr;
    QDoubleSpinBox* all_ = nullptr;
    QDoubleSpinBox* edge_[4] = {};  ///< left, top, right, bottom
    QLineEdit* pages_ = nullptr;
};

/// Recognize Text (OCR): which languages, which pages, how finely.
class OcrDialog : public QDialog {
    Q_OBJECT

public:
    /// `installed` are the language codes Tesseract has data for.
    OcrDialog(QWidget* parent, int pageCount, const QStringList& installed);

    /// Tesseract's syntax: "est+eng".
    [[nodiscard]] QString languages() const;
    [[nodiscard]] QString pages() const;
    [[nodiscard]] bool skipPagesWithText() const;
    [[nodiscard]] int dpi() const;

private:
    int pageCount_;
    QVector<QCheckBox*> languages_;
    QLineEdit* pages_ = nullptr;
    QCheckBox* skip_ = nullptr;
    QSpinBox* dpi_ = nullptr;
};

/// After a box is dragged with the Crop tool: which pages to crop to it.
/// Returns a page-range spec ("3", "" for all, or what was typed), or a null
/// QString when cancelled.
QString askCropPages(QWidget* parent, int page, int pageCount);

/// True if `spec` is a page range valid for `pageCount` pages (empty is: all).
/// Otherwise shows why and returns false.
bool checkPages(QWidget* parent, const QString& spec, int pageCount);
