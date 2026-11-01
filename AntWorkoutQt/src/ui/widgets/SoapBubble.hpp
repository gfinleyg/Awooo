#pragma once

// The workout graph's bubbles (MaxBubble, BlockBubble) as soap bubbles: a see-through film in the plot labels'
// background color with faint rainbow swirls drifting across it, thicker and more colorful toward its rim, a thin
// iridescent rim and a glint or two of light. When one pops its film tears open from a point on its edge, the torn
// edge racing across it, and flings a spray of droplets out that fall away and fade. Every color goes through the
// theme (kColors, which LayoutViewer lists).

#include <QPainterPath>
#include <QRectF>
#include <QStringList>

#include <functional>

class QPainter;

namespace awo::soap {

// how long a pop takes, in seconds
inline constexpr double kPopSeconds = 0.45;
// the film's rainbow (rim, swirls, droplets) and its glints' white
extern const QStringList kFilmColors;
extern const QString kGlintColor;
// every color it paints in, besides the plot labels' background and the text's black
QStringList colors();

// what's painted on the film (its text), over the glints and under the rim
using Content = std::function<void(QPainter*)>;

QPainterPath outline(const QRectF& rect, double radius);
// how far past its rect a pop's droplets may fly, in pixels: the room its bounding rect needs while popping
double popMargin(const QRectF& rect);
// the bubble filling rect, its corners rounded by radius; shimmer: seconds on its owner's clock (its swirls and rim
// colors drift with it)
void paintFilm(QPainter* painter, const QRectF& rect, double radius, double shimmer, const Content& content);
// its pop t of the way through (0 to 1); seed: where it breaks and how its droplets fly
void paintPop(QPainter* painter, const QRectF& rect, double radius, double shimmer, double t, unsigned seed,
              const Content& content);

}  // namespace awo::soap
