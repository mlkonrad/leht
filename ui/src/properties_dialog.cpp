// SPDX-License-Identifier: AGPL-3.0-or-later
#include "properties_dialog.hpp"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimeZone>
#include <QVBoxLayout>

#include <cmath>

QDateTime parsePdfDate(const QString& text) {
    // D:YYYYMMDDHHmmSSOHH'mm' -- everything after the year optional.
    static const QRegularExpression re(QStringLiteral(
        R"(^(?:D:)?(\d{4})(\d{2})?(\d{2})?(\d{2})?(\d{2})?(\d{2})?([Zz+\-])?(\d{2})?'?(\d{2})?'?$)"));
    const QRegularExpressionMatch m = re.match(text.trimmed());
    if (!m.hasMatch()) {
        return {};
    }
    const auto num = [&](int i, int fallback) {
        return m.captured(i).isEmpty() ? fallback : m.captured(i).toInt();
    };
    const QDate date(num(1, 1), num(2, 1), num(3, 1));
    const QTime time(num(4, 0), num(5, 0), num(6, 0));
    if (!date.isValid() || !time.isValid()) {
        return {};
    }
    const QString sign = m.captured(7);
    if (sign.isEmpty()) {
        return QDateTime(date, time);  // no zone given: local, as readers assume
    }
    int offset = (num(8, 0) * 60 + num(9, 0)) * 60;
    if (sign == QLatin1String("-")) {
        offset = -offset;
    }
    return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(sign.compare(QLatin1String("z"), Qt::CaseInsensitive) == 0 ? 0 : offset));
}

QString describePageSize(QSizeF points) {
    if (points.isEmpty()) {
        return {};
    }
    const double w = points.width() * 25.4 / 72.0;
    const double h = points.height() * 25.4 / 72.0;
    const struct {
        const char* name;
        double w, h;
    } kSizes[] = {{"A3", 297, 420}, {"A4", 210, 297}, {"A5", 148, 210}, {"Letter", 215.9, 279.4},
                  {"Legal", 215.9, 355.6}};
    QString name;
    for (const auto& s : kSizes) {
        const bool upright = std::abs(w - s.w) < 2 && std::abs(h - s.h) < 2;
        const bool sideways = std::abs(w - s.h) < 2 && std::abs(h - s.w) < 2;
        if (upright || sideways) {
            name = sideways ? QObject::tr("%1 landscape").arg(QLatin1String(s.name)) : QLatin1String(s.name);
        }
    }
    const QString mm = QObject::tr("%1 × %2 mm").arg(std::lround(w)).arg(std::lround(h));
    return name.isEmpty() ? mm : QStringLiteral("%1, %2").arg(name, mm);
}

namespace {

QString sizeText(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QLabel* fact(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

}  // namespace

PropertiesDialog::PropertiesDialog(QWidget* parent, const QString& path, int pageCount, QSizeF firstPage,
                                   const QHash<QString, QString>& info, bool editable)
    : QDialog(parent) {
    setObjectName(QStringLiteral("propertiesDialog"));
    const QFileInfo file(path);
    setWindowTitle(tr("Properties of “%1”").arg(file.fileName()));
    resize(520, 0);
    auto* layout = new QVBoxLayout(this);

    auto* described = new QGroupBox(tr("Description"), this);
    auto* form = new QFormLayout(described);
    const struct {
        const char* key;
        const char* label;
        const char* hint;
    } kFields[] = {
        {"Title", QT_TR_NOOP("Title:"), QT_TR_NOOP("Shown instead of the file name by many readers")},
        {"Author", QT_TR_NOOP("Author:"), ""},
        {"Subject", QT_TR_NOOP("Subject:"), ""},
        {"Keywords", QT_TR_NOOP("Keywords:"), QT_TR_NOOP("Separated by commas; used by search")},
    };
    for (const auto& f : kFields) {
        const QString key = QLatin1String(f.key);
        const QString value = info.value(QStringLiteral("info:") + key);
        before_.insert(key, value);
        auto* edit = new QLineEdit(value, described);
        edit->setObjectName(QStringLiteral("info") + key);
        edit->setPlaceholderText(tr(f.hint));
        edit->setReadOnly(!editable);
        fields_.insert(key, edit);
        form->addRow(tr(f.label), edit);
    }
    if (!editable) {
        form->addRow(QString(), fact(tr("This document cannot be changed here (it is not a PDF, or its "
                                        "certification forbids it)."),
                                     described));
    }
    layout->addWidget(described);

    auto* facts = new QGroupBox(tr("File"), this);
    auto* factForm = new QFormLayout(facts);
    factForm->addRow(tr("Location:"), fact(file.absolutePath(), facts));
    factForm->addRow(tr("Size:"), fact(sizeText(file.size()), facts));
    factForm->addRow(tr("Pages:"), fact(QString::number(pageCount), facts));
    if (const QString size = describePageSize(firstPage); !size.isEmpty()) {
        factForm->addRow(tr("Page size:"), fact(size, facts));
    }
    if (info.contains(QStringLiteral("format"))) {
        factForm->addRow(tr("Format:"), fact(info.value(QStringLiteral("format")), facts));
    }
    const QString encryption = info.value(QStringLiteral("encryption"));
    factForm->addRow(tr("Password:"),
                     fact(encryption.isEmpty() || encryption == QLatin1String("None")
                              ? tr("None")
                              : tr("Protected (%1)").arg(encryption),
                          facts));
    const auto date = [&](const char* key, const QString& label) {
        const QString raw = info.value(QLatin1String(key));
        if (raw.isEmpty()) {
            return;
        }
        const QDateTime when = parsePdfDate(raw);
        factForm->addRow(label, fact(when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::LongFormat)
                                                    : raw,
                                     facts));
    };
    date("info:CreationDate", tr("Created:"));
    date("info:ModDate", tr("Modified:"));
    if (info.contains(QStringLiteral("info:Creator"))) {
        factForm->addRow(tr("Made with:"), fact(info.value(QStringLiteral("info:Creator")), facts));
    }
    if (info.contains(QStringLiteral("info:Producer"))) {
        factForm->addRow(tr("PDF made by:"), fact(info.value(QStringLiteral("info:Producer")), facts));
    }
    layout->addWidget(facts);

    auto* buttons = new QDialogButtonBox(
        editable ? QDialogButtonBox::Ok | QDialogButtonBox::Cancel : QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QList<QPair<QString, QString>> PropertiesDialog::changes() const {
    QList<QPair<QString, QString>> out;
    for (const QString key : {QStringLiteral("Title"), QStringLiteral("Author"), QStringLiteral("Subject"),
                              QStringLiteral("Keywords")}) {
        const QString now = fields_.value(key)->text().trimmed();
        if (now != before_.value(key).trimmed()) {
            out.push_back({key, now});
        }
    }
    return out;
}
