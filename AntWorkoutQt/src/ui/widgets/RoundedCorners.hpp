#pragma once

// Rounds some of a closed outline's corners: each one listed is cut back radius px along the outline either way (over
// as many of its points as that takes) and the gap bridged by a quarter circle's curve (close to one for a square
// corner), sampled into points, so the result is still points (QPolygonF). Used for the round ends of the gauges'
// arcs and of the power spiral's plot.

#include <QList>
#include <QPolygonF>

#include <map>

namespace awo::rounded {

// for each of outline's points, the points that stand in for it: itself, nothing (cut away by a corner nearby), or
// the corner's curve; joined up in order they're the rounded outline. corners: point index -> radius (px; under 0.5,
// left square)
QList<QPolygonF> cornerPieces(const QPolygonF& outline, const std::map<int, double>& corners);

// the pieces joined up
QPolygonF corners(const QPolygonF& outline, const std::map<int, double>& corners);

}  // namespace awo::rounded
