// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sign_dialog.hpp"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStackedWidget>
#include <QVBoxLayout>

DrawPad::DrawPad(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::CrossCursor);
    setMinimumSize(200, 80);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::white);
    setPalette(pal);
}

void DrawPad::clear() {
    strokes_.clear();
    update();
}

void DrawPad::mousePressEvent(QMouseEvent* event) {
    drawing_ = true;
    strokes_.push_back(QPolygonF{event->position()});
    update();
}

void DrawPad::mouseMoveEvent(QMouseEvent* event) {
    if (drawing_ && !strokes_.isEmpty()) {
        strokes_.back().push_back(event->position());
        update();
    }
}

void DrawPad::mouseReleaseEvent(QMouseEvent* /*event*/) {
    drawing_ = false;
    // A click that never moved is a dot, not a stroke: drop it, so an
    // accidental click does not become part of the signature.
    if (!strokes_.isEmpty() && strokes_.back().size() < 2) {
        strokes_.pop_back();
        update();
    }
}

void DrawPad::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(120, 120, 120), 1, Qt::DashLine));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    painter.setPen(QPen(QColor(13, 26, 90), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const QPolygonF& stroke : strokes_) {
        painter.drawPolyline(stroke);
    }
    if (strokes_.isEmpty()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, tr("Draw your signature here"));
    }
}

SignDialog::SignDialog(QWidget* parent, int page, QRectF rect, QString suggestedField)
    : QDialog(parent), page_(page), rect_(rect), field_(std::move(suggestedField)) {
    setWindowTitle(tr("Sign document"));
    QSettings settings;

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    keyPath_ = new QLineEdit(settings.value(QStringLiteral("signing/lastKey")).toString(), this);
    keyPath_->setPlaceholderText(tr("a .p12 or .pfx file"));
    auto* browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &SignDialog::browseForKey);
    auto* keyRow = new QHBoxLayout;
    keyRow->addWidget(keyPath_);
    keyRow->addWidget(browse);
    form->addRow(tr("Key file:"), keyRow);

    password_ = new QLineEdit(this);
    password_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), password_);

    name_ = new QLineEdit(settings.value(QStringLiteral("signing/name")).toString(), this);
    name_->setPlaceholderText(tr("taken from the certificate when left empty"));
    form->addRow(tr("Name:"), name_);
    reason_ = new QLineEdit(this);
    form->addRow(tr("Reason:"), reason_);
    location_ = new QLineEdit(settings.value(QStringLiteral("signing/location")).toString(), this);
    form->addRow(tr("Location:"), location_);
    layout->addLayout(form);

    if (!rect_.isEmpty()) {
        auto* box = new QGroupBox(tr("What the signature shows"), this);
        auto* boxLayout = new QVBoxLayout(box);
        auto* choices = new QHBoxLayout;
        textOnly_ = new QRadioButton(tr("Name and date"), box);
        drawn_ = new QRadioButton(tr("Draw"), box);
        imported_ = new QRadioButton(tr("Image…"), box);
        textOnly_->setChecked(true);
        choices->addWidget(textOnly_);
        choices->addWidget(drawn_);
        choices->addWidget(imported_);
        choices->addStretch();
        boxLayout->addLayout(choices);

        appearance_ = new QStackedWidget(box);
        auto* nothing = new QLabel(tr("The signature box will show the name and the date."), box);
        nothing->setWordWrap(true);
        appearance_->addWidget(nothing);

        auto* drawPage = new QWidget(box);
        auto* drawLayout = new QVBoxLayout(drawPage);
        drawLayout->setContentsMargins(0, 0, 0, 0);
        pad_ = new DrawPad(drawPage);
        drawLayout->addWidget(pad_);
        auto* clear = new QPushButton(tr("Clear"), drawPage);
        connect(clear, &QPushButton::clicked, pad_, &DrawPad::clear);
        drawLayout->addWidget(clear, 0, Qt::AlignLeft);
        appearance_->addWidget(drawPage);

        auto* imagePage = new QWidget(box);
        auto* imageLayout = new QHBoxLayout(imagePage);
        imageLayout->setContentsMargins(0, 0, 0, 0);
        imageLabel_ = new QLabel(tr("No image chosen"), imagePage);
        auto* pick = new QPushButton(tr("Choose image…"), imagePage);
        connect(pick, &QPushButton::clicked, this, &SignDialog::browseForImage);
        imageLayout->addWidget(imageLabel_, 1);
        imageLayout->addWidget(pick);
        appearance_->addWidget(imagePage);
        boxLayout->addWidget(appearance_);

        connect(textOnly_, &QRadioButton::toggled, this, [this](bool on) {
            if (on) {
                appearance_->setCurrentIndex(0);
            }
        });
        connect(drawn_, &QRadioButton::toggled, this, [this](bool on) {
            if (on) {
                appearance_->setCurrentIndex(1);
            }
        });
        connect(imported_, &QRadioButton::toggled, this, [this](bool on) {
            if (on) {
                appearance_->setCurrentIndex(2);
            }
        });
        layout->addWidget(box);
    } else {
        auto* note = new QLabel(tr("This signature will be invisible: it protects the file "
                                   "without marking a page."), this);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    useTsa_ = new QCheckBox(tr("Timestamp the signature (PAdES B-T)"), this);
    useTsa_->setChecked(settings.value(QStringLiteral("signing/useTsa"), false).toBool());
    useTsa_->setToolTip(tr("Asks a timestamp authority over the network to attest when this "
                           "signature was made, which is what keeps it verifiable after the "
                           "certificate expires."));
    tsa_ = new QLineEdit(settings.value(QStringLiteral("signing/tsa")).toString(), this);
    tsa_->setPlaceholderText(tr("https://timestamp.example.org"));
    tsa_->setEnabled(useTsa_->isChecked());
    connect(useTsa_, &QCheckBox::toggled, tsa_, &QLineEdit::setEnabled);
    layout->addWidget(useTsa_);
    layout->addWidget(tsa_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Sign"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (keyPath_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Sign document"),
                                 tr("Choose the .p12 file holding your signing key."));
            return;
        }
        if (useTsa_->isChecked() && tsa_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Sign document"),
                                 tr("Give the timestamp authority's URL, or turn timestamping "
                                    "off."));
            return;
        }
        QSettings saved;
        saved.setValue(QStringLiteral("signing/lastKey"), keyPath_->text());
        saved.setValue(QStringLiteral("signing/name"), name_->text());
        saved.setValue(QStringLiteral("signing/location"), location_->text());
        saved.setValue(QStringLiteral("signing/useTsa"), useTsa_->isChecked());
        saved.setValue(QStringLiteral("signing/tsa"), tsa_->text());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SignDialog::browseForKey() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Signing key"), QFileInfo(keyPath_->text()).absolutePath(),
        tr("PKCS#12 keys (*.p12 *.pfx);;All files (*)"));
    if (!path.isEmpty()) {
        keyPath_->setText(path);
    }
}

