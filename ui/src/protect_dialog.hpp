// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;

/// File > Password Protect: a password to open the document, and optionally
/// restrictions on what a reader may do without the permissions password.
class ProtectDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProtectDialog(QWidget* parent);
    ~ProtectDialog() override;

    [[nodiscard]] QString openPassword() const;
    /// Empty when nothing is restricted.
    [[nodiscard]] QString permissionsPassword() const;
    /// An ops::Encryption.
    [[nodiscard]] int method() const;
    /// ipc::Protect's permission bits.
    [[nodiscard]] int permissions() const;

private:
    void update();

    QLineEdit* password_ = nullptr;
    QLineEdit* repeat_ = nullptr;
    QLabel* strength_ = nullptr;
    QGroupBox* restrict_ = nullptr;
    QCheckBox* print_ = nullptr;
    QCheckBox* copy_ = nullptr;
    QCheckBox* comment_ = nullptr;
    QCheckBox* fill_ = nullptr;
    QCheckBox* change_ = nullptr;
    QLineEdit* owner_ = nullptr;
    QComboBox* method_ = nullptr;
    QLabel* problem_ = nullptr;
    class QPushButton* ok_ = nullptr;
};
