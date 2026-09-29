// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sign_dialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QImage>
#include <QHBoxLayout>
#include <QDate>
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

#include <algorithm>

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
    emit changed();
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
    emit changed();
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

SignaturePreview::SignaturePreview(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("signaturePreview"));
    setMinimumHeight(80);
    setAccessibleName(tr("Preview of the signature"));
}

void SignaturePreview::show(QSizeF box, const QVector<QPolygonF>& strokes, QSizeF canvas, const QImage& image,
                            const QStringList& lines) {
    box_ = box;
    strokes_ = strokes;
    canvas_ = canvas;
    image_ = image;
    lines_ = lines;
    setAccessibleDescription(lines.join(QStringLiteral(", ")));
    update();
}

void SignaturePreview::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // The box as the page will have it: its proportions, fitted here.
    const QSizeF want = box_.isEmpty() ? QSizeF(3, 1) : box_;
    const QSizeF fitted = want.scaled(QSizeF(size()) - QSizeF(8, 8), Qt::KeepAspectRatio);
    const QRectF frame(QPointF((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
    p.fillRect(frame, Qt::white);
    p.setPen(QPen(QColor(30, 90, 200), 1, Qt::DashLine));
    p.drawRect(frame);

    const bool graphic = !strokes_.isEmpty() || !image_.isNull();
    const QRectF left(frame.left() + 4, frame.top() + 4, graphic ? frame.width() / 2 - 8 : 0, frame.height() - 8);
    const QRectF text(graphic ? frame.center().x() + 4 : frame.left() + 6, frame.top() + 4,
                      graphic ? frame.width() / 2 - 10 : frame.width() - 12, frame.height() - 8);
    if (!image_.isNull()) {
        const QSizeF s = QSizeF(image_.size()).scaled(left.size(), Qt::KeepAspectRatio);
        p.drawImage(QRectF(left.center() - QPointF(s.width() / 2, s.height() / 2), s), image_);
    } else if (!strokes_.isEmpty() && !canvas_.isEmpty()) {
        const double k = std::min(left.width() / canvas_.width(), left.height() / canvas_.height());
        p.save();
        p.translate(left.center() - QPointF(canvas_.width() * k / 2, canvas_.height() * k / 2));
        p.scale(k, k);
        p.setPen(QPen(QColor(13, 26, 90), 2.0 / k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const QPolygonF& stroke : strokes_) {
            p.drawPolyline(stroke);
        }
        p.restore();
    }
    p.setPen(Qt::black);
    QFont f = font();
    f.setPointSizeF(std::max(6.0, std::min(11.0, text.height() / std::max<qsizetype>(3, lines_.size()) * 0.55)));
    p.setFont(f);
    p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, lines_.join(QLatin1Char('\n')));
}

SignDialog::SignDialog(QWidget* parent, int page, QRectF rect, QString suggestedField,
                       bool canCertify, QVector<FieldRow> emptyFields)
    : QDialog(parent), page_(page), rect_(rect), emptyFields_(std::move(emptyFields)) {
    setWindowTitle(tr("Sign document"));
    QSettings settings;

    auto* layout = new QVBoxLayout(this);
    // Steps: a header saying where one is, the pages, then Back/Next/Sign.
    // The header is numbered in showPlace(), which knows whether Look counts.
    auto* header = new QHBoxLayout;
    for (int i = 0; i < 4; ++i) {
        auto* label = new QLabel(this);
        label->setObjectName(QStringLiteral("stepLabel"));
        stepLabels_.push_back(label);
        header->addWidget(label);
        if (i < 3) {
            auto* arrow = new QLabel(QStringLiteral("›"), this);
            arrow->setEnabled(false);
            stepArrows_.push_back(arrow);
            header->addWidget(arrow);
        }
    }
    header->addStretch(1);
    layout->addLayout(header);
    steps_ = new QStackedWidget(this);
    steps_->setObjectName(QStringLiteral("signSteps"));
    layout->addWidget(steps_, 1);

    // Where: the box drawn, an empty field of the form, a box to draw now,
    // or nowhere at all.
    auto* wherePage = new QWidget(steps_);
    auto* whereLayout = new QVBoxLayout(wherePage);
    whereLayout->setContentsMargins(0, 8, 0, 0);
    if (!rect_.isEmpty()) {
        placeDrawn_ = new QRadioButton(tr("In the box you drew on page %1").arg(page_ + 1), wherePage);
        whereLayout->addWidget(placeDrawn_);
    }
    if (!emptyFields_.isEmpty()) {
        placeField_ = new QRadioButton(tr("In a signature field of the form:"), wherePage);
        whereLayout->addWidget(placeField_);
        fields_ = new QComboBox(wherePage);
        fields_->setObjectName(QStringLiteral("signFields"));
        for (const FieldRow& f : emptyFields_) {
            fields_->addItem(tr("%1, page %2").arg(f.name).arg(f.page + 1), f.name);
        }
        auto* indent = new QHBoxLayout;
        indent->setContentsMargins(24, 0, 0, 0);
        indent->addWidget(fields_, 1);
        whereLayout->addLayout(indent);
        connect(fields_, &QComboBox::currentIndexChanged, this, [this] {
            placeField_->setChecked(true);
            showPlace();
        });
    }
    if (rect_.isEmpty()) {
        placeNewBox_ = new QRadioButton(tr("In a box I draw on the page"), wherePage);
        placeNewBox_->setToolTip(tr("Closes this window; drag a box on the page, and it opens again"));
        whereLayout->addWidget(placeNewBox_);
    }
    placeInvisible_ = new QRadioButton(tr("Invisible: it protects the file without marking a page"), wherePage);
    whereLayout->addWidget(placeInvisible_);
    whereLayout->addStretch(1);
    steps_->addWidget(wherePage);
    // What it starts on: the field asked for, else the box drawn, else (from
    // Sign Invisibly) invisible.
    const int suggested = fields_ != nullptr ? fields_->findData(suggestedField) : -1;
    if (suggested >= 0) {
        fields_->setCurrentIndex(suggested);
        placeField_->setChecked(true);
    } else if (placeDrawn_ != nullptr) {
        placeDrawn_->setChecked(true);
    } else {
        placeInvisible_->setChecked(true);
    }
    for (QRadioButton* b : {placeDrawn_, placeField_, placeNewBox_, placeInvisible_}) {
        if (b != nullptr) {
            connect(b, &QRadioButton::toggled, this, &SignDialog::showPlace);
        }
    }

    auto* howPage = new QWidget(steps_);
    auto* howLayout = new QVBoxLayout(howPage);
    howLayout->setContentsMargins(0, 8, 0, 0);
    auto* form = new QFormLayout;
    form_ = form;
    howLayout->addLayout(form);
    howLayout->addStretch(1);
    steps_->addWidget(howPage);

    auto* lookPage = new QWidget(steps_);
    auto* lookLayout = new QVBoxLayout(lookPage);
    lookLayout->setContentsMargins(0, 8, 0, 0);
    steps_->addWidget(lookPage);
    auto* detailsPage = new QWidget(steps_);
    auto* detailsLayout = new QVBoxLayout(detailsPage);
    detailsLayout->setContentsMargins(0, 8, 0, 0);
    auto* details = new QFormLayout;
    detailsLayout->addLayout(details);
    steps_->addWidget(detailsPage);

    // Where the key is: a .p12 file, or a card that keeps it.
    fromFile_ = new QRadioButton(tr("Key file"), this);
    fromCard_ = new QRadioButton(tr("ID card or token"), this);
    auto* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(fromFile_);
    sourceRow->addWidget(fromCard_);
    sourceRow->addStretch();
    form->addRow(tr("Sign with:"), sourceRow);

    keyPath_ = new QLineEdit(settings.value(QStringLiteral("signing/lastKey")).toString(), this);
    keyPath_->setPlaceholderText(tr("a .p12 or .pfx file"));
    auto* browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &SignDialog::browseForKey);
    keyFileRow_ = new QWidget(this);
    auto* keyRow = new QHBoxLayout(keyFileRow_);
    keyRow->setContentsMargins(0, 0, 0, 0);
    keyRow->addWidget(keyPath_);
    keyRow->addWidget(browse);
    form->addRow(tr("Key file:"), keyFileRow_);

    password_ = new QLineEdit(this);
    password_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), password_);

    cardKeys_ = new QComboBox(this);
    cardKeys_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    auto* refresh = new QPushButton(tr("Refresh"), this);
    refresh->setToolTip(tr("Look for cards again, after inserting one"));
    connect(refresh, &QPushButton::clicked, this, &SignDialog::refreshCardKeys);
    cardRow_ = new QWidget(this);
    auto* cardRow = new QHBoxLayout(cardRow_);
    cardRow->setContentsMargins(0, 0, 0, 0);
    cardRow->addWidget(cardKeys_, 1);
    cardRow->addWidget(refresh);
    form->addRow(tr("Key:"), cardRow_);
    pin_ = new QLineEdit(this);
    pin_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("PIN:"), pin_);
    cardStatus_ = new QLabel(this);
    cardStatus_->setWordWrap(true);
    form->addRow(QString(), cardStatus_);
    connect(cardKeys_, &QComboBox::currentIndexChanged, this, &SignDialog::showCardKey);

    const bool card = settings.value(QStringLiteral("signing/source")).toString() ==
                      QStringLiteral("card");
    (card ? fromCard_ : fromFile_)->setChecked(true);
    connect(fromCard_, &QRadioButton::toggled, this, &SignDialog::showSource);

    name_ = new QLineEdit(settings.value(QStringLiteral("signing/name")).toString(), this);
    name_->setPlaceholderText(tr("taken from the certificate when left empty"));
    details->addRow(tr("Name:"), name_);
    reason_ = new QLineEdit(this);
    details->addRow(tr("Reason:"), reason_);
    location_ = new QLineEdit(settings.value(QStringLiteral("signing/location")).toString(), this);
    details->addRow(tr("Location:"), location_);
    // Certifying says what may still be done to the document after this
    // signature; only the first signature can.
    certify_ = new QComboBox(this);
    certify_->addItem(tr("No: an ordinary signature"), 0);
    certify_->addItem(tr("Yes: no changes allowed afterwards"), 1);
    certify_->addItem(tr("Yes: form filling and signing allowed"), 2);
    certify_->addItem(tr("Yes: also comments (annotations) allowed"), 3);
    certify_->setEnabled(canCertify);
    certify_->setToolTip(canCertify
                             ? tr("A certification is the author's signature: it says what "
                                  "others may still change without breaking it.")
                             : tr("Only a document's first signature can certify it."));
    details->addRow(tr("Certify:"), certify_);
    // What each level means, in words, under the choice.
    auto* certifyNote = new QLabel(detailsPage);
    certifyNote->setWordWrap(true);
    certifyNote->setEnabled(false);
    const auto explainCertify = [this, certifyNote] {
        switch (certify_->currentData().toInt()) {
            case 1: certifyNote->setText(tr("Nothing may be changed after this signature: no comments, no "
                                            "form filling, no further signatures.")); break;
            case 2: certifyNote->setText(tr("Others may still fill in the form and sign; anything else "
                                            "breaks the certification.")); break;
            case 3: certifyNote->setText(tr("Others may still fill in the form, sign, and add comments.")); break;
            default: certifyNote->setText(tr("Anyone may still add to the document; each later change shows "
                                             "as made after this signature.")); break;
        }
    };
    connect(certify_, &QComboBox::currentIndexChanged, this, explainCertify);
    explainCertify();
    details->addRow(QString(), certifyNote);

    {
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
        lookLayout->addWidget(box);
        auto* previewTitle = new QLabel(tr("Preview"), lookPage);
        lookLayout->addWidget(previewTitle);
        preview_ = new SignaturePreview(lookPage);
        lookLayout->addWidget(preview_, 1);
        connect(pad_, &DrawPad::changed, this, &SignDialog::updatePreview);
        for (QRadioButton* b : {textOnly_, drawn_, imported_}) {
            connect(b, &QRadioButton::toggled, this, &SignDialog::updatePreview);
        }
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
    ltv_ = new QCheckBox(tr("Add long-term validation data (PAdES B-LTA)"), this);
    ltv_->setChecked(settings.value(QStringLiteral("signing/ltv"), false).toBool());
    ltv_->setEnabled(useTsa_->isChecked());
    ltv_->setToolTip(tr("Then asks the certificates' own revocation services (OCSP, CRL) over "
                        "the network whether they were valid, and embeds the answers with a "
                        "document timestamp over it all: the signature stays checkable after "
                        "the certificate expires. Only certificate identifiers are sent, "
                        "never the document."));
    connect(useTsa_, &QCheckBox::toggled, ltv_, &QCheckBox::setEnabled);
    auto* proof = new QGroupBox(tr("Proof of time"), detailsPage);
    auto* proofLayout = new QVBoxLayout(proof);
    proofLayout->addWidget(useTsa_);
    proofLayout->addWidget(tsa_);
    proofLayout->addWidget(ltv_);
    detailsLayout->addWidget(proof);
    detailsLayout->addStretch(1);
    connect(name_, &QLineEdit::textChanged, this, &SignDialog::updatePreview);
    connect(reason_, &QLineEdit::textChanged, this, &SignDialog::updatePreview);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    sign_ = buttons->button(QDialogButtonBox::Ok);
    sign_->setText(tr("Sign"));
    sign_->setObjectName(QStringLiteral("signButton"));
    back_ = buttons->addButton(tr("Back"), QDialogButtonBox::ActionRole);
    back_->setObjectName(QStringLiteral("backButton"));
    next_ = buttons->addButton(tr("Next"), QDialogButtonBox::ActionRole);
    next_->setObjectName(QStringLiteral("nextButton"));
    connect(back_, &QPushButton::clicked, this, [this] { goToStep(stepFrom(steps_->currentIndex(), -1)); });
    connect(next_, &QPushButton::clicked, this, [this] {
        const int at = steps_->currentIndex();
        if (at == Where && placeNewBox_ != nullptr && placeNewBox_->isChecked()) {
            done(PlaceBox);
            return;
        }
        if (at == How && !keyIsReady()) {
            return;
        }
        goToStep(stepFrom(at, +1));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!keyIsReady()) {
            goToStep(How);
            return;
        }
        const int at = cardKeys_->currentIndex();
        const leht::crypto::TokenKey* key =
            at >= 0 && at < static_cast<int>(tokenKeys_.size()) ? &tokenKeys_[at] : nullptr;
        if (useTsa_->isChecked() && tsa_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Sign document"),
                                 tr("Give the timestamp authority's URL, or turn timestamping "
                                    "off."));
            return;
        }
        QSettings saved;
        saved.setValue(QStringLiteral("signing/source"),
                       fromCard_->isChecked() ? QStringLiteral("card") : QStringLiteral("file"));
        if (key != nullptr) {
            saved.setValue(QStringLiteral("signing/lastCardKey"), QString::fromStdString(key->uri));
        }
        saved.setValue(QStringLiteral("signing/lastKey"), keyPath_->text());
        saved.setValue(QStringLiteral("signing/name"), name_->text());
        saved.setValue(QStringLiteral("signing/location"), location_->text());
        saved.setValue(QStringLiteral("signing/useTsa"), useTsa_->isChecked());
        saved.setValue(QStringLiteral("signing/tsa"), tsa_->text());
        saved.setValue(QStringLiteral("signing/ltv"), ltv_->isChecked());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    showSource();
    showPlace();
    goToStep(Where);
}

