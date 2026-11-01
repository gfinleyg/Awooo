#include "SoapBubble.hpp"

#include <QConicalGradient>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "Theme.hpp"

namespace awo::soap {

// pink, gold, mint, sky and violet, as a soap film's colors run
const QStringList kFilmColors = {QStringLiteral("#ff6ec7"), QStringLiteral("#ffd36e"), QStringLiteral("#7dffb0"),
                                 QStringLiteral("#6ecbff"), QStringLiteral("#b48cff")};
// (a white of its own, so a theme can set it apart from the rest of the app's white)
const QString kGlintColor = QStringLiteral("#fffffd");

namespace {

// how see-through the film is (out of 255): enough to show the graph faintly behind, not enough to lose the text
constexpr int kFilmAlpha = 185;
constexpr int kSwirlAlpha = 55;
constexpr int kEdgeAlpha = 80;
constexpr int kRimAlpha = 235;
// how fast its colors drift, in degrees a second (the rim) and its widths a second (the swirls)
constexpr double kRimDrift = 45;
constexpr double kSwirlDrift = 0.12;
// the share of a pop its film takes to tear all the way across; its droplets fly for the rest
constexpr double kTear = 0.4;
constexpr int kDroplets = 18;

QColor themed(const QString& hex, int alpha = 255) {
    QColor color(theme::current().mapHex(hex));
    color.setAlpha(alpha);
    return color;
}

QColor filmColor(int i, int alpha) {
    return themed(kFilmColors[i % kFilmColors.size()], alpha);
}

// the rainbow round its center, turning with shimmer
QConicalGradient rimGradient(const QRectF& rect, double shimmer, int alpha) {
    QConicalGradient gradient(rect.center(), std::fmod(shimmer * kRimDrift, 360.0));
    const qsizetype count = kFilmColors.size();
    for (qsizetype i = 0; i <= count; ++i) {
        gradient.setColorAt(static_cast<double>(i) / count, filmColor(static_cast<int>(i), alpha));
    }
    return gradient;
}

// the film itself, filling shape (rect's outline, or what's left of it as it tears)
void paintShape(QPainter* painter, const QPainterPath& shape, const QRectF& rect, double radius, double shimmer,
                const Content& content, double fade = 1.0) {
    const double height = rect.height();
    painter->fillPath(shape, themed(theme::kPlotLabelBg, static_cast<int>(kFilmAlpha * fade)));
    // swirls of color drifting across it, faint
    const double drift = std::fmod(shimmer * kSwirlDrift, 2.0) * rect.width();
    QLinearGradient swirl(rect.left() + drift, rect.top(), rect.left() + drift + rect.width() * 0.6, rect.bottom());
    swirl.setSpread(QGradient::ReflectSpread);
    const qsizetype count = kFilmColors.size();
    for (qsizetype i = 0; i < count; ++i) {
        swirl.setColorAt(static_cast<double>(i) / (count - 1),
                         filmColor(static_cast<int>(i), static_cast<int>(kSwirlAlpha * fade)));
    }
    painter->fillPath(shape, swirl);
    // thicker toward its rim, where the colors are stronger: a broad stroke round it, only its inside half showing
    painter->save();
    painter->setClipPath(shape, Qt::IntersectClip);
    painter->strokePath(shape, QPen(QBrush(rimGradient(rect, shimmer + 2, static_cast<int>(kEdgeAlpha * fade))),
                                    std::max(2.0, height * 0.34)));
    painter->restore();
    // a soft glint up at its left end and a small one down at its right
    painter->save();
    painter->setClipPath(shape, Qt::IntersectClip);
    painter->setPen(Qt::NoPen);
    const double glintRadius = std::min(radius, rect.width() / 2);
    const QPointF big(rect.left() + glintRadius * 0.95, rect.top() + height * 0.3);
    QRadialGradient bigGlint(big, glintRadius * 0.75);
    bigGlint.setColorAt(0, themed(kGlintColor, static_cast<int>(230 * fade)));
    bigGlint.setColorAt(0.45, themed(kGlintColor, static_cast<int>(110 * fade)));
    bigGlint.setColorAt(1, themed(kGlintColor, 0));
    painter->setBrush(bigGlint);
    painter->translate(big);
    painter->rotate(-25);
    painter->scale(1.0, 0.45);
    painter->drawEllipse(QPointF(0, 0), glintRadius * 0.75, glintRadius * 0.75);
    painter->restore();
    painter->save();
    painter->setPen(Qt::NoPen);
    const QPointF small(rect.right() - glintRadius * 0.7, rect.bottom() - height * 0.24);
    QRadialGradient smallGlint(small, height * 0.1);
    smallGlint.setColorAt(0, themed(kGlintColor, static_cast<int>(200 * fade)));
    smallGlint.setColorAt(1, themed(kGlintColor, 0));
    painter->setBrush(smallGlint);
    painter->setClipPath(shape, Qt::IntersectClip);
    painter->drawEllipse(small, height * 0.1, height * 0.1);
    painter->restore();
    if (content) {
        painter->save();
        painter->setClipPath(shape, Qt::IntersectClip);
        content(painter);
        painter->restore();
    }
    // its rim: a thin rainbow line, with a fainter white one just inside it
    painter->save();
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(QBrush(rimGradient(rect, shimmer, static_cast<int>(kRimAlpha * fade))), 1.4));
    painter->drawPath(shape);
    painter->setClipPath(shape, Qt::IntersectClip);
    painter->setPen(QPen(themed(kGlintColor, static_cast<int>(120 * fade)), 0.8));
    painter->drawPath(outline(rect.adjusted(1.5, 1.5, -1.5, -1.5), std::max(0.0, radius - 1.5)));
    painter->restore();
}

double easeOut(double t) {
    return 1 - (1 - t) * (1 - t);
}

}  // namespace

