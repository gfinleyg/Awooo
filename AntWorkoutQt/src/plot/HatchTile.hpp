#pragma once

// The faint diagonal lines over what isn't being ridden (an interval's other blocks, the workout graph's bars still to
// come): one small antialiased tile for each ink and size, made once and kept in QPixmapCache, and repeated over the
// area, so the lines cost a pixmap copy to draw rather than a line drawn through every box each frame.

#include <QColor>
#include <QPixmap>
#include <QPointF>
#include <QRect>

class QPainter;

namespace awo::plot {

// design px between the lines (along a row of pixels), and how wide they are
constexpr double kHatchSpacing = 10;
constexpr double kHatchWidth = 1.5;
// the ink's alpha: 20%
constexpr int kHatchAlpha = 51;

// the ink that stands out on a color (dark on light colors, light on dark ones)
QColor hatchInk(const QColor& over);
// the tile (seamless, whole device pixels across so it repeats exactly), the same pixmap each time while it's cached
QPixmap hatchTile(const QColor& ink, double scale, double devicePixelRatio);
// rect filled with the tile in the painter's own coordinates (untransformed: a transformed tile would be resampled);
// the lines stay in place relative to anchor, wherever rect is
void drawHatch(QPainter& painter, const QRect& rect, const QPixmap& tile, const QPointF& anchor);

}  // namespace awo::plot