int SignDialog::chosenPage() const {
    if (placeField_ != nullptr && placeField_->isChecked()) {
        return emptyFields_.value(fields_->currentIndex()).page;
    }
    return page_;
}

QRectF SignDialog::chosenRect() const {
    if (placeField_ != nullptr && placeField_->isChecked()) {
        return emptyFields_.value(fields_->currentIndex()).rect;  // empty for an invisible field
    }
    if (placeDrawn_ != nullptr && placeDrawn_->isChecked()) {
        return rect_;
    }
    return {};
}

QString SignDialog::chosenField() const {
    if (placeField_ != nullptr && placeField_->isChecked()) {
        return fields_->currentData().toString();
    }
    return {};
}

int SignDialog::stepFrom(int from, int delta) const {
    int to = from + delta;
    if (to == Look && chosenRect().isEmpty()) {
        to += delta;  // nothing to look at
    }
    return std::clamp(to, 0, steps_->count() - 1);
}

void SignDialog::showPlace() {
    // The header counts only the steps this signature has.
    const bool look = !chosenRect().isEmpty();
    const QString names[4] = {tr("Where"), tr("How"), tr("Look"), tr("Details")};
    int number = 1;
    for (int i = 0; i < 4; ++i) {
        const bool shown = i != Look || look;
        stepLabels_[i]->setVisible(shown);
        if (i > 0) {
            stepArrows_[i - 1]->setVisible(shown);
        }
        if (shown) {
            stepLabels_[i]->setText(QStringLiteral("%1  %2").arg(number++).arg(names[i]));
        }
    }
    if (steps_->currentIndex() == Where && next_ != nullptr) {
        next_->setText(placeNewBox_ != nullptr && placeNewBox_->isChecked() ? tr("Draw the Box")
                                                                             : tr("Next"));
    }
    updatePreview();
}

