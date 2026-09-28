// SPDX-License-Identifier: AGPL-3.0-or-later
#include "protect_dialog.hpp"
#include "contrast.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QLineEdit* passwordField(QWidget* parent, const QString& placeholder) {
    auto* field = new QLineEdit(parent);
    field->setEchoMode(QLineEdit::Password);
    field->setPlaceholderText(placeholder);
    field->setMaxLength(127);  // the most a PDF password can be
    field->setClearButtonEnabled(true);
    return field;
}

/// A rough word on a password: length and variety, nothing cleverer. Enough
/// to stop "1234" without pretending to measure entropy.
QString strengthWord(const QString& pw, QColor& colour) {
    if (pw.isEmpty()) {
        return {};
    }
    int kinds = 0;
    bool lower = false, upper = false, digit = false, other = false;
    for (const QChar c : pw) {
        lower = lower || c.isLower();
        upper = upper || c.isUpper();
        digit = digit || c.isDigit();
        other = other || !c.isLetterOrNumber();
    }
    kinds = int(lower) + int(upper) + int(digit) + int(other);
    if (pw.size() < 8 || (pw.size() < 12 && kinds < 2)) {
        colour = QColor(200, 30, 30);
        return QObject::tr("Weak: use at least 12 characters, or several words.");
    }
    if (pw.size() < 14 && kinds < 3) {
        colour = QColor(210, 130, 0);
        return QObject::tr("Fair.");
    }
    colour = QColor(30, 150, 60);
    return QObject::tr("Strong.");
}

}  // namespace

ProtectDialog::ProtectDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("protectDialog"));
    setWindowTitle(tr("Password Protect"));
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(tr("Anyone who opens the new file will be asked for this password. "
                                "Leht cannot recover it if it is lost."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    password_ = passwordField(this, tr("Password to open"));
    password_->setObjectName(QStringLiteral("openPassword"));
    repeat_ = passwordField(this, tr("The same again"));
    repeat_->setObjectName(QStringLiteral("repeatPassword"));
    strength_ = new QLabel(this);
    form->addRow(tr("Password:"), password_);
    form->addRow(tr("Repeat:"), repeat_);
    form->addRow(QString(), strength_);
    layout->addLayout(form);

    restrict_ = new QGroupBox(tr("Also restrict what readers may do"), this);
    restrict_->setObjectName(QStringLiteral("restrict"));
    restrict_->setCheckable(true);
    restrict_->setChecked(false);
    auto* restrictions = new QVBoxLayout(restrict_);
    auto* honest = new QLabel(tr("PDF readers are asked to honour these, but a reader that ignores them "
                                 "is not stopped: they are a request, not a lock. The password above "
                                 "is what actually protects the content."),
                              restrict_);
    honest->setWordWrap(true);
    restrictions->addWidget(honest);
    print_ = new QCheckBox(tr("Allow printing"), restrict_);
    print_->setChecked(true);
    copy_ = new QCheckBox(tr("Allow copying text and images"), restrict_);
    comment_ = new QCheckBox(tr("Allow comments"), restrict_);
    fill_ = new QCheckBox(tr("Allow filling in forms and signing"), restrict_);
    fill_->setChecked(true);
    change_ = new QCheckBox(tr("Allow changing the document and its pages"), restrict_);
    for (QCheckBox* box : {print_, copy_, comment_, fill_, change_}) {
        restrictions->addWidget(box);
    }
    auto* ownerForm = new QFormLayout;
    owner_ = passwordField(restrict_, tr("Different from the one to open"));
    owner_->setObjectName(QStringLiteral("permissionsPassword"));
    ownerForm->addRow(tr("Permissions password:"), owner_);
    restrictions->addLayout(ownerForm);
    layout->addWidget(restrict_);

    auto* advanced = new QFormLayout;
    method_ = new QComboBox(this);
    method_->addItem(tr("AES-256 (recommended)"), 2);
    method_->addItem(tr("AES-128"), 1);
    method_->addItem(tr("RC4-128 (only for very old readers; weak)"), 0);
    advanced->addRow(tr("Encryption:"), method_);
    layout->addLayout(advanced);

    problem_ = new QLabel(this);
    problem_->setWordWrap(true);
    QPalette warn = problem_->palette();
    warn.setColor(QPalette::WindowText, contrast::readableOn(QColor(200, 30, 30), warn.color(QPalette::Window)));
    problem_->setPalette(warn);
    layout->addWidget(problem_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    ok_->setText(tr("Protect…"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (QLineEdit* field : {password_, repeat_, owner_}) {
        connect(field, &QLineEdit::textChanged, this, &ProtectDialog::update);
    }
    connect(restrict_, &QGroupBox::toggled, this, &ProtectDialog::update);
    update();
}

ProtectDialog::~ProtectDialog() {
    // Passwords do not outlive the dialog in its fields.
    for (QLineEdit* field : {password_, repeat_, owner_}) {
        field->clear();
    }
}

void ProtectDialog::update() {
    QColor colour;
    const QString word = strengthWord(password_->text(), colour);
    strength_->setText(word);
    QPalette pal = strength_->palette();
    pal.setColor(QPalette::WindowText, contrast::readableOn(colour, pal.color(QPalette::Window)));
    strength_->setPalette(pal);

    QString problem;
    if (password_->text().isEmpty() && !restrict_->isChecked()) {
        problem = tr("Give a password, or choose restrictions.");
    } else if (password_->text() != repeat_->text()) {
        problem = repeat_->text().isEmpty() ? tr("Type the password again to confirm it.")
                                            : tr("The two passwords differ.");
    } else if (restrict_->isChecked() && owner_->text().isEmpty()) {
        problem = tr("Restrictions need a permissions password, or anyone could lift them.");
    } else if (restrict_->isChecked() && owner_->text() == password_->text()) {
        problem = tr("The permissions password must differ from the one to open, or anyone who can "
                     "open the file could lift the restrictions.");
    }
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty() && !(password_->text().isEmpty() && repeat_->text().isEmpty() &&
                                                 !restrict_->isChecked()));
    ok_->setEnabled(problem.isEmpty());
}

QString ProtectDialog::openPassword() const {
    return password_->text();
}

QString ProtectDialog::permissionsPassword() const {
    return restrict_->isChecked() ? owner_->text() : QString();
}

int ProtectDialog::method() const {
    return method_->currentData().toInt();
}

int ProtectDialog::permissions() const {
    // print, modify, copy, annotate, fill_forms, assemble, print_high_quality
    if (!restrict_->isChecked()) {
        return 0x7F;
    }
    int bits = 0;
    const auto set = [&](int bit, bool on) { bits |= on ? (1 << bit) : 0; };
    set(0, print_->isChecked());
    set(1, change_->isChecked());
    set(2, copy_->isChecked());
    set(3, comment_->isChecked());
    set(4, fill_->isChecked());
    set(5, change_->isChecked());
    set(6, print_->isChecked());
    return bits;
}
