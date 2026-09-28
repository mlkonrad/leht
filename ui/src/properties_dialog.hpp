// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QList>
#include <QPair>
#include <QSizeF>

class QLineEdit;

/// File > Document Properties: the title, author, subject and keywords to
/// edit, and what the file is (size, pages, PDF version, encryption, the
/// software that made it, when) to read.
class PropertiesDialog : public QDialog {
    Q_OBJECT

public:
    /// `info` maps Document::metadata() keys ("format", "info:Title", ...) to
    /// values, as the worker gave them.
    PropertiesDialog(QWidget* parent, const QString& path, int pageCount, QSizeF firstPage,
                     const QHash<QString, QString>& info, bool editable);

    /// The fields changed: (Title|Author|Subject|Keywords, new value).
    [[nodiscard]] QList<QPair<QString, QString>> changes() const;

private:
    QHash<QString, QString> before_;
    QHash<QString, QLineEdit*> fields_;
};

/// A PDF date ("D:20260928143000+03'00'") as a QDateTime; invalid if it is not one.
[[nodiscard]] QDateTime parsePdfDate(const QString& text);
/// A page size in points as a person would say it: "A4, 210 × 297 mm".
[[nodiscard]] QString describePageSize(QSizeF points);
