// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QColor>

#include <algorithm>
#include <cmath>

/// Colours meant to be read: status words in red, amber or green keep their
/// hue but are darkened or lightened until they stand out from the background
/// by the WCAG ratio for text (4.5:1), in light, dark and high-contrast themes.
namespace contrast {

/// Relative luminance, 0 (black) to 1 (white).
inline double luminance(const QColor& c) {
    const auto channel = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

inline double ratio(const QColor& a, const QColor& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// `wanted`, as little changed as will read on `background`.
inline QColor readableOn(const QColor& wanted, const QColor& background, double needed = 4.5) {
    QColor c = wanted.toHsl();
    const bool darken = luminance(background) > 0.18;
    for (int step = 0; step < 50 && ratio(c, background) < needed; ++step) {
        const double l = c.lightnessF() + (darken ? -0.02 : 0.02);
        c.setHslF(c.hslHueF(), c.hslSaturationF(), std::clamp(l, 0.0, 1.0));
    }
    return c.toRgb();
}

}  // namespace contrast