bool SignDialog::keyIsReady() {
    const int at = cardKeys_->currentIndex();
    const leht::crypto::TokenKey* key =
        at >= 0 && at < static_cast<int>(tokenKeys_.size()) ? &tokenKeys_[at] : nullptr;
    if (fromFile_->isChecked() && keyPath_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Sign document"),
                             tr("Choose the .p12 file holding your signing key."));
        return false;
    }
    if (fromCard_->isChecked()) {
        if (key == nullptr) {
            QMessageBox::warning(this, tr("Sign document"),
                                 tr("No card key is chosen. Insert the card, press "
                                    "Refresh, and choose its signing key."));
            return false;
        }
        if (key->pin_locked) {
            QMessageBox::warning(this, tr("Sign document"), cardStatus_->text());
            return false;
        }
        if (!key->pinpad && pin_->text().isEmpty()) {
            QMessageBox::warning(this, tr("Sign document"), tr("Enter the card's PIN."));
            return false;
        }
    }
    return true;
}

void SignDialog::goToStep(int index) {
    index = std::clamp(index, 0, steps_->count() - 1);
    steps_->setCurrentIndex(index);
    for (int i = 0; i < stepLabels_.size(); ++i) {
        QFont f = stepLabels_[i]->font();
        f.setWeight(i == index ? QFont::DemiBold : QFont::Normal);
        stepLabels_[i]->setFont(f);
        stepLabels_[i]->setEnabled(i <= index);
    }
    const bool last = index == steps_->count() - 1;
    back_->setVisible(index > 0);
    next_->setVisible(!last);
    next_->setText(index == Where && placeNewBox_ != nullptr && placeNewBox_->isChecked() ? tr("Draw the Box")
                                                                                            : tr("Next"));
    sign_->setVisible(last);
    (last ? sign_ : next_)->setDefault(true);
    updatePreview();
}

