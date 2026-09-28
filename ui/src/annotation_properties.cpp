// SPDX-License-Identifier: AGPL-3.0-or-later
#include "annotation_properties.hpp"

#include "color_swatches.hpp"
#include "comments_panel.hpp"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

AnnotationProperties::AnnotationProperties(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("annotationProperties"));
    setAccessibleName(tr("Comment properties"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 6, 0, 0);
    title_ = new QLabel(this);
    QFont bold = title_->font();
    bold.setWeight(QFont::DemiBold);
    title_->setFont(bold);
    layout->addWidget(title_);
    note_ = new QLabel(this);
    note_->setWordWrap(true);
    layout->addWidget(note_);

    form_ = new QFormLayout;
    form_->setContentsMargins(0, 0, 0, 0);
    form_->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->addLayout(form_);

    color_ = new ColorSwatches(this);
    color_->setObjectName(QStringLiteral("propertyColor"));
    form_->addRow(tr("Colour:"), color_);

    auto* opacityRow = new QWidget(this);
    auto* opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacity_ = new QSlider(Qt::Horizontal, opacityRow);
    opacity_->setObjectName(QStringLiteral("propertyOpacity"));
    opacity_->setRange(10, 100);
    opacity_->setSingleStep(5);
    opacity_->setPageStep(10);
    opacity_->setAccessibleName(tr("Opacity"));
    opacityValue_ = new QLabel(opacityRow);
    opacityValue_->setMinimumWidth(opacityValue_->fontMetrics().horizontalAdvance(QStringLiteral("100 %")));
    opacityLayout->addWidget(opacity_, 1);
    opacityLayout->addWidget(opacityValue_);
    form_->addRow(tr("Opacity:"), opacityRow);

    lineWidth_ = new QDoubleSpinBox(this);
    lineWidth_->setObjectName(QStringLiteral("propertyLineWidth"));
    lineWidth_->setRange(0.25, 20);
    lineWidth_->setSingleStep(0.5);
    lineWidth_->setDecimals(2);
    lineWidth_->setSuffix(tr(" pt"));
    lineWidth_->setKeyboardTracking(false);
    form_->addRow(tr("Line width:"), lineWidth_);

    fontSize_ = new QDoubleSpinBox(this);
    fontSize_->setObjectName(QStringLiteral("propertyFontSize"));
    fontSize_->setRange(4, 144);
    fontSize_->setDecimals(1);
    fontSize_->setSuffix(tr(" pt"));
    fontSize_->setKeyboardTracking(false);
    form_->addRow(tr("Text size:"), fontSize_);

    author_ = new QLineEdit(this);
    author_->setObjectName(QStringLiteral("propertyAuthor"));
    author_->setPlaceholderText(tr("No name"));
    form_->addRow(tr("Author:"), author_);

    connect(color_, &ColorSwatches::colorChosen, this, [this] { request(); });
    connect(opacity_, &QSlider::valueChanged, this, [this](int v) {
        opacityValue_->setText(tr("%1 %").arg(v));
        // Dragging asks once, on release; the keyboard asks at each step.
        if (!opacity_->isSliderDown()) {
            request();
        }
    });
    connect(opacity_, &QSlider::sliderReleased, this, [this] { request(); });
    connect(lineWidth_, &QDoubleSpinBox::valueChanged, this, [this] { request(); });
    connect(fontSize_, &QDoubleSpinBox::valueChanged, this, [this] { request(); });
    connect(author_, &QLineEdit::editingFinished, this, [this] { request(); });
    setAnnotation(nullptr);
}

void AnnotationProperties::setAnnotation(const AnnotRow* row) {
    loading_ = true;
    row_ = row != nullptr ? *row : AnnotRow{};
    const bool some = row != nullptr;
    const QString type = annotationTypeName(row_.type);
    title_->setText(some ? tr("%1 on page %2").arg(type).arg(row_.page + 1) : QString());
    title_->setVisible(some);
    if (some) {
        color_->setColor(row_.color.isValid() ? row_.color : QColor(Qt::black));
        const int percent = static_cast<int>(std::lround(row_.opacity * 100));
        opacity_->setValue(percent);
        opacityValue_->setText(tr("%1 %").arg(opacity_->value()));
        lineWidth_->setValue(row_.lineWidth > 0 ? row_.lineWidth : 1.5);
        fontSize_->setValue(row_.fontSize > 0 ? row_.fontSize : 12);
        author_->setText(row_.author);
    }
    const bool free = row_.type == QLatin1String("FreeText");
    form_->setRowVisible(lineWidth_, some && row_.lineWidth > 0);
    form_->setRowVisible(fontSize_, some && free);
    form_->setRowVisible(color_, some);
    form_->setRowVisible(opacity_->parentWidget(), some);
    form_->setRowVisible(author_, some);
    note_->setText(!some                ? tr("Choose a comment to change its colour, opacity or author.")
                   : !row_.styleable    ? tr("A %1 keeps the look it was given.").arg(type.toLower())
                                        : QString());
    note_->setVisible(!note_->text().isEmpty());
    updateEnabled();
    loading_ = false;
}

void AnnotationProperties::setEditable(bool editable) {
    editable_ = editable;
    updateEnabled();
}

void AnnotationProperties::updateEnabled() {
    const bool on = editable_ && row_.id != 0 && row_.styleable;
    for (QWidget* w : {static_cast<QWidget*>(color_), static_cast<QWidget*>(opacity_),
                       static_cast<QWidget*>(lineWidth_), static_cast<QWidget*>(fontSize_),
                       static_cast<QWidget*>(author_)}) {
        w->setEnabled(on);
    }
}

void AnnotationProperties::request() {
    if (loading_ || row_.id == 0 || !row_.styleable || !editable_) {
        return;
    }
    const QColor color = color_->color();
    const double opacity = opacity_->value() / 100.0;
    const double lineWidth = row_.lineWidth > 0 ? lineWidth_->value() : 0;
    const double fontSize = row_.type == QLatin1String("FreeText") ? fontSize_->value() : 0;
    const QString author = author_->text().trimmed();
    const bool same = color.rgb() == row_.color.rgb() &&
                      std::abs(opacity - row_.opacity) < 0.005 &&
                      std::abs(lineWidth - row_.lineWidth) < 0.005 &&
                      std::abs(fontSize - row_.fontSize) < 0.05 && author == row_.author;
    if (same) {
        return;
    }
    // What it now is, so a second change before the list comes back is
    // measured against this one, not the old.
    row_.color = color;
    row_.opacity = opacity;
    row_.lineWidth = lineWidth;
    row_.fontSize = fontSize;
    row_.author = author;
    emit styleRequested(row_.id, color, opacity, lineWidth, fontSize, author);
}
