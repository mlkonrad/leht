// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

/// The files opened lately, newest first, kept in QSettings ("recentFiles").
/// Only paths are kept: nothing is read from the files until one is opened
/// again, and then only by the worker.
namespace recent {

inline constexpr int kMax = 12;

/// The list, without files that no longer exist.
[[nodiscard]] QStringList files();
/// Puts `path` (made absolute) first.
void add(const QString& path);
void remove(const QString& path);
void clear();

}  // namespace recent