void SignDialog::updatePreview() {
    if (preview_ == nullptr) {
        return;
    }
    // As the engine will draw it: the graphic, and the name and date beside it.
    const QString name = name_->text().trimmed().isEmpty() ? tr("Name from the certificate")
                                                           : name_->text().trimmed();
    QStringList lines{name, QDate::currentDate().toString(Qt::ISODate)};
    if (!reason_->text().trimmed().isEmpty()) {
        lines << reason_->text().trimmed();
    }
    QImage image;
    if (imported_->isChecked() && !image_.isEmpty()) {
        image.loadFromData(image_);
    }
    const bool drawing = drawn_->isChecked() && !pad_->isEmpty();
    preview_->show(chosenRect().size(), drawing ? pad_->strokes() : QVector<QPolygonF>{}, QSizeF(pad_->size()), image,
                   lines);
}

void SignDialog::showSource() {
    const bool card = fromCard_->isChecked();
    const bool file = fromFile_->isChecked();
    form_->setRowVisible(keyFileRow_, file);
    form_->setRowVisible(password_, file);
    form_->setRowVisible(cardRow_, card);
    form_->setRowVisible(cardStatus_, card);
    if (card && !cardKeysLoaded_) {
        refreshCardKeys();  // shows the PIN row as the chosen key needs
    } else {
        form_->setRowVisible(pin_, card);
        if (card) {
            showCardKey();
        }
    }
}

