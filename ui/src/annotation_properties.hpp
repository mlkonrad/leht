// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QColor>
#include <QWidget>

#include "edit_model.hpp"

class ColorSwatches;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QSlider;

/// How the chosen comment looks, and who wrote it: colour, opacity, line
/// width (drawings and shapes), text size (text boxes) and author. Each change
/// asks for one edit, so Undo takes back one change at a time. Stamps and the
/// like keep the look they have, and say so.
class AnnotationProperties : public QWidget {
    Q_OBJECT

public:
    explicit AnnotationProperties(QWidget* parent = nullptr);

    /// Shows `row`; a null pointer shows nothing chosen.
    void setAnnotation(const AnnotRow* row);
    [[nodiscard]] int annotationId() const { return row_.id; }
    void setEditable(bool editable);

signals:
    void styleRequested(int id, QColor color, double opacity, double lineWidth, double fontSize,
                        QString author);

private:
    void request();
    void updateEnabled();

    QLabel* title_ = nullptr;
    QLabel* note_ = nullptr;
    QFormLayout* form_ = nullptr;
    ColorSwatches* color_ = nullptr;
    QSlider* opacity_ = nullptr;
    QLabel* opacityValue_ = nullptr;
    QDoubleSpinBox* lineWidth_ = nullptr;
    QDoubleSpinBox* fontSize_ = nullptr;
    QLineEdit* author_ = nullptr;
    AnnotRow row_;
    bool editable_ = true;
    bool loading_ = false;
};