QStringList colors() {
    return kFilmColors + QStringList{kGlintColor};
}

QPainterPath outline(const QRectF& rect, double radius) {
    QPainterPath path;
    const double r = std::min({radius, rect.width() / 2, rect.height() / 2});
    path.addRoundedRect(rect, r, r);
    return path;
}

double popMargin(const QRectF& rect) {
    return rect.height() * 1.3 + 2;
}

void paintFilm(QPainter* painter, const QRectF& rect, double radius, double shimmer, const Content& content) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    paintShape(painter, outline(rect, radius), rect, radius, shimmer, content);
    painter->restore();
}

void paintPop(QPainter* painter, const QRectF& rect, double radius, double shimmer, double t, unsigned seed,
              const Content& content) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QRandomGenerator random(seed);
    const QPainterPath shape = outline(rect, radius);
    // where it breaks: somewhere along its top or bottom edge
    const QPointF burst(rect.left() + rect.width() * (0.15 + 0.7 * random.generateDouble()),
                        random.bounded(2) ? rect.top() : rect.bottom());
    double reach = 0;
    for (const QPointF& corner : {rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()}) {
        reach = std::max(reach, std::hypot(corner.x() - burst.x(), corner.y() - burst.y()));
    }
    // the hole racing across it, fastest at first, what's left of the film fading a little as it goes
    const double tear = std::min(1.0, t / kTear);
    if (tear < 1) {
        const double hole = reach * easeOut(tear);
        QPainterPath gap;
        gap.addEllipse(burst, hole, hole);
        const QPainterPath left = shape.subtracted(gap);
        paintShape(painter, left, rect, radius, shimmer, content, 1 - 0.4 * tear);
        // its torn edge, gathered into a bright band as it pulls back
        painter->save();
        painter->setClipPath(shape, Qt::IntersectClip);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QBrush(rimGradient(rect, shimmer + 1, static_cast<int>(kRimAlpha * (1 - tear)))),
                             1 + 1.5 * (1 - tear)));
        painter->drawEllipse(burst, hole, hole);
        painter->restore();
    }
    // a faint ghost of its outline spreading out and gone
    if (t < 0.35) {
        const double s = t / 0.35;
        const double grow = rect.height() * 0.25 * easeOut(s);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QBrush(rimGradient(rect, shimmer, static_cast<int>(140 * (1 - s)))), 1));
        painter->drawPath(outline(rect.adjusted(-grow, -grow, grow, grow), radius + grow));
    }
    // droplets thrown off its edge as the tear reaches each, flying out from the break and falling away as they fade
    painter->setPen(Qt::NoPen);
    const QPointF center = rect.center();
    const double height = rect.height();
    for (int i = 0; i < kDroplets; ++i) {
        const double angle = 2 * std::numbers::pi * (i + random.generateDouble() * 0.8) / kDroplets;
        const QPointF from(center.x() + std::cos(angle) * rect.width() / 2, center.y() + std::sin(angle) * height / 2);
        const double away = std::hypot(from.x() - burst.x(), from.y() - burst.y());
        const double thrown = kTear * away / std::max(reach, 1.0);
        const double speed = 0.4 + 0.6 * random.generateDouble();
        const double size = std::max(1.0, height * (0.07 + 0.07 * random.generateDouble()));
        if (t < thrown) {
            continue;
        }
        const double s = std::min(1.0, (t - thrown) / std::max(1 - thrown, 0.2));
        if (s >= 1) {
            continue;
        }
        // out from its center and away from the break
        QPointF out(std::cos(angle), std::sin(angle));
        if (away > 0) {
            out = (out + (from - burst) / away) / 2;
        }
        const double length = std::max(std::hypot(out.x(), out.y()), 0.01);
        const double flown = height * 0.9 * speed * easeOut(s);
        const QPointF at = from + out / length * flown + QPointF(0, height * 0.35 * s * s);
        const double r = size * (1 - 0.5 * s);
        const int alpha = static_cast<int>(230 * (1 - s));
        painter->setBrush(filmColor(i, alpha));
        painter->drawEllipse(at, r, r);
        painter->setBrush(themed(kGlintColor, alpha));
        painter->drawEllipse(at - QPointF(r * 0.35, r * 0.35), r * 0.35, r * 0.35);
    }
    painter->restore();
}

}  // namespace awo::soap
