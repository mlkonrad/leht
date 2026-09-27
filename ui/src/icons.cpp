// SPDX-License-Identifier: AGPL-3.0-or-later
#include "icons.hpp"

#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QImageReader>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSettings>

namespace {

QString resourcePath(const QString& name) {
    return QStringLiteral(":/leht/icons/%1.svg").arg(name);
}

/// Draws one Lucide SVG with `currentColor` replaced by the palette colour for
/// the mode it is asked for. The colour is looked up at paint time, not when
/// the icon is made, so a palette change repaints in the new colour.
class TintedSvgEngine final : public QIconEngine {
public:
    explicit TintedSvgEngine(QByteArray svg) : svg_(std::move(svg)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        const qreal scale = painter->device() != nullptr ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State /*state*/,
                         qreal scale) override {
        const QSize pixels = (QSizeF(size) * scale).toSize();
        if (pixels.isEmpty()) {
            return {};
        }
        const QColor color = colorFor(mode);
        const QString key = QStringLiteral("%1x%2:%3").arg(pixels.width()).arg(pixels.height())
                                .arg(color.rgba(), 8, 16);
        if (auto it = cache_.constFind(key); it != cache_.constEnd()) {
            return *it;
        }
        QByteArray tinted = svg_;
        tinted.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
        QBuffer buffer(&tinted);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "svg");
        reader.setScaledSize(pixels);
        QImage image = reader.read();
        if (image.isNull()) {
            return {};
        }
        if (color.alpha() != 255) {
            // The SVG plugin ignores alpha in a colour name; fade it here.
            QImage faded(image.size(), QImage::Format_ARGB32_Premultiplied);
            faded.fill(Qt::transparent);
            QPainter p(&faded);
            p.setOpacity(color.alphaF());
            p.drawImage(0, 0, image);
            p.end();
            image = faded;
        }
        QPixmap pixmap = QPixmap::fromImage(image);
        pixmap.setDevicePixelRatio(scale);
        if (cache_.size() > 32) {
            cache_.clear();
        }
        cache_.insert(key, pixmap);
        return pixmap;
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }

    QIconEngine* clone() const override { return new TintedSvgEngine(svg_); }

    QString key() const override { return QStringLiteral("leht-tinted-svg"); }

private:
    static QColor colorFor(QIcon::Mode mode) {
        const QPalette palette = QApplication::palette();
        switch (mode) {
            case QIcon::Disabled: {
                QColor c = palette.color(QPalette::Disabled, QPalette::ButtonText);
                // Some styles give disabled text the same colour as enabled
                // text and grey it elsewhere; make sure an icon looks off.
                if (c == palette.color(QPalette::Active, QPalette::ButtonText)) {
                    c.setAlphaF(0.4);
                }
                return c;
            }
            case QIcon::Selected:
                return palette.color(QPalette::Active, QPalette::HighlightedText);
            case QIcon::Normal:
            case QIcon::Active:
                break;
        }
        return palette.color(QPalette::Active, QPalette::ButtonText);
    }

    QByteArray svg_;
    QHash<QString, QPixmap> cache_;
};

}  // namespace

namespace icons {

bool available() {
    static const bool svg = QImageReader::supportedImageFormats().contains("svg");
    return svg;
}

bool has(const QString& lucide) {
    return !lucide.isEmpty() && QFile::exists(resourcePath(lucide));
}

QPixmap tinted(const QString& lucide, const QColor& color, int size, qreal scale) {
    QFile file(resourcePath(lucide));
    if (lucide.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray svg = file.readAll();
    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
    QBuffer buffer(&svg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "svg");
    reader.setScaledSize(QSize(size, size) * scale);
    QPixmap pixmap = QPixmap::fromImage(reader.read());
    pixmap.setDevicePixelRatio(scale);
    return pixmap;
}

QIcon named(const QString& lucide, const QString& freedesktop) {
    if (!freedesktop.isEmpty() && QSettings().value(QLatin1String(kUseThemeKey), false).toBool() &&
        QIcon::hasThemeIcon(freedesktop)) {
        return QIcon::fromTheme(freedesktop);
    }
    QFile file(resourcePath(lucide));
    if (lucide.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QIcon(new TintedSvgEngine(file.readAll()));
}

}  // namespace icons
