// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;

/// What the person sees while a phone signs, through Smart-ID or Mobile-ID:
/// the QR code to scan (renewed every second, as Smart-ID requires), or the
/// verification code to compare with the phone's, what is happening, and
/// Cancel. It only shows; the signing runs on the render worker's thread,
/// which Cancel asks to stop. The owner closes it when signing ends.
class PhoneSignDialog : public QDialog {
    Q_OBJECT

public:
    PhoneSignDialog(QWidget* parent, const QString& service);

    /// A Smart-ID device link, drawn as a QR code.
    void showLink(const QString& link);
    void showCode(const QString& code);
    void showStatus(const QString& text);

    /// Cancel: asks the worker to stop, and waits for it to say it has.
    void reject() override;

    /// The number of QR codes drawn so far (for the tests).
    [[nodiscard]] int codesDrawn() const { return drawn_; }

signals:
    /// Cancel was pressed (or the window closed): stop waiting on the phone.
    void cancelRequested();

private:
    QLabel* qr_ = nullptr;
    QLabel* code_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* cancel_ = nullptr;
    int drawn_ = 0;
    bool cancelling_ = false;
};
