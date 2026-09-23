// SPDX-License-Identifier: AGPL-3.0-or-later
#include "page_dialogs.hpp"

#include "leht/edit.hpp"
#include "leht/error.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

bool checkPages(QWidget* parent, const QString& spec, int pageCount) {
    try {
        if (leht::page_set(spec.trimmed().toStdString(), pageCount).empty()) {
            throw leht::Error(0, "no page in that range");
        }
        return true;
    } catch (const leht::Error& e) {
        QMessageBox::warning(parent, parent->windowTitle(),
                             QObject::tr("Pages “%1”: %2").arg(spec, QString::fromUtf8(e.what())));
        return false;
    }
}

namespace {

QLineEdit* pagesField(QWidget* parent, const QString& value) {
    auto* field = new QLineEdit(value, parent);
    field->setPlaceholderText(QObject::tr("all pages; or e.g. 1-3,8"));
    return field;
}

void showColor(QPushButton* button, const QColor& color) {
    QPixmap swatch(28, 14);
    swatch.fill(color);
    button->setIcon(QIcon(swatch));
    button->setText(color.name());
}

}  // namespace

// --- Watermark ----------------------------------------------------------------

WatermarkDialog::WatermarkDialog(QWidget* parent, QImage page, QSizeF pageSize, int pageCount)
    : QDialog(parent),
      page_(std::move(page)),
      pageSize_(pageSize.isEmpty() ? QSizeF(612, 792) : pageSize),
      pageCount_(pageCount) {
    setWindowTitle(tr("Watermark"));
    QSettings settings;
    settings.beginGroup(QStringLiteral("watermark"));
    const leht::ops::WatermarkOptions defaults;

    auto* layout = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    auto* form = new QFormLayout;
    text_ = new QLineEdit(settings.value(QStringLiteral("text"), tr("DRAFT")).toString(), this);
    form->addRow(tr("Text:"), text_);
    pages_ = pagesField(this, settings.value(QStringLiteral("pages")).toString());
    form->addRow(tr("Pages:"), pages_);
    size_ = new QDoubleSpinBox(this);
    size_->setRange(0, 1000);
    size_->setSuffix(tr(" pt"));
    size_->setSpecialValueText(tr("Fit the page"));
    size_->setValue(settings.value(QStringLiteral("size"), defaults.font_size).toDouble());
    form->addRow(tr("Size:"), size_);
    opacity_ = new QSlider(Qt::Horizontal, this);
    opacity_->setRange(1, 100);
    opacity_->setValue(
        settings.value(QStringLiteral("opacity"), static_cast<int>(std::lround(defaults.opacity * 100))).toInt());
    form->addRow(tr("Opacity:"), opacity_);
    angle_ = new QDoubleSpinBox(this);
    angle_->setRange(-360, 360);
    angle_->setSuffix(QStringLiteral("°"));
    angle_->setValue(settings.value(QStringLiteral("angle"), defaults.angle).toDouble());
    form->addRow(tr("Angle:"), angle_);
    chosen_ = settings.value(QStringLiteral("color"),
                             QColor::fromRgbF(defaults.color[0], defaults.color[1],
                                              defaults.color[2]))
                  .value<QColor>();
    color_ = new QPushButton(this);
    showColor(color_, chosen_);
    connect(color_, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(chosen_, this, tr("Watermark colour"));
        if (c.isValid()) {
            chosen_ = c;
            showColor(color_, chosen_);
            updatePreview();
        }
    });
    form->addRow(tr("Colour:"), color_);
    under_ = new QCheckBox(tr("Beneath the page content"), this);
    under_->setToolTip(tr("Subtler, but a page with an opaque background -- any scan -- "
                          "hides it entirely."));
    under_->setChecked(settings.value(QStringLiteral("under"), false).toBool());
    form->addRow(QString(), under_);
    left->addLayout(form);
    auto* note = new QLabel(tr("Latin text only: it is drawn in the standard Helvetica, so no "
                               "font is added to the file."), this);
    note->setWordWrap(true);
    left->addWidget(note);
    left->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Add Watermark"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (text_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, windowTitle(), tr("Give the text to stamp."));
            return;
        }
        if (!checkPages(this, pages_->text(), pageCount_)) {
            return;
        }
        QSettings saved;
        saved.beginGroup(QStringLiteral("watermark"));
        saved.setValue(QStringLiteral("text"), text_->text());
        saved.setValue(QStringLiteral("pages"), pages_->text());
        saved.setValue(QStringLiteral("size"), size_->value());
        saved.setValue(QStringLiteral("opacity"), opacity_->value());
        saved.setValue(QStringLiteral("angle"), angle_->value());
        saved.setValue(QStringLiteral("color"), chosen_);
        saved.setValue(QStringLiteral("under"), under_->isChecked());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    left->addWidget(buttons);
    layout->addLayout(left, 1);

    preview_ = new QLabel(this);
    preview_->setFixedSize(220, 285);
    preview_->setAlignment(Qt::AlignCenter);
    layout->addWidget(preview_);

    connect(text_, &QLineEdit::textChanged, this, &WatermarkDialog::updatePreview);
    connect(size_, &QDoubleSpinBox::valueChanged, this, &WatermarkDialog::updatePreview);
    connect(opacity_, &QSlider::valueChanged, this, &WatermarkDialog::updatePreview);
    connect(angle_, &QDoubleSpinBox::valueChanged, this, &WatermarkDialog::updatePreview);
    connect(under_, &QCheckBox::toggled, this, &WatermarkDialog::updatePreview);
    updatePreview();
}

