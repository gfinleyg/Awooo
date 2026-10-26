#pragma once

// The colors that follow the zones (FtpColors.py's color math; the current theme's palette and the pens are the UI's,
// PLAN.md §3.1). There are five zones, 0 to 4, split at the zone limits: the power zones at 36/87/119/187% of FTP, or
// in Musical Cadence Mode the cadence zones (cadenceRanges). Each zone has one color, its brightest, and fades on its
// own: pale where the zone starts (the same hue, washed out), its color where it ends (the top zone's at topOf()).
//
// The colors are fitted to the theme (fittedPalette): a color too close to the window's background behind the
// gauges, spiral and graph has its lightness moved until it stands out, and a pale end too faint, or looking like
// somewhere along another zone's fade (orange and red look alike washed out), is taken back toward its zone's color.
// The hue is kept. A palette whose colors moved noticeably for a theme is "adjusted" for it.

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <array>
#include <utility>

namespace awo::colors {

// OKLab/OKLCH (lightness, chroma, hue), where equal steps look equal
struct Oklch {
    double L = 0;
    double C = 0;
    double h = 0;
};

Oklch toOklch(const QString& hex);
// back to "#rrggbb"; a color past what the screen shows loses chroma, keeping its lightness and hue
QString fromOklch(double L, double C, double h);
// how different two colors look (OKLab distance: hue counts, not just lightness)
double difference(const QString& a, const QString& b);
// a taken amount of the way to b
QString mix(const QString& a, const QString& b, double amount);

}  // namespace awo::colors

namespace awo::ftpcolors {

constexpr int kZones = 5;

// the default colors of the zones, and of free ride (power 0: the block labels with no target)
extern const std::array<QString, kZones> kDefaultZones;
extern const QString kDefaultFree;

// The zone limits (4 of them), and whether they're Musical Cadence Mode's: its zones start part way along their fade
// (kCadenceFadeFrom), so a cadence just into a zone already shows its color; the power zones start at their pale end
struct ZoneRanges {
    QList<double> limits;
    bool cadence = false;

    bool isEmpty() const { return limits.isEmpty(); }
    bool operator==(const ZoneRanges&) const = default;
};

// the FTP the zones are worked out from while none is set (a ramp FTP test has its own)
constexpr int kUnsetFtpWatts = 50;
// the power zones' upper limits as fractions of FTP
constexpr std::array<double, 4> kPowerZoneFtp{.36, .87, 1.19, 1.87};
// the top zone is at its brightest this far past where it starts, as a share of the span of the zones below it
constexpr double kTopZoneShare = 0.5;
// Musical Cadence Mode's zones: zone 0 under the bottom, the top zone from the top, the ones between evenly spread
constexpr double kCadenceScaleBottomRpm = 40;
constexpr double kCadenceScaleTopRpm = 115;
constexpr double kCadenceFadeFrom = 0.5;

// The FTP the power zones come from: the FTP, or while it isn't set the selected ramp test's (kUnsetFtpWatts otherwise)
int zoneFtp(int ftp, const QString& ftpTestMode);
ZoneRanges powerRanges(int zoneFtp);
ZoneRanges cadenceRanges(double low);
// "free" for 0, else "0" to "4"
QString zoneOf(double value, const ZoneRanges& ranges);
double topOf(const ZoneRanges& ranges);
// where a zone (0 - 4) is palest and where it's its color
std::pair<double, double> zoneSpan(int zone, const ZoneRanges& ranges);
// the zone (0 - 4) and how far through it (0 - 1)
std::pair<int, double> zonePosition(double value, const ZoneRanges& ranges);
double fadeFrom(const ZoneRanges& ranges);

// the window's background gradient, and how far along it the colors have to stand out from it: the gauges and spiral
// sit around the middle, the graph toward the end
constexpr double kColorMinDifference = 0.16;
constexpr double kPaleMinDifference = 0.09;
constexpr double kZoneMinDifference = 0.08;
constexpr double kZoneFadeFrom = 0.3;
constexpr double kZoneMaxPull = 0.75;
constexpr double kPaleChroma = 0.35;
constexpr double kPaleLightness = 0.6;
constexpr double kAdjustedDifference = 0.06;
constexpr int kPaleSteps = 20;

using Backgrounds = std::array<QString, 2>;
// The backgrounds with a theme's colors (original -> replacement)
Backgrounds themeBackgrounds(const QHash<QString, QString>& themeColors = {});

struct ZoneColors {
    QString pale;
    QString color;
    bool adjusted = false;

    bool operator==(const ZoneColors&) const = default;
};
using Palette = std::array<ZoneColors, kZones>;
using ZonePalette = std::array<QString, kZones>;

// Each zone's pale end, color and whether it moved, for colors on backgrounds
Palette fittedPalette(const ZonePalette& colors, const Backgrounds& backgrounds);
// A gradients file's (or theme's "ftpColors") zone colors, the defaults for any it leaves out
ZonePalette filePalette(const QJsonObject& value);

// color washed out (t = 0, its palest) to the color itself (t = 1)
QString paleAt(const QString& color, double t);
// pale to color, t of the way along, in OKLCH
QString blend(const QString& pale, const QString& color, double t);

// A value's color: its zone's pale end where the zone starts, its zone's color where it ends
QString zoneHex(double value, const ZoneRanges& ranges, const Palette& palette);
// A zone's pale end (where its fade starts, see fadeFrom)
QString paleHex(int zone, const ZoneRanges& ranges, const Palette& palette);
// A gauge band's (value, color) stops: each zone fading from its pale end to its color, a hard edge at the next
QList<std::pair<double, QColor>> zoneStops(const ZoneRanges& ranges, const Palette& palette, int steps = 6);

// A block label's background (start, end): a range from its low power's color to its high power's, a single power
// from its color to its zone's pale end
std::pair<QString, QString> blockColors(double power0, double power1, const ZoneRanges& ranges, const Palette& palette);

// the graph's bars: a little see-through, the ones not ridden yet mixed with grey, more so paused
enum class GraphState { Paused, Upcoming, Past };
constexpr int kGraphAlpha = 0xcc;
QColor graphColor(double value, const ZoneRanges& ranges, const Palette& palette, GraphState state);

// the graphs' sensor lines: each over a halo that sets it apart from whatever is under it, light under a dark line
extern const QString kSensorPower, kSensorHr, kSensorCadence, kSensorSpeed;
constexpr double kSensorLinePx = 2;
constexpr double kSensorHaloPx = 3.5;
constexpr int kHaloAlpha = 0x80;
QColor haloColor(const QString& color, int alpha = kHaloAlpha);

}  // namespace awo::ftpcolors
