// SPDX-License-Identifier: AGPL-3.0-or-later
#include "recent_files.hpp"

#include <QFileInfo>
#include <QSettings>

namespace {

const QString kKey = QStringLiteral("recentFiles");

void store(const QStringList& list) {
    QSettings().setValue(kKey, list);
}

}  // namespace

namespace recent {

QStringList files() {
    const QStringList saved = QSettings().value(kKey).toStringList();
    QStringList present;
    for (const QString& path : saved) {
        if (QFileInfo::exists(path)) {
            present << path;
        }
    }
    if (present.size() != saved.size()) {
        store(present);
    }
    return present;
}

void add(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QStringList list = QSettings().value(kKey).toStringList();
    list.removeAll(absolute);
    list.prepend(absolute);
    while (list.size() > kMax) {
        list.removeLast();
    }
    store(list);
}

void remove(const QString& path) {
    QStringList list = QSettings().value(kKey).toStringList();
    list.removeAll(QFileInfo(path).absoluteFilePath());
    store(list);
}

void clear() {
    store({});
}

}  // namespace recent
