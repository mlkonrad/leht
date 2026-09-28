// SPDX-License-Identifier: AGPL-3.0-or-later
#include "phone_sign_dialog.hpp"

#include "leht/crypto/crypto.hpp"

#include <algorithm>

#include <QDialogButtonBox>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

/// Pixels a module: SK recommends 6 to 10 on a computer screen.
constexpr int kModule = 6;
/// The quiet zone around the code, in modules, as the QR standard asks.
constexpr int kQuiet = 4;

}  // namespace

PhoneSignDialog::PhoneSignDialog(QWidget* parent, const QString& service) : QDialog(parent) {
    setObjectName(QStringLiteral("phoneSignDialog"));
    setWindowTitle(tr("Sign with %1").arg(service));
    auto* layout = new QVBoxLayout(this);

    qr_ = new QLabel(this);
    qr_->setObjectName(QStringLiteral("phoneQr"));
    qr_->setAlignment(Qt::AlignCenter);
    qr_->setVisible(false);
    layout->addWidget(qr_);

    code_ = new QLabel(this);
    code_->setObjectName(QStringLiteral("phoneCode"));
    code_->setAlignment(Qt::AlignCenter);
    code_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont big = code_->font();
    big.setPointSizeF(big.pointSizeF() * 2.5);
    big.setBold(true);
    code_->setFont(big);
    code_->setVisible(false);
    layout->addWidget(code_);

    status_ = new QLabel(tr("Contacting SK…"), this);
    status_->setObjectName(QStringLiteral("phoneStatus"));
    status_->setWordWrap(true);
    status_->setAlignment(Qt::AlignCenter);
    layout->addWidget(status_);

    auto* note = new QLabel(tr("SK's demo environment. Only the fingerprint to sign is sent, "
                               "never the document."),
                            this);
    note->setWordWrap(true);
    note->setAlignment(Qt::AlignCenter);
    note->setEnabled(false);  // quieter
    layout->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    cancel_ = buttons->button(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::rejected, this, &PhoneSignDialog::reject);
    layout->addWidget(buttons);
    setMinimumWidth(360);
}

void PhoneSignDialog::showLink(const QString& link) {
    leht::crypto::QrCode qr;
    try {
        qr = leht::crypto::qr_modules(link.toStdString());
    } catch (const std::exception& e) {
        status_->setText(QString::fromUtf8(e.what()));
        return;
    }
    const int side = (qr.size + 2 * kQuiet) * kModule;
    QImage image(side, side, QImage::Format_Grayscale8);
    image.fill(255);
    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.at(x, y)) {
                continue;
            }
            for (int dy = 0; dy < kModule; ++dy) {
                uchar* row = image.scanLine((y + kQuiet) * kModule + dy);
                std::fill_n(row + (x + kQuiet) * kModule, kModule, uchar{0});
            }
        }
    }
    // Whatever the screen's scale: a module stays whole device pixels.
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(1.0);
    qr_->setPixmap(pixmap);
    qr_->setVisible(true);
    ++drawn_;
}

void PhoneSignDialog::showCode(const QString& code) {
    // A code replaces the QR code: the phone has been chosen by now.
    qr_->setVisible(false);
    code_->setText(code);
    code_->setToolTip(tr("Your phone must show this same code. If it does not, do not enter "
                         "your PIN."));
    code_->setVisible(true);
}

void PhoneSignDialog::showStatus(const QString& text) { status_->setText(text); }

void PhoneSignDialog::reject() {
    // Closing does not stop the phone by itself: the worker must stop
    // waiting, and says when it has (the owner then closes this).
    if (!cancelling_) {
        cancelling_ = true;
        cancel_->setEnabled(false);
        status_->setText(tr("Cancelling…"));
        emit cancelRequested();
    }
}
