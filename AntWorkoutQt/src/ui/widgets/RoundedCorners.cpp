#include "RoundedCorners.hpp"

#include <QLineF>

#include <algorithm>

#include <set>

namespace awo::rounded {

namespace {

// how far a quarter circle's cubic curve's handles reach toward the corner, as a share of the radius
constexpr double kCircle = 0.5523;
// points along each corner's curve
constexpr int kSamples = 10;

// the point distance px from point `from` along the outline in direction step (1, -1), and the points passed on the
// way; it stops halfway along an edge that leads to another corner (corners: their indices)
std::pair<QPointF, QList<int>> walk(const QPolygonF& outline, const std::set<int>& corners, int from, int step,
                                    double distance) {
    const int n = static_cast<int>(outline.size());
    QList<int> passed;
    int at = from;
    for (int i = 0; i < n - 1; ++i) {
        const int next = ((at + step) % n + n) % n;
        const QPointF a = outline[at];
        const QPointF b = outline[next];
        const double edge = QLineF(a, b).length();
        const bool cornerNext = corners.count(next) > 0;
        const double room = cornerNext ? edge / 2 : edge;
        if (distance <= room || cornerNext) {
            const double share = edge > 0 ? std::min(distance, room) / edge : 0;
            return {a + (b - a) * share, passed};
        }
        distance -= edge;
        passed.append(next);
        at = next;
    }
    return {outline[at], passed};
}

}  // namespace

QList<QPolygonF> cornerPieces(const QPolygonF& outline, const std::map<int, double>& corners) {
    const int n = static_cast<int>(outline.size());
    QList<QPolygonF> pieces;
    for (const QPointF& point : outline) {
        pieces.append(QPolygonF{point});
    }
    if (n < 3) {
        return pieces;
    }
    std::set<int> rounding;
    for (const auto& [index, radius] : corners) {
        if (radius >= 0.5 && 0 <= index && index < n) {
            rounding.insert(index);
        }
    }
    QList<int> cut;
    for (const int index : rounding) {
        const double radius = corners.at(index);
        const auto [in, before] = walk(outline, rounding, index, -1, radius);
        const auto [out, after] = walk(outline, rounding, index, 1, radius);
        cut.append(before);
        cut.append(after);
        const QPointF corner = outline[index];
        const QPointF c1 = in + (corner - in) * kCircle;
        const QPointF c2 = out + (corner - out) * kCircle;
        QPolygonF curve;
        for (int k = 0; k <= kSamples; ++k) {
            const double t = static_cast<double>(k) / kSamples;
            const double u = 1 - t;
            curve.append(in * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + out * (t * t * t));
        }
        pieces[index] = curve;
    }
    for (const int index : cut) {
        if (!rounding.count(index)) {
            pieces[index] = QPolygonF();
        }
    }
    return pieces;
}

QPolygonF corners(const QPolygonF& outline, const std::map<int, double>& corners) {
    QPolygonF joined;
    for (const QPolygonF& piece : cornerPieces(outline, corners)) {
        joined += piece;
    }
    return joined;
}

}  // namespace awo::rounded
