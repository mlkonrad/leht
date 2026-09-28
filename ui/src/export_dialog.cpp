// SPDX-License-Identifier: AGPL-3.0-or-later
#include "export_dialog.hpp"

#include "leht/edit.hpp"
#include "leht/error.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

ExportDialog::ExportDialog(QWidget* parent, Kind kind, int pageCount, int currentPage)
    : QDialog(parent), pageCount_(pageCount), currentPage_(currentPage) {
    setObjectName(QStringLiteral("exportDialog"));
    setWindowTitle(kind == Kind::Images ? tr("Export Pages as Images") : tr("Export Text"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(kind == Kind::Images
                                 ? tr("Each page becomes a PNG picture. The document is not changed.")
                                 : tr("The text of the pages, as plain text in reading order. The document "
                                      "is not changed; scanned pages have text only after Recognize Text."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    which_ = new QButtonGroup(this);
    auto* all = new QRadioButton(tr("All %n page(s)", nullptr, pageCount), this);
    auto* current = new QRadioButton(tr("This page (%1)").arg(currentPage + 1), this);
    auto* some = new QRadioButton(tr("Pages:"), this);
    range_ = new QLineEdit(this);
    range_->setPlaceholderText(tr("e.g. 1-3, 7"));
    range_->setEnabled(false);
    which_->addButton(all, 0);
    which_->addButton(current, 1);
    which_->addButton(some, 2);
    (kind == Kind::Images ? current : all)->setChecked(true);
    layout->addWidget(all);
    layout->addWidget(current);
    auto* row = new QHBoxLayout;
    row->addWidget(some);
    row->addWidget(range_, 1);
    layout->addLayout(row);
    connect(some, &QRadioButton::toggled, range_, &QLineEdit::setEnabled);

    if (kind == Kind::Images) {
        auto* form = new QFormLayout;
        dpi_ = new QComboBox(this);
        dpi_->addItem(tr("Screen (96 dpi)"), 96);
        dpi_->addItem(tr("Good (150 dpi)"), 150);
        dpi_->addItem(tr("Print (300 dpi)"), 300);
        dpi_->setCurrentIndex(1);
        form->addRow(tr("Sharpness:"), dpi_);
        layout->addLayout(form);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export…"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (which_->checkedId() == 2) {
            try {
                if (leht::page_set(range_->text().trimmed().toStdString(), pageCount_).empty()) {
                    throw leht::Error(0, "no pages");
                }
            } catch (const leht::Error&) {
                QMessageBox::warning(this, windowTitle(),
                                     tr("Give pages between 1 and %1, like 1-3, 7.").arg(pageCount_));
                return;
            }
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString ExportDialog::pages() const {
    switch (which_->checkedId()) {
        case 1: return QString::number(currentPage_ + 1);
        case 2: return range_->text().trimmed();
        default: return {};
    }
}

int ExportDialog::dpi() const {
    return dpi_ != nullptr ? dpi_->currentData().toInt() : 150;
}
