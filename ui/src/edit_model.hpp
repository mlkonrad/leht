// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QMetaType>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

/// One annotation, as the GUI sees it: for hit-testing (erasing) and listing.
/// Built on the worker thread from ops::AnnotInfo. Geometry is in base
/// coordinates, like every other box the view draws.
struct AnnotRow {
    int id = 0;
    int page = 0;
    QString type;
    QRectF rect;
    QString contents;
};

/// One form field, for the Form panel. `type` is a leht::ops::FieldType cast
/// to int.
struct FieldRow {
    QString name;
    int type = 0;
    QString value;
    QStringList options;
    int page = 0;
    bool readOnly = false;
};

Q_DECLARE_METATYPE(AnnotRow)
Q_DECLARE_METATYPE(QVector<AnnotRow>)
Q_DECLARE_METATYPE(FieldRow)
Q_DECLARE_METATYPE(QVector<FieldRow>)
