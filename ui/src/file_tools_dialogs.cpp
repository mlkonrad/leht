// SPDX-License-Identifier: AGPL-3.0-or-later
#include "file_tools_dialogs.hpp"

#include "file_tools.hpp"

#include "leht/error.hpp"
#include "leht/ops/pages.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/// "report.pdf" -> "report"; "scan.jpg" -> "scan".
QString baseName(const QString& path) { return QFileInfo(path).completeBaseName(); }

/// A note that the new files will not carry the document's signatures:
/// signatures cover bytes, and every one of these writes new bytes.
QLabel* signatureNote(QWidget* parent, const QString& text) {
    auto* note = new QLabel(text, parent);
    note->setWordWrap(true);
    return note;
}

}  // namespace

// --- Combine Files ---------------------------------------------------------------

QString CombineDialog::inputFilter() {
    return QObject::tr("PDFs and images (*.pdf *.jpg *.jpeg *.jpe *.png *.tif *.tiff *.bmp *.gif "
                       "*.pnm *.pgm *.ppm *.pam *.jp2 *.jpx);;All files (*)");
}

CombineDialog::CombineDialog(QWidget* parent, const QString& current) : QDialog(parent) {
    setWindowTitle(tr("Combine files"));
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(tr("PDFs and images are combined into one new PDF, in this order. "
                               "Drag to reorder. Images become one page each, without "
                               "losing quality."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* row = new QHBoxLayout;
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("combineList"));
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setMinimumSize(380, 220);
    row->addWidget(list_, 1);

    auto* side = new QVBoxLayout;
    auto* add = new QPushButton(tr("Add…"), this);
    remove_ = new QPushButton(tr("Remove"), this);
    up_ = new QPushButton(tr("Move Up"), this);
    down_ = new QPushButton(tr("Move Down"), this);
    for (QPushButton* b : {add, remove_, up_, down_}) {
        side->addWidget(b);
    }
    side->addStretch();
    row->addLayout(side);
    layout->addLayout(row);

    connect(add, &QPushButton::clicked, this, &CombineDialog::addFiles);
    connect(remove_, &QPushButton::clicked, this, [this] {
        qDeleteAll(list_->selectedItems());
        updateButtons();
    });
    connect(up_, &QPushButton::clicked, this, [this] { move(-1); });
    connect(down_, &QPushButton::clicked, this, [this] { move(1); });
    connect(list_, &QListWidget::itemSelectionChanged, this, &CombineDialog::updateButtons);
    connect(list_->model(), &QAbstractItemModel::rowsMoved, this, &CombineDialog::updateButtons);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    combine_ = buttons->button(QDialogButtonBox::Ok);
    combine_->setText(tr("Combine…"));
    connect(buttons, &QDialogButtonBox::accepted, this, &CombineDialog::chooseOutputAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    if (!current.isEmpty()) {
        addPaths({current});
    }
    updateButtons();
}

QStringList CombineDialog::inputs() const {
    QStringList paths;
    for (int i = 0; i < list_->count(); ++i) {
        paths << list_->item(i)->data(Qt::UserRole).toString();
    }
    return paths;
}

void CombineDialog::addFiles() {
    const QStringList paths =
        QFileDialog::getOpenFileNames(this, tr("Add files"), QString(), inputFilter());
    addPaths(paths);
}

void CombineDialog::addPaths(const QStringList& paths) {
    for (const QString& path : paths) {
        const QFileInfo info(path);
        auto* item = new QListWidgetItem(info.fileName(), list_);
        item->setData(Qt::UserRole, info.absoluteFilePath());
        item->setToolTip(info.absoluteFilePath());
    }
    updateButtons();
}

void CombineDialog::move(int by) {
    // Selected rows move together, keeping their order; a block already at
    // the edge stays put.
    QList<int> rows;
    for (const QListWidgetItem* item : list_->selectedItems()) {
        rows << list_->row(item);
    }
    if (rows.isEmpty()) {
        return;
    }
    std::sort(rows.begin(), rows.end());
    if ((by < 0 && rows.first() == 0) || (by > 0 && rows.last() == list_->count() - 1)) {
        return;
    }
    if (by > 0) {
        std::reverse(rows.begin(), rows.end());
    }
    for (const int r : rows) {
        QListWidgetItem* item = list_->takeItem(r);
        list_->insertItem(r + by, item);
        item->setSelected(true);
    }
    updateButtons();
}

void CombineDialog::updateButtons() {
    const bool any = !list_->selectedItems().isEmpty();
    remove_->setEnabled(any);
    up_->setEnabled(any);
    down_->setEnabled(any);
    combine_->setEnabled(list_->count() > 0);
}

void CombineDialog::chooseOutputAndAccept() {
    const QStringList paths = inputs();
    if (paths.isEmpty()) {
        return;
    }
    const QFileInfo first(paths.first());
    const QString suggestion = first.absoluteDir().filePath(
        paths.size() == 1 ? baseName(first.fileName()) + QStringLiteral(".pdf")
                          : tr("Combined") + QStringLiteral(".pdf"));
    QString path = QFileDialog::getSaveFileName(this, tr("Save combined PDF as"), suggestion,
                                                tr("PDF files (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".pdf");
    }
    output_ = path;
    accept();
}

// --- Reduce File Size ------------------------------------------------------------

ReduceDialog::ReduceDialog(QWidget* parent, const QString& input, bool signedDocument, FileTools* tools)
    : QDialog(parent), input_(input), tools_(tools) {
    setWindowTitle(tr("Reduce file size"));
    auto* layout = new QVBoxLayout(this);
    const QFileInfo info(input);
    auto* intro = new QLabel(tr("Makes a smaller copy of “%1” (%2). The original is not changed.")
                                 .arg(info.fileName(), QLocale().formattedDataSize(info.size())),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // In the order of ops::CompressPreset.
    const QString labels[4] = {
        tr("Best quality — tidy the file only; pictures stay exactly as they are"),
        tr("High quality — good for printing (pictures at about 300 dpi)"),
        tr("Balanced — good for sharing and e-mail (about 150 dpi)"),
        tr("Smallest — for reading on screen (about 72 dpi)"),
    };
    auto* grid = new QGridLayout;
    grid->setColumnStretch(0, 1);
    for (int i = 0; i < 4; ++i) {
        presets_[i] = new QRadioButton(labels[i], this);
        grid->addWidget(presets_[i], i, 0);
        estimates_[i] = new QLabel(this);
        estimates_[i]->setObjectName(QStringLiteral("reduceEstimate%1").arg(i));
        estimates_[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(estimates_[i], i, 1);
    }
    layout->addLayout(grid);
    presets_[2]->setChecked(true);

    if (tools_ != nullptr) {
        const qint64 before = info.size();
        for (QLabel* l : estimates_) {
            l->setText(tr("estimating…"));
            l->setEnabled(false);
        }
        // Queued back to this thread; the connection dies with the dialog.
        connect(tools_, &FileTools::estimated, this, [this, before](int preset, qint64 bytes) {
            if (preset < 0 || preset > 3) {
                return;
            }
            QLabel* l = estimates_[preset];
            l->setEnabled(true);
            if (bytes < 0) {
                l->setText(QString());
            } else if (bytes >= before) {
                l->setText(tr("no smaller"));
                l->setEnabled(false);
            } else {
                const int saved = static_cast<int>(100.0 - 100.0 * static_cast<double>(bytes) /
                                                               static_cast<double>(before));
                l->setText(tr("about %1 (−%2%)").arg(QLocale().formattedDataSize(bytes)).arg(saved));
            }
        });
        FileTools* t = tools_;
        const QString path = input;
        QMetaObject::invokeMethod(tools_, [t, path] { t->estimate(path, QString()); }, Qt::QueuedConnection);
    }

    auto* where = new QFormLayout;
    auto* row = new QHBoxLayout;
    output_ = new QLineEdit(
        info.absoluteDir().filePath(info.completeBaseName() + tr("-smaller") + QStringLiteral(".pdf")),
        this);
    output_->setObjectName(QStringLiteral("reduceOutput"));
    auto* browse = new QPushButton(tr("Browse…"), this);
    row->addWidget(output_, 1);
    row->addWidget(browse);
    where->addRow(tr("Save as:"), row);
    layout->addLayout(where);

    if (signedDocument) {
        layout->addWidget(signatureNote(
            this, tr("This document is signed. The smaller copy will not carry the signatures.")));
    }
    problem_ = new QLabel(this);
    problem_->setWordWrap(true);
    layout->addWidget(problem_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    ok_->setText(tr("Reduce"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (QFileInfo::exists(output()) &&
            QMessageBox::question(this, windowTitle(),
                                  tr("“%1” already exists. Replace it?")
                                      .arg(QFileInfo(output()).fileName())) != QMessageBox::Yes) {
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this, &ReduceDialog::browse);
    connect(output_, &QLineEdit::textChanged, this, &ReduceDialog::validate);
    validate();
}

void ReduceDialog::done(int result) {
    // Before the dialog's caller queues the real job, never after: a late
    // cancel would stop that job instead of the estimate.
    if (tools_ != nullptr) {
        tools_->cancel();  // thread-safe; the estimate stops after its current preset
    }
    QDialog::done(result);
}

int ReduceDialog::preset() const {
    for (int i = 0; i < 4; ++i) {
        if (presets_[i]->isChecked()) {
            return i;
        }
    }
    return 2;
}

QString ReduceDialog::output() const {
    QString path = output_->text().trimmed();
    if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".pdf");
    }
    return path;
}

void ReduceDialog::browse() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save smaller copy as"), output(),
                                                      tr("PDF files (*.pdf)"), nullptr,
                                                      QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty()) {
        output_->setText(path);
    }
}

void ReduceDialog::validate() {
    QString problem;
    const QString path = output();
    if (path.isEmpty()) {
        problem = tr("Choose where to save the smaller copy.");
    } else if (QFileInfo(path).absoluteFilePath() == QFileInfo(input_).absoluteFilePath()) {
        problem = tr("Choose another name: the original is never replaced.");
    } else if (!QFileInfo(QFileInfo(path).absolutePath()).isDir()) {
        problem = tr("That folder does not exist.");
    }
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
    ok_->setEnabled(problem.isEmpty());
}

// --- Split Document --------------------------------------------------------------

QStringList SplitDialog::chunks(int pageCount, int perFile) {
    QStringList parts;
    if (perFile < 1) {
        return parts;
    }
    for (int first = 1; first <= pageCount; first += perFile) {
        const int last = std::min(first + perFile - 1, pageCount);
        parts << (first == last ? QString::number(first)
                                : QStringLiteral("%1-%2").arg(first).arg(last));
    }
    return parts;
}

QStringList SplitDialog::numberedNames(const QString& folder, const QString& base, int count) {
    QStringList names;
    const int width = static_cast<int>(QString::number(count).size());
    const QDir dir(folder);
    for (int i = 1; i <= count; ++i) {
        names << dir.filePath(QStringLiteral("%1-%2.pdf").arg(base).arg(i, width, 10, QLatin1Char('0')));
    }
    return names;
}

SplitDialog::SplitDialog(QWidget* parent, const QString& input, int pageCount,
                         bool signedDocument)
    : QDialog(parent), input_(input), pageCount_(pageCount) {
    setWindowTitle(tr("Split document"));
    auto* layout = new QVBoxLayout(this);
    const QFileInfo info(input);
    auto* intro = new QLabel(tr("Writes the pages of “%1” (%n page(s)) into separate new files. "
                                "The original is not changed.",
                                nullptr, pageCount)
                                 .arg(info.fileName()),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* how = new QFormLayout;
    everyMode_ = new QRadioButton(tr("Every"), this);
    every_ = new QSpinBox(this);
    every_->setRange(1, std::max(1, pageCount));
    every_->setSuffix(tr(" page(s) to a file"));
    how->addRow(everyMode_, every_);
    rangesMode_ = new QRadioButton(tr("Pages:"), this);
    rangesEdit_ = new QLineEdit(this);
    rangesEdit_->setObjectName(QStringLiteral("splitRanges"));
    rangesEdit_->setPlaceholderText(tr("one file per group, e.g. 1-3; 4-10; 11-"));
    rangesEdit_->setEnabled(false);
    how->addRow(rangesMode_, rangesEdit_);
    everyMode_->setChecked(true);
    layout->addLayout(how);

    auto* where = new QFormLayout;
    auto* row = new QHBoxLayout;
    folder_ = new QLineEdit(info.absolutePath(), this);
    folder_->setObjectName(QStringLiteral("splitFolder"));
    auto* browse = new QPushButton(tr("Browse…"), this);
    row->addWidget(folder_, 1);
    row->addWidget(browse);
    where->addRow(tr("Folder:"), row);
    base_ = new QLineEdit(info.completeBaseName(), this);
    where->addRow(tr("Names start with:"), base_);
    layout->addLayout(where);

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    summary_->setObjectName(QStringLiteral("splitSummary"));
    layout->addWidget(summary_);
    if (signedDocument) {
        layout->addWidget(signatureNote(
            this, tr("This document is signed. The new files will not carry the signatures.")));
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    ok_->setText(tr("Split"));
    connect(buttons, &QDialogButtonBox::accepted, this, &SplitDialog::confirmAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(everyMode_, &QRadioButton::toggled, this, [this](bool on) {
        every_->setEnabled(on);
        rangesEdit_->setEnabled(!on);
        refresh();
    });
    connect(every_, &QSpinBox::valueChanged, this, &SplitDialog::refresh);
    connect(rangesEdit_, &QLineEdit::textChanged, this, &SplitDialog::refresh);
    connect(folder_, &QLineEdit::textChanged, this, &SplitDialog::refresh);
    connect(base_, &QLineEdit::textChanged, this, &SplitDialog::refresh);
    connect(browse, &QPushButton::clicked, this, &SplitDialog::browse);
    refresh();
}

void SplitDialog::browse() {
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("Write the files into"), folder_->text());
    if (!dir.isEmpty()) {
        folder_->setText(dir);
    }
}

void SplitDialog::refresh() {
    ranges_.clear();
    outputs_.clear();
    QString problem;

    if (everyMode_->isChecked()) {
        ranges_ = chunks(pageCount_, every_->value());
    } else {
        for (const QString& part : rangesEdit_->text().split(QLatin1Char(';'))) {
            const QString spec = part.trimmed();
            if (spec.isEmpty()) {
                continue;
            }
            try {
                (void)leht::ops::parse_page_ranges(spec.toStdString(), pageCount_);
            } catch (const leht::Error& e) {
                problem = tr("“%1”: %2").arg(spec, QString::fromUtf8(e.what()));
                break;
            }
            ranges_ << spec;
        }
        if (problem.isEmpty() && ranges_.isEmpty()) {
            problem = tr("Give the pages for each file, separated by semicolons.");
        }
    }

    const QString base = base_->text().trimmed();
    if (problem.isEmpty()) {
        if (base.isEmpty() || base.contains(QLatin1Char('/'))) {
            problem = tr("Give the files a name, without “/”.");
        } else if (!QFileInfo(folder_->text()).isDir()) {
            problem = tr("That folder does not exist.");
        }
    }

    if (problem.isEmpty()) {
        outputs_ = numberedNames(folder_->text(), base, static_cast<int>(ranges_.size()));
        const QString original = QFileInfo(input_).absoluteFilePath();
        for (const QString& path : outputs_) {
            if (QFileInfo(path).absoluteFilePath() == original) {
                problem = tr("Choose another name: “%1” is the original.")
                              .arg(QFileInfo(path).fileName());
                outputs_.clear();
                break;
            }
        }
    }
    if (problem.isEmpty()) {
        const QString first = QFileInfo(outputs_.first()).fileName();
        summary_->setText(outputs_.size() == 1
                              ? tr("Makes 1 file: %1").arg(first)
                              : tr("Makes %1 files: %2 … %3")
                                    .arg(outputs_.size())
                                    .arg(first, QFileInfo(outputs_.last()).fileName()));
    } else {
        ranges_.clear();
        summary_->setText(problem);
    }
    ok_->setEnabled(problem.isEmpty());
}

void SplitDialog::confirmAndAccept() {
    refresh();
    if (outputs_.isEmpty()) {
        return;
    }
    QStringList existing;
    for (const QString& path : outputs_) {
        if (QFileInfo::exists(path)) {
            existing << QFileInfo(path).fileName();
        }
    }
    if (!existing.isEmpty() &&
        QMessageBox::question(this, windowTitle(),
                              tr("%n file(s) already exist and would be replaced: %1", nullptr,
                                 static_cast<int>(existing.size()))
                                  .arg(existing.mid(0, 5).join(QStringLiteral(", ")) +
                                       (existing.size() > 5 ? QStringLiteral(", …") : QString()))) !=
            QMessageBox::Yes) {
        return;
    }
    accept();
}
