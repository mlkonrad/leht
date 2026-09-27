// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

/// The viewer's icons: Lucide SVGs compiled in as resources (ui/icons/lucide),
/// drawn in the palette's text colour when painted, so one set serves light
/// and dark themes and follows a theme switch without a restart.
namespace icons {

/// The icon named `lucide` (a file name in ui/icons/lucide, without ".svg").
/// When the user chose the desktop's icon theme in Preferences and the theme
/// has `freedesktop`, that wins. A name with no resource gives a null icon.
[[nodiscard]] QIcon named(const QString& lucide, const QString& freedesktop = {});

/// `lucide` drawn in `color` at `size` (device-independent pixels, for a
/// screen of `scale`), for a status mark that must keep its colour.
[[nodiscard]] QPixmap tinted(const QString& lucide, const QColor& color, int size, qreal scale = 1.0);

/// Whether icons can be drawn at all: they are SVG, read through Qt's SVG
/// image plugin (qt6-qtsvg), which a minimal install may lack. Without it the
/// toolbars show text instead.
[[nodiscard]] bool available();

/// Whether `lucide` is compiled in.
[[nodiscard]] bool has(const QString& lucide);

/// The QSettings key for "use the desktop's icon theme" (default false: the
/// theme rarely has every icon, and a mixed set looks broken).
inline constexpr const char* kUseThemeKey = "appearance/themeIcons";

}  // namespace icons