void SignDialog::refreshCardKeys() {
    const QString previous = cardKeys_->currentIndex() >= 0 && !tokenKeys_.empty()
                                 ? QString::fromStdString(
                                       tokenKeys_[static_cast<std::size_t>(
                                                      cardKeys_->currentIndex())]
                                           .uri)
                                 : QSettings().value(QStringLiteral("signing/lastCardKey"))
                                       .toString();
    QString problem;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        tokenKeys_ = leht::crypto::list_token_keys(pkcs11Module().toStdString());
    } catch (const std::exception& e) {
        tokenKeys_.clear();
        problem = QString::fromUtf8(e.what());
    }
    QApplication::restoreOverrideCursor();
    cardKeysLoaded_ = true;

    const QSignalBlocker quiet(cardKeys_);
    cardKeys_->clear();
    int select = 0;
    for (std::size_t i = 0; i < tokenKeys_.size(); ++i) {
        const leht::crypto::TokenKey& k = tokenKeys_[i];
        const QString who = QString::fromStdString(
            k.cert.common_name.empty() ? k.cert.subject : k.cert.common_name);
        const QString use = k.non_repudiation ? tr("signing") : tr("authentication");
        cardKeys_->addItem(tr("%1 — %2 (%3)")
                               .arg(who, QString::fromStdString(k.token_label), use));
        if (QString::fromStdString(k.uri) == previous) {
            select = static_cast<int>(i);
        }
    }
    cardKeys_->setEnabled(!tokenKeys_.empty());
    if (tokenKeys_.empty()) {
        cardKeys_->addItem(tr("No card found"));
        cardKeys_->setCurrentIndex(-1);
        cardStatus_->setText(problem.isEmpty()
                                 ? tr("No card found. Is the reader connected and the card in "
                                      "it? Then press Refresh.")
                                 : problem);
        form_->setRowVisible(pin_, false);
        return;
    }
    cardKeys_->setCurrentIndex(select);
    showCardKey();
}

