// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

/// A flattened outline row for crossing the thread boundary. The worker cannot
/// build QWidgets (wrong thread), so it emits these and the GUI builds the tree.
/// `depth` gives the nesting level (0 = top), which is enough to rebuild the
/// hierarchy in order.
struct OutlineRow {
    int depth = 0;
    QString title;
    int page = -1;   ///< 0-based target page, -1 if none
    double y = 0.0;  ///< target offset down the page, in unscaled points
};

Q_DECLARE_METATYPE(OutlineRow)
Q_DECLARE_METATYPE(QVector<OutlineRow>)