void SignDialog::browseForImage() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Signature image"), QString(),
                                                      tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Signature image"),
                             tr("Cannot read %1: %2").arg(path, file.errorString()));
        return;
    }
    image_ = file.readAll();
    imageLabel_->setText(QFileInfo(path).fileName());
}

SignSpec SignDialog::spec() const {
    SignSpec spec;
    spec.p12Path = keyPath_->text().trimmed();
    spec.password = password_->text();
    spec.field = field_;
    spec.page = page_;
    spec.rect = rect_;
    spec.name = name_->text().trimmed();
    spec.reason = reason_->text().trimmed();
    spec.location = location_->text().trimmed();
    if (useTsa_->isChecked()) {
        spec.tsaUrl = tsa_->text().trimmed();
    }
    if (rect_.isEmpty()) {
        return spec;  // invisible: no appearance at all
    }
    if (drawn_->isChecked() && !pad_->isEmpty()) {
        spec.strokes = pad_->strokes();
        spec.strokesCanvas = QSizeF(pad_->width(), pad_->height());
    } else if (imported_->isChecked()) {
        spec.image = image_;
    }
    // The text beside the graphic. Left to the engine when nothing is chosen,
    // which fills in the name and the date itself.
    if (!spec.strokes.isEmpty() || !spec.image.isEmpty()) {
        if (!spec.name.isEmpty()) {
            spec.lines.push_back(spec.name);
        }
        spec.lines.push_back(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd")));
    }
    return spec;
}