void WatermarkDialog::updatePreview() {
    // Drawn here, not by the engine, but by the engine's rules: the size in
    // points (or the fit: 80% of the longest line through the centre at this
    // angle, at most half the shorter side), the angle, colour and opacity.
    QImage shown(pageSize_.toSize(), QImage::Format_ARGB32_Premultiplied);
    shown.fill(Qt::white);
    shown = shown.scaled(preview_->size(), Qt::KeepAspectRatio);
    QPainter p(&shown);
    if (!page_.isNull()) {
        p.drawImage(shown.rect(), page_);
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const double scale = shown.width() / pageSize_.width();  // pixels per point
    const QString text = text_->text().isEmpty() ? QStringLiteral(" ") : text_->text();
    QFont font(QStringLiteral("Helvetica"));
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(1000);
    const double unitWidth = QFontMetricsF(font).horizontalAdvance(text) / 1000.0;
    double size = size_->value();
    if (size <= 0) {
        const double a = angle_->value() * M_PI / 180.0;
        const double w = pageSize_.width();
        const double h = pageSize_.height();
        const double halfW = std::abs(std::cos(a)) > 1e-4 ? w / 2 / std::abs(std::cos(a)) : 1e9;
        const double halfH = std::abs(std::sin(a)) > 1e-4 ? h / 2 / std::abs(std::sin(a)) : 1e9;
        size = std::min(0.8 * 2 * std::min(halfW, halfH) / std::max(unitWidth, 1e-3),
                        0.5 * std::min(w, h));
    }
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(size * scale))));
    QColor c = chosen_;
    c.setAlphaF(opacity_->value() / 100.0);
    p.setFont(font);
    p.setPen(c);
    p.translate(shown.width() / 2.0, shown.height() / 2.0);
    p.rotate(-angle_->value());
    const double textW = unitWidth * size * scale;
    p.drawText(QPointF(-textW / 2, 0.35 * size * scale), text);
    p.end();
    preview_->setPixmap(QPixmap::fromImage(shown));
}

leht::ops::WatermarkOptions WatermarkDialog::options() const {
    leht::ops::WatermarkOptions o;
    o.text = text_->text().toStdString();
    o.font_size = static_cast<float>(size_->value());
    o.opacity = static_cast<float>(opacity_->value() / 100.0);
    o.angle = static_cast<float>(angle_->value());
    o.color[0] = static_cast<float>(chosen_.redF());
    o.color[1] = static_cast<float>(chosen_.greenF());
    o.color[2] = static_cast<float>(chosen_.blueF());
    o.under = under_->isChecked();
    return o;
}

