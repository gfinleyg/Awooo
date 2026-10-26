#include "FtpColors.hpp"

#include <QMutex>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "FtpTest.hpp"
#include "PyCompat.hpp"

namespace awo::colors {

namespace {

double toLinear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double toSrgb(double c) {
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

std::array<double, 3> linearRgb(double L, double C, double h) {
    const double a = C * std::cos(h);
    const double b = C * std::sin(h);
    const double l = std::pow(L + 0.3963377774 * a + 0.2158037573 * b, 3.0);
    const double m = std::pow(L - 0.1055613458 * a - 0.0638541728 * b, 3.0);
    const double s = std::pow(L - 0.0894841775 * a - 1.2914855480 * b, 3.0);
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

std::array<double, 3> oklab(const QString& hex) {
    const Oklch c = toOklch(hex);
    return {c.L, c.C * std::cos(c.h), c.C * std::sin(c.h)};
}

}  // namespace

Oklch toOklch(const QString& hex) {
    const QColor color(hex);
    const double r = toLinear(color.redF());
    const double g = toLinear(color.greenF());
    const double b = toLinear(color.blueF());
    const double l = std::pow(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b, 1.0 / 3);
    const double m = std::pow(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b, 1.0 / 3);
    const double s = std::pow(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b, 1.0 / 3);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    return {L, std::hypot(A, B), std::atan2(B, A)};
}

QString fromOklch(double L, double C, double h) {
    L = std::min(1.0, std::max(0.0, L));
    std::array<double, 3> rgb = linearRgb(L, C, h);
    for (int i = 0; i < 40; ++i) {
        if (std::all_of(rgb.begin(), rgb.end(), [](double c) { return -1e-4 <= c && c <= 1 + 1e-4; })) {
            break;
        }
        C *= 0.92;
        rgb = linearRgb(L, C, h);
    }
    QString hex = QStringLiteral("#");
    for (double c : rgb) {
        const double srgb = std::min(1.0, std::max(0.0, toSrgb(std::min(1.0, std::max(0.0, c)))));
        hex += QStringLiteral("%1").arg(static_cast<int>(py::round(srgb * 255)), 2, 16, QLatin1Char('0'));
    }
    return hex;
}

double difference(const QString& a, const QString& b) {
    const std::array<double, 3> p = oklab(a);
    const std::array<double, 3> q = oklab(b);
    return std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
}

QString mix(const QString& a, const QString& b, double amount) {
    const QColor x(a);
    const QColor y(b);
    // (worked out in double, handed to Qt as float: PyQt6 does the same with Python's floats)
    const double r = x.redF() + (static_cast<double>(y.redF()) - x.redF()) * amount;
    const double g = x.greenF() + (static_cast<double>(y.greenF()) - x.greenF()) * amount;
    const double bl = x.blueF() + (static_cast<double>(y.blueF()) - x.blueF()) * amount;
    return QColor::fromRgbF(static_cast<float>(r), static_cast<float>(g), static_cast<float>(bl)).name();
}

}  // namespace awo::colors

namespace awo::ftpcolors {

using colors::difference;
using colors::fromOklch;
using colors::mix;
using colors::toOklch;

const std::array<QString, kZones> kDefaultZones = {QStringLiteral("#d1d1d1"), QStringLiteral("#38813b"),
                                                   QStringLiteral("#e0c200"), QStringLiteral("#f07000"),
                                                   QStringLiteral("#d11e14")};
const QString kDefaultFree = QStringLiteral("#d9d9d9");

const QString kSensorPower = QStringLiteral("#0000ff");
const QString kSensorHr = QStringLiteral("#ff0000");
const QString kSensorCadence = QStringLiteral("#ffff00");
const QString kSensorSpeed = QStringLiteral("#ffffff");

namespace {

const std::array<QString, 2> kWindowGradient = {QStringLiteral("#28275e"), QStringLiteral("#9fc5d1")};
constexpr std::array<double, 2> kBackgroundAt{0.5, 0.85};
const QString kHaloDark = QStringLiteral("#101018");
const QString kHaloLight = QStringLiteral("#f2f2f6");
constexpr double kHaloLightBelow = 0.55;

double nearest(const QString& color, const Backgrounds& backgrounds) {
    return std::min(difference(color, backgrounds[0]), difference(color, backgrounds[1]));
}

// color with its lightness moved (the least it takes) until it looks different enough from the backgrounds
QString standOut(const QString& color, const Backgrounds& backgrounds) {
    if (nearest(color, backgrounds) >= kColorMinDifference) {
        return color;
    }
    const auto [L, C, h] = toOklch(color);
    for (int step = 1; step <= 100; ++step) {
        for (int direction : {1, -1}) {
            const QString moved = fromOklch(L + (direction * step) / 100.0, C, h);
            if (nearest(moved, backgrounds) >= kColorMinDifference) {
                return moved;
            }
        }
    }
    QString best;
    double bestDifference = -1;
    for (int step = -100; step <= 100; ++step) {
        const QString candidate = fromOklch(L + step / 100.0, C, h);
        const double d = nearest(candidate, backgrounds);
        if (d > bestDifference) {
            best = candidate;
            bestDifference = d;
        }
    }
    return best;
}

// colors along a zone's fade from kZoneFadeFrom to its color
QList<QString> fade(const QString& pale, const QString& color, int samples = 10) {
    QList<QString> along;
    for (int k = 0; k <= samples; ++k) {
        if (static_cast<double>(k) / samples >= kZoneFadeFrom) {
            along.append(blend(pale, color, static_cast<double>(k) / samples));
        }
    }
    return along;
}

}  // namespace

int zoneFtp(int ftp, const QString& ftpTestMode) {
    if (ftp) {
        return ftp;
    }
    const ftptest::Test* test = ftptest::find(ftpTestMode);
    return test && test->unsetFtpWatts ? test->unsetFtpWatts : kUnsetFtpWatts;
}

ZoneRanges powerRanges(int zoneFtp) {
    ZoneRanges ranges;
    for (double fraction : kPowerZoneFtp) {
        ranges.limits.append(zoneFtp * fraction);
    }
    return ranges;
}

ZoneRanges cadenceRanges(double low) {
    const int limits = kZones - 1;
    low = std::min(low, kCadenceScaleTopRpm - (limits - 1));  // (room for the zones between)
    const double step = (kCadenceScaleTopRpm - low) / (limits - 1);
    ZoneRanges ranges;
    ranges.cadence = true;
    for (int k = 0; k < limits; ++k) {
        ranges.limits.append(low + step * k);
    }
    return ranges;
}

QString zoneOf(double value, const ZoneRanges& ranges) {
    if (value == 0) {
        return QStringLiteral("free");
    }
    for (qsizetype i = 0; i < ranges.limits.size(); ++i) {
        if (value < ranges.limits[i]) {
            return QString::number(i);
        }
    }
    return QString::number(ranges.limits.size());
}

double topOf(const ZoneRanges& ranges) {
    return ranges.limits.last() + (ranges.limits.last() - ranges.limits.first()) * kTopZoneShare;
}

std::pair<double, double> zoneSpan(int zone, const ZoneRanges& ranges) {
    const double start = zone == 0 ? 0.0 : ranges.limits[zone - 1];
    const double end = zone < ranges.limits.size() ? ranges.limits[zone] : topOf(ranges);
    return {start, end};
}

std::pair<int, double> zonePosition(double value, const ZoneRanges& ranges) {
    const auto above = std::upper_bound(ranges.limits.begin(), ranges.limits.end(), value);
    const int zone = std::min(kZones - 1, static_cast<int>(above - ranges.limits.begin()));
    const auto [start, end] = zoneSpan(zone, ranges);
    return {zone, end > start ? std::min(1.0, std::max(0.0, (value - start) / (end - start))) : 1.0};
}

double fadeFrom(const ZoneRanges& ranges) {
    return ranges.cadence ? kCadenceFadeFrom : 0.0;
}

Backgrounds themeBackgrounds(const QHash<QString, QString>& themeColors) {
    const QString start = themeColors.value(kWindowGradient[0], kWindowGradient[0]);
    const QString end = themeColors.value(kWindowGradient[1], kWindowGradient[1]);
    return {mix(start, end, kBackgroundAt[0]), mix(start, end, kBackgroundAt[1])};
}

QString paleAt(const QString& color, double t) {
    const auto [L1, C1, h] = toOklch(color);
    const double L0 = L1 + (0.97 - L1) * kPaleLightness;
    const double C0 = C1 * kPaleChroma;
    return fromOklch(L0 + (L1 - L0) * t, C0 + (C1 - C0) * t, h);
}

QString blend(const QString& pale, const QString& color, double t) {
    auto [L0, C0, h0] = toOklch(pale);
    const auto [L1, C1, h1] = toOklch(color);
    if (C0 < 0.02) {  // (a grey has no hue to keep)
        h0 = h1;
    }
    const double dh = py::mod(h1 - h0 + std::numbers::pi, 2 * std::numbers::pi) - std::numbers::pi;
    return fromOklch(L0 + (L1 - L0) * t, C0 + (C1 - C0) * t, h0 + dh * t);
}

Palette fittedPalette(const ZonePalette& rawColors, const Backgrounds& backgrounds) {
    ZonePalette colors;
    for (int zone = 0; zone < kZones; ++zone) {
        colors[zone] = rawColors[zone].toLower();
    }
    // (worked out once for each palette and backgrounds: every redraw asks)
    static QMutex mutex;
    static QHash<QString, Palette> cache;
    const QString key = QStringList(colors.begin(), colors.end()).join(u',') + u'/' + backgrounds[0] + u',' +
                        backgrounds[1];
    {
        QMutexLocker locker(&mutex);
        const auto found = cache.constFind(key);
        if (found != cache.constEnd()) {
            return *found;
        }
    }

    std::array<QString, kZones> brights;
    for (int zone = 0; zone < kZones; ++zone) {
        brights[zone] = standOut(colors[zone], backgrounds);
    }
    // how far toward its color each pale end has been taken (only ever further)
    std::array<int, kZones> steps{};
    const int maxPull = static_cast<int>(py::round(kZoneMaxPull * kPaleSteps));
    const auto fits = [&](int zone, int step, const std::array<QList<QString>, kZones>& fades) {
        const QString pale = paleAt(brights[zone], static_cast<double>(step) / kPaleSteps);
        if (nearest(pale, backgrounds) < kPaleMinDifference) {
            return false;
        }
        if (step >= maxPull) {
            return true;
        }
        for (int other = 0; other < kZones; ++other) {
            if (other == zone) {
                continue;
            }
            for (const QString& c : fades[other]) {
                if (difference(pale, c) < kZoneMinDifference) {
                    return false;
                }
            }
        }
        return true;
    };
    // (taking one pale end back changes the fade the others are compared with: again until none moves)
    for (int round = 0; round < kZones * kPaleSteps; ++round) {
        std::array<QList<QString>, kZones> fades;
        for (int zone = 0; zone < kZones; ++zone) {
            fades[zone] = fade(paleAt(brights[zone], static_cast<double>(steps[zone]) / kPaleSteps), brights[zone]);
        }
        bool moved = false;
        for (int zone = 0; zone < kZones; ++zone) {
            while (steps[zone] < kPaleSteps && !fits(zone, steps[zone], fades)) {
                ++steps[zone];
                moved = true;
            }
        }
        if (!moved) {
            break;
        }
    }
    Palette palette;
    for (int zone = 0; zone < kZones; ++zone) {
        palette[zone] = {paleAt(brights[zone], static_cast<double>(steps[zone]) / kPaleSteps), brights[zone],
                         difference(colors[zone], brights[zone]) > kAdjustedDifference};
    }
    QMutexLocker locker(&mutex);
    cache.insert(key, palette);
    return palette;
}

ZonePalette filePalette(const QJsonObject& value) {
    const QJsonObject zones = value.value(QStringLiteral("zones")).toObject();
    ZonePalette palette;
    for (int zone = 0; zone < kZones; ++zone) {
        palette[zone] = zones.value(QString::number(zone)).toString(kDefaultZones[zone]);
    }
    return palette;
}

QString zoneHex(double value, const ZoneRanges& ranges, const Palette& palette) {
    const double start = fadeFrom(ranges);
    const auto [zone, t] = zonePosition(value, ranges);
    return blend(palette[zone].pale, palette[zone].color, start + (1 - start) * t);
}

QString paleHex(int zone, const ZoneRanges& ranges, const Palette& palette) {
    const double start = fadeFrom(ranges);
    return start ? blend(palette[zone].pale, palette[zone].color, start) : palette[zone].pale;
}

QList<std::pair<double, QColor>> zoneStops(const ZoneRanges& ranges, const Palette& palette, int steps) {
    const double fadeStart = fadeFrom(ranges);
    QList<std::pair<double, QColor>> stops;
    for (int zone = 0; zone < kZones; ++zone) {
        const auto [start, end] = zoneSpan(zone, ranges);
        const double edge = zone < kZones - 1 ? (end - start) * 1e-4 : 0;
        for (int i = 0; i <= steps; ++i) {
            stops.append({start + (end - start) * i / steps - (i == steps ? edge : 0),
                          QColor(blend(palette[zone].pale, palette[zone].color,
                                       fadeStart + (1 - fadeStart) * i / steps))});
        }
    }
    return stops;
}

std::pair<QString, QString> blockColors(double power0, double power1, const ZoneRanges& ranges, const Palette& palette) {
    const QString start = zoneHex(power0, ranges, palette);
    const QString end = power1 != 0 ? zoneHex(power1, ranges, palette)
                                    : paleHex(zonePosition(power0, ranges).first, ranges, palette);
    return {start, end};
}

QColor graphColor(double value, const ZoneRanges& ranges, const Palette& palette, GraphState state) {
    QString color = zoneHex(value, ranges, palette);
    if (state == GraphState::Paused) {
        color = mix(color, QStringLiteral("#9a9a9a"), 0.70);
    } else if (state == GraphState::Upcoming) {
        color = mix(color, QStringLiteral("#d3d3d3"), 0.45);
    }
    QColor result(color);
    result.setAlpha(kGraphAlpha);
    return result;
}

QColor haloColor(const QString& color, int alpha) {
    QColor halo(toOklch(color).L < kHaloLightBelow ? kHaloLight : kHaloDark);
    halo.setAlpha(alpha);
    return halo;
}

}  // namespace awo::ftpcolors
