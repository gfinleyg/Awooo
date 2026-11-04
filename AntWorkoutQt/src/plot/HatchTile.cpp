#include "HatchTile.hpp"

#include <QPainter>
#include <QPixmapCache>

#include <algorithm>
#include <cmath>

#include "FtpColors.hpp"

namespace awo::plot {

QColor hatchInk(const QColor& over) {
    return ftpcolors::haloColor(over.name(), kHatchAlpha);
}

QPixmap hatchTile(const QColor& ink, double scale, double devicePixelRatio) {
    const int period = std::max(4, static_cast<int>(std::lround(kHatchSpacing * scale * devicePixelRatio)));
    const double width = std::max(1.0, kHatchWidth * scale * devicePixelRatio);
    const QString key = QStringLiteral("awoHatch_%1_%2_%3_%4")
                            .arg(ink.rgba(), 0, 16)
                            .arg(period)
                            .arg(width, 0, 'f', 3)
                            .arg(devicePixelRatio, 0, 'f', 3);
    QPixmap tile;
    if (QPixmapCache::find(key, &tile)) {
        return tile;
    }
    tile = QPixmap(period, period);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ink, width, Qt::SolidLine, Qt::FlatCap));
    // the line through the tile and the ones through its corners, whose edges come back in on the other side
    for (int i = 0; i <= 2; ++i) {
        painter.drawLine(QPointF((i - 2.0) * period, 2.0 * period), QPointF((i + 2.0) * period, -2.0 * period));
    }
    painter.end();
    tile.setDevicePixelRatio(devicePixelRatio);
    QPixmapCache::insert(key, tile);
    return tile;
}

void drawHatch(QPainter& painter, const QRect& rect, const QPixmap& tile, const QPointF& anchor) {
    if (rect.isEmpty()) {
        return;
    }
    const QSizeF size = tile.deviceIndependentSize();
    const auto wrapped = [](double value, double period) {
        const double at = std::fmod(value, period);
        return at < 0 ? at + period : at;
    };
    painter.drawTiledPixmap(rect, tile,
                            QPointF(wrapped(rect.x() - anchor.x(), size.width()),
                                    wrapped(rect.y() - anchor.y(), size.height())));
}

}  // namespace awo::plot
