// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QDialog>

class QButtonGroup;
class QComboBox;
class QLineEdit;

/// File > Export: which pages, and for pictures how sharp. Where to write is
/// asked after, with the desktop's own file dialog.
class ExportDialog : public QDialog {
    Q_OBJECT

public:
    enum class Kind { Images, Text };
    ExportDialog(QWidget* parent, Kind kind, int pageCount, int currentPage);

    /// A range spec for the pages chosen (1-based), "" for all.
    [[nodiscard]] QString pages() const;
    /// Dots per inch, for images.
    [[nodiscard]] int dpi() const;

private:
    int pageCount_;
    int currentPage_;
    QButtonGroup* which_ = nullptr;
    QLineEdit* range_ = nullptr;
    QComboBox* dpi_ = nullptr;
};