QString WatermarkDialog::pages() const { return pages_->text().trimmed(); }

// --- Crop margins ---------------------------------------------------------------

CropMarginsDialog::CropMarginsDialog(QWidget* parent, int pageCount)
    : QDialog(parent), pageCount_(pageCount) {
    setWindowTitle(tr("Crop margins"));
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(tr("Cropping hides content; it stays in the file. Use Redact to "
                               "remove it."), this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* form = new QFormLayout;
    same_ = new QRadioButton(tr("Every edge alike:"), this);
    auto* each = new QRadioButton(tr("Each edge:"), this);
    same_->setChecked(true);
    const auto spin = [this](double value) {
        auto* s = new QDoubleSpinBox(this);
        s->setRange(0, 5000);
        s->setSuffix(tr(" pt"));
        s->setValue(value);
        return s;
    };
    all_ = spin(36);
    form->addRow(same_, all_);
    form->addRow(each, new QWidget(this));
    const QString names[] = {tr("Left:"), tr("Top:"), tr("Right:"), tr("Bottom:")};
    for (int i = 0; i < 4; ++i) {
        edge_[i] = spin(36);
        edge_[i]->setEnabled(false);
        form->addRow(names[i], edge_[i]);
    }
    connect(same_, &QRadioButton::toggled, this, [this](bool on) {
        all_->setEnabled(on);
        for (QDoubleSpinBox* e : edge_) {
            e->setEnabled(!on);
        }
    });
    pages_ = pagesField(this, QString());
    form->addRow(tr("Pages:"), pages_);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Crop"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        const leht::ops::Margins m = margins();
        if (m.left + m.top + m.right + m.bottom <= 0) {
            QMessageBox::warning(this, windowTitle(), tr("Nothing to trim."));
            return;
        }
        if (checkPages(this, pages_->text(), pageCount_)) {
            accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

leht::ops::Margins CropMarginsDialog::margins() const {
    if (same_->isChecked()) {
        const auto v = static_cast<float>(all_->value());
        return {v, v, v, v};
    }
    return {static_cast<float>(edge_[0]->value()), static_cast<float>(edge_[1]->value()),
            static_cast<float>(edge_[2]->value()), static_cast<float>(edge_[3]->value())};
}

QString CropMarginsDialog::pages() const { return pages_->text().trimmed(); }

// --- Crop to a box ----------------------------------------------------------------

QString askCropPages(QWidget* parent, int page, int pageCount) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Crop to this box"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(QObject::tr("Everything outside the box is hidden: it stays in the "
                                        "file. Use Redact to remove it."), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* thisPage = new QRadioButton(QObject::tr("This page (%1)").arg(page + 1), &dialog);
    auto* allPages = new QRadioButton(QObject::tr("Every page"), &dialog);
    auto* somePages = new QRadioButton(QObject::tr("Pages:"), &dialog);
    auto* spec = pagesField(&dialog, QString());
    spec->setEnabled(false);
    QObject::connect(somePages, &QRadioButton::toggled, spec, &QLineEdit::setEnabled);
    thisPage->setChecked(true);
    layout->addWidget(thisPage);
    layout->addWidget(allPages);
    auto* row = new QHBoxLayout;
    row->addWidget(somePages);
    row->addWidget(spec, 1);
    layout->addLayout(row);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Crop"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (somePages->isChecked() &&
            (spec->text().trimmed().isEmpty() || !checkPages(&dialog, spec->text(), pageCount))) {
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return QString();
    }
    if (thisPage->isChecked()) {
        return QString::number(page + 1);
    }
    if (allPages->isChecked()) {
        return QStringLiteral("");  // empty, not null: every page
    }
    return spec->text().trimmed();
}
