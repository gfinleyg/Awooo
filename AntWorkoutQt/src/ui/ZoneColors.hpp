#pragma once

// The zone colors as the app shows them (FtpColors.py's half that follows the theme; the color math is
// awo_core's FtpColors): the zone colors with the theme's changes on top, fitted to the theme's background, and what
// each part draws with. Whatever shows them connects to notifier().changed() to be redrawn when they change.
//
// Everything colored by zone asks these: the power and cadence gauges' bands (zoneStops), their center rings, the
// Ride At target arc (gaugeArc), the ring behind the power reading (gaugeRing), the spiral, the workout block labels
// (blockColors / blockCss) and the workout graph's bars (graphColor: upcoming and paused mixed with grey).

#include <QColor>
#include <QObject>
#include <QPen>
#include <QString>

#include <optional>
#include <utility>

#include "FtpColors.hpp"

namespace awo::zones {

using ftpcolors::GraphState;
using ftpcolors::Palette;
using ftpcolors::ZonePalette;
using ftpcolors::ZoneRanges;

// the block labels' text: dark on their light zone colors whatever a theme does with the app's black text
extern const QString kBlockText;

// ---- The colors, the defaults with the theme's changes on top ----

ZonePalette zoneColors();
QString freeColor();
// a zone's color (0 - 4) changed in the theme; empty goes back to the default
void setZoneColor(int zone, const std::optional<QString>& color);
void setFreeColor(const std::optional<QString>& color);
bool isDefaultZone(int zone);
bool isDefaultFree();
bool isDefault();

// ---- The palette fitted to the theme ----

// the current zone colors fitted to the current theme's backgrounds (or to the colors and theme colors given)
Palette palette();
Palette palette(const ZonePalette& colors, const QHash<QString, QString>& themeColors);
// the zones (0 - 4) whose colors were moved to stand out on the theme
QList<int> adjustedZones(const ZonePalette& colors, const QHash<QString, QString>& themeColors);

// The ranges the app colors by: AppState's zone limits, or the power zones while they're not set
ZoneRanges appRanges();
// The FTP's power zones (the gauge's ring follows them, even in Musical Cadence Mode)
ZoneRanges powerRanges();

QString zoneHex(double value, const std::optional<ZoneRanges>& ranges = std::nullopt);
QColor zoneColor(double value, const std::optional<ZoneRanges>& ranges = std::nullopt);
QString paleHex(int zone, const std::optional<ZoneRanges>& ranges = std::nullopt);
QList<std::pair<double, QColor>> zoneStops(const ZoneRanges& ranges, int steps = 6);

// ---- What each part draws with ----

// the target arc's two colors for a power, without the "#" (the gauge adds transparency): its zone's pale end to the
// power's color
std::pair<QString, QString> gaugeArc(double power, const std::optional<ZoneRanges>& ranges = std::nullopt);
// the ring behind the power being ridden, without the "#"
QString gaugeRing(double power, const std::optional<ZoneRanges>& ranges = std::nullopt);
// the block label's text for its gradient: kBlockText, or light text where the zone colors are dark
QString blockText(const QString& start, const QString& end);
std::pair<QString, QString> blockColors(double power0, double power1 = 0,
                                        const std::optional<ZoneRanges>& ranges = std::nullopt);
QString blockCss(double power0, double power1 = 0, const std::optional<ZoneRanges>& ranges = std::nullopt);
std::pair<QString, QString> freeRideColors();
QString freeRideCss();
QColor graphColor(double value, const ZoneRanges& ranges, GraphState state);
// a graph's sensor line in color: (pen, halo pen), both cosmetic (as wide on screen whatever the graph's scale)
std::pair<QPen, QPen> sensorPens(const QString& color, double width = ftpcolors::kSensorLinePx,
                                 double haloWidth = ftpcolors::kSensorHaloPx);
// the pen outlining a sensor's current reading marker: its line's halo color
QPen markerOutline(const QString& color, double width = 1);

// ---- Redrawing what shows them ----

class Notifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};
Notifier& notifier();
// the colors changed: everything showing them is told
void notify();
// after a theme is loaded or cleared: told only if the colors or the background they're fitted to changed
void notifyIfChanged();

}  // namespace awo::zones
