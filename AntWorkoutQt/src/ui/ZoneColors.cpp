#include "ZoneColors.hpp"

#include <QJsonObject>

#include "AppState.hpp"
#include "Theme.hpp"

namespace awo::zones {

const QString kBlockText = QStringLiteral("#111111");

namespace {

QJsonObject& changes() {
    return theme::current().ftp;
}

// what the last notify() was for
std::optional<QJsonObject> lastApplied;
std::optional<ftpcolors::Backgrounds> lastBackgrounds;

ZoneRanges orApp(const std::optional<ZoneRanges>& ranges) {
    return ranges && !ranges->isEmpty() ? *ranges : appRanges();
}

QPen cosmeticPen(const QColor& color, double width) {
    QPen pen(color, width);
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

}  // namespace

ZonePalette zoneColors() {
    ZonePalette colors = ftpcolors::kDefaultZones;
    const QJsonObject zones = changes().value(QStringLiteral("zones")).toObject();
    for (int zone = 0; zone < ftpcolors::kZones; ++zone) {
        const QJsonValue value = zones.value(QString::number(zone));
        if (value.isString()) {
            colors[zone] = value.toString();
        }
    }
    return colors;
}

QString freeColor() {
    const QJsonValue value = changes().value(QStringLiteral("free"));
    return value.isString() ? value.toString() : ftpcolors::kDefaultFree;
}

void setZoneColor(int zone, const std::optional<QString>& color) {
    QJsonObject zones = changes().value(QStringLiteral("zones")).toObject();
    if (!color || *color == ftpcolors::kDefaultZones[zone]) {
        zones.remove(QString::number(zone));
    } else {
        zones.insert(QString::number(zone), *color);
    }
    if (zones.isEmpty()) {
        changes().remove(QStringLiteral("zones"));
    } else {
        changes().insert(QStringLiteral("zones"), zones);
    }
    notify();
}

void setFreeColor(const std::optional<QString>& color) {
    if (!color || *color == ftpcolors::kDefaultFree) {
        changes().remove(QStringLiteral("free"));
    } else {
        changes().insert(QStringLiteral("free"), *color);
    }
    notify();
}

bool isDefaultZone(int zone) {
    return !changes().value(QStringLiteral("zones")).toObject().contains(QString::number(zone));
}

bool isDefaultFree() {
    return !changes().contains(QStringLiteral("free"));
}

bool isDefault() {
    return changes().isEmpty();
}

Palette palette() {
    return palette(zoneColors(), theme::current().colors);
}

Palette palette(const ZonePalette& colors, const QHash<QString, QString>& themeColors) {
    return ftpcolors::fittedPalette(colors, ftpcolors::themeBackgrounds(themeColors));
}

QList<int> adjustedZones(const ZonePalette& colors, const QHash<QString, QString>& themeColors) {
    QList<int> adjusted;
    const Palette fitted = palette(colors, themeColors);
    for (int zone = 0; zone < ftpcolors::kZones; ++zone) {
        if (fitted[zone].adjusted) {
            adjusted << zone;
        }
    }
    return adjusted;
}

ZoneRanges powerRanges() {
    const AppState& state = appState();
    return ftpcolors::powerRanges(ftpcolors::zoneFtp(state.ftp, state.ftpTestMode));
}

ZoneRanges appRanges() {
    const ZoneRanges& ranges = appState().ftpRanges;
    return ranges.isEmpty() ? powerRanges() : ranges;
}

QString zoneHex(double value, const std::optional<ZoneRanges>& ranges) {
    return ftpcolors::zoneHex(value, orApp(ranges), palette());
}

QColor zoneColor(double value, const std::optional<ZoneRanges>& ranges) {
    return QColor(zoneHex(value, ranges));
}

QString paleHex(int zone, const std::optional<ZoneRanges>& ranges) {
    return ftpcolors::paleHex(zone, orApp(ranges), palette());
}

QList<std::pair<double, QColor>> zoneStops(const ZoneRanges& ranges, int steps) {
    return ftpcolors::zoneStops(ranges, palette(), steps);
}

std::pair<QString, QString> gaugeArc(double power, const std::optional<ZoneRanges>& ranges) {
    if (power == 0) {
        const QString free = freeColor().mid(1);
        return {free, free};
    }
    const ZoneRanges zoneRanges = orApp(ranges);
    const int zone = ftpcolors::zonePosition(power, zoneRanges).first;
    return {paleHex(zone, zoneRanges).mid(1), zoneHex(power, zoneRanges).mid(1)};
}

QString gaugeRing(double power, const std::optional<ZoneRanges>& ranges) {
    if (power == 0) {
        return freeColor().mid(1);
    }
    return zoneHex(power, ranges).mid(1);
}

QString blockText(const QString& start, const QString& end) {
    const QColor middle(colors::mix(start, end, 0.5));
    return theme::contrast(middle, QColor(kBlockText)) >= theme::contrast(middle, QColor(theme::kFixedLightText))
               ? kBlockText
               : theme::kFixedLightText;
}

std::pair<QString, QString> blockColors(double power0, double power1, const std::optional<ZoneRanges>& ranges) {
    return ftpcolors::blockColors(power0, power1, orApp(ranges), palette());
}

QString blockCss(double power0, double power1, const std::optional<ZoneRanges>& ranges) {
    const auto [start, end] = blockColors(power0, power1, ranges);
    return QStringLiteral("background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 %1 stop:1 %2); color:%3;")
        .arg(start, end, blockText(start, end));
}

std::pair<QString, QString> freeRideColors() {
    return {freeColor(), QStringLiteral("#ffffff")};
}

QString freeRideCss() {
    const auto [start, end] = freeRideColors();
    return QStringLiteral("background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 %1 stop:1 %2); color:%3;")
        .arg(start, end, blockText(start, end));
}

QColor graphColor(double value, const ZoneRanges& ranges, GraphState state) {
    return ftpcolors::graphColor(value, ranges, palette(), state);
}

std::pair<QPen, QPen> sensorPens(const QString& color, double width, double haloWidth) {
    return {cosmeticPen(QColor(color), width), cosmeticPen(ftpcolors::haloColor(color), haloWidth)};
}

QPen markerOutline(const QString& color, double width) {
    return cosmeticPen(ftpcolors::haloColor(color, 0xff), width);
}

Notifier& notifier() {
    static Notifier instance;
    return instance;
}

void notify() {
    lastApplied = changes();
    lastBackgrounds = ftpcolors::themeBackgrounds(theme::current().colors);
    emit notifier().changed();
}

void notifyIfChanged() {
    if (!lastApplied || *lastApplied != changes() ||
        !lastBackgrounds || *lastBackgrounds != ftpcolors::themeBackgrounds(theme::current().colors)) {
        notify();
    }
}

}  // namespace awo::zones
