// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sign_dialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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
    form_ = form;

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
        const int at = cardKeys_->currentIndex();
        const leht::crypto::TokenKey* key =
            at >= 0 && at < static_cast<int>(tokenKeys_.size()) ? &tokenKeys_[at] : nullptr;
        if (fromFile_->isChecked() && keyPath_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Sign document"),
                                 tr("Choose the .p12 file holding your signing key."));
            return;
        }
        if (fromCard_->isChecked()) {
            if (key == nullptr) {
                QMessageBox::warning(this, tr("Sign document"),
                                     tr("No card key is chosen. Insert the card, press "
                                        "Refresh, and choose its signing key."));
                return;
            }
            if (key->pin_locked) {
                QMessageBox::warning(this, tr("Sign document"), cardStatus_->text());
                return;
            }
            if (!key->pinpad && pin_->text().isEmpty()) {
                QMessageBox::warning(this, tr("Sign document"), tr("Enter the card's PIN."));
                return;
            }
        }
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
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    showSource();
}

void SignDialog::showSource() {
    const bool card = fromCard_->isChecked();
    form_->setRowVisible(keyFileRow_, !card);
    form_->setRowVisible(password_, !card);
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