void SignDialog::showCardKey() {
    const int at = cardKeys_->currentIndex();
    if (at < 0 || at >= static_cast<int>(tokenKeys_.size())) {
        return;
    }
    const leht::crypto::TokenKey& k = tokenKeys_[static_cast<std::size_t>(at)];
    // An Estonian card has two PINs; name the one this key wants.
    const bool pin2 = k.token_label.find("PIN2") != std::string::npos;
    if (auto* label = qobject_cast<QLabel*>(form_->labelForField(pin_))) {
        label->setText(pin2 ? tr("PIN2:") : tr("PIN:"));
    }
    form_->setRowVisible(pin_, fromCard_->isChecked() && !k.pinpad);

    QStringList notes;
    if (k.pin_locked) {
        notes << tr("This PIN is blocked after too many wrong tries. Unblock it with the PUK "
                    "code (for an Estonian ID card, in DigiDoc4).");
    } else if (k.pin_final_try) {
        notes << tr("Careful: one more wrong PIN blocks it.");
    } else if (k.pin_count_low) {
        notes << tr("A wrong PIN was entered before.");
    }
    if (k.pinpad && !k.pin_locked) {
        notes << tr("Enter the PIN on the reader's keypad when it asks.");
    }
    if (!k.non_repudiation) {
        notes << tr("This key is for logging in, not for signing documents; the card's "
                    "signing key is the better choice.");
    }
    cardStatus_->setText(notes.join(QLatin1Char(' ')));
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
    const int at = cardKeys_->currentIndex();
    if (fromCard_->isChecked() && at >= 0 && at < static_cast<int>(tokenKeys_.size())) {
        spec.pkcs11Uri = QString::fromStdString(tokenKeys_[static_cast<std::size_t>(at)].uri);
        spec.password = pin_->text();
    } else {
        spec.p12Path = keyPath_->text().trimmed();
        spec.password = password_->text();
    }
    spec.certify = certify_->currentData().toInt();
    spec.field = chosenField();
    spec.page = chosenPage();
    spec.rect = chosenRect();
    spec.name = name_->text().trimmed();
    spec.reason = reason_->text().trimmed();
    spec.location = location_->text().trimmed();
    if (useTsa_->isChecked()) {
        spec.tsaUrl = tsa_->text().trimmed();
        spec.ltv = ltv_->isChecked();
    }
    if (spec.rect.isEmpty()) {
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
