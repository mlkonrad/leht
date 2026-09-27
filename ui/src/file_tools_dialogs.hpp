// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;

/// Combine Files: PDFs and images, in an order the user sets, into one PDF
/// (`leht merge`). The output is chosen when the dialog is accepted.
class CombineDialog : public QDialog {
    Q_OBJECT

public:
    /// `current` is the open document, listed first; empty for none.
    CombineDialog(QWidget* parent, const QString& current);

    [[nodiscard]] QStringList inputs() const;
    [[nodiscard]] QString output() const { return output_; }

    /// The file-dialog filter for everything Combine accepts.
    [[nodiscard]] static QString inputFilter();

    /// Adds files to the list, as if chosen with Add Files (files dropped on
    /// the window, say).
    void addPaths(const QStringList& paths);

private:
    void addFiles();
    void move(int by);
    void updateButtons();
    void chooseOutputAndAccept();

    QListWidget* list_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* down_ = nullptr;
    QPushButton* combine_ = nullptr;
    QString output_;
};

/// Reduce File Size (`leht compress`): a preset in plain words, and where to
/// write the smaller copy. The original is never replaced.
class ReduceDialog : public QDialog {
    Q_OBJECT

public:
    /// `signedDocument`: warn that the copy will not carry the signatures.
    ReduceDialog(QWidget* parent, const QString& input, bool signedDocument);

    /// An ops::CompressPreset.
    [[nodiscard]] int preset() const;
    [[nodiscard]] QString output() const;

private:
    void browse();
    void validate();

    QString input_;
    QRadioButton* presets_[4] = {};
    QLineEdit* output_ = nullptr;
    QLabel* problem_ = nullptr;
    QPushButton* ok_ = nullptr;
};

/// Split Document (`leht split`): every N pages, or at page ranges the user
/// gives, into numbered files in a folder.
class SplitDialog : public QDialog {
    Q_OBJECT

public:
    SplitDialog(QWidget* parent, const QString& input, int pageCount, bool signedDocument);

    /// One page-range spec per output file, and the files, in step.
    [[nodiscard]] QStringList ranges() const { return ranges_; }
    [[nodiscard]] QStringList outputs() const { return outputs_; }

    /// The parts for `pageCount` pages, `perFile` to a file: "1-3", "4-6", ...
    [[nodiscard]] static QStringList chunks(int pageCount, int perFile);
    /// "name-1.pdf" .. "name-12.pdf", zero-padded so they sort: "name-01.pdf".
    [[nodiscard]] static QStringList numberedNames(const QString& folder, const QString& base,
                                                   int count);

private:
    void browse();
    /// Recomputes ranges_ and outputs_ and says what will be written, or why not.
    void refresh();
    void confirmAndAccept();

    QString input_;
    int pageCount_;
    QRadioButton* everyMode_ = nullptr;
    QSpinBox* every_ = nullptr;
    QRadioButton* rangesMode_ = nullptr;
    QLineEdit* rangesEdit_ = nullptr;
    QLineEdit* folder_ = nullptr;
    QLineEdit* base_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* ok_ = nullptr;
    QStringList ranges_;
    QStringList outputs_;
};
