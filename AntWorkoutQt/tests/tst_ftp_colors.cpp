// The zone colors (src/core/FtpColors): tests/test_colors_and_theme.py's color math, and golden/colors.json (every
// shipped theme's colors with every gradients file, fitted the same as the Python app fits them)

#include "FtpColors.hpp"
#include "TestSupport.hpp"

using namespace awo;
using namespace awo::ftpcolors;

namespace {

const ZoneRanges kRanges{{10, 20, 30, 40}};

Palette defaultPalette() {
    return fittedPalette(kDefaultZones, themeBackgrounds());
}

QHash<QString, QString> themeColors(const QString& name) {
    QFile file(awotest::pythonAppDir() + QStringLiteral("/themes/colors/") + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QHash<QString, QString> colors;
    const QJsonObject map = QJsonDocument::fromJson(file.readAll()).object()[QStringLiteral("colors")].toObject();
    for (auto it = map.begin(); it != map.end(); ++it) {
        colors.insert(it.key().toLower(), it.value().toString().toLower());
    }
    return colors;
}

Palette paletteFromJson(const QJsonArray& array) {
    Palette palette;
    for (int zone = 0; zone < kZones; ++zone) {
        const QJsonArray entry = array[zone].toArray();
        palette[zone] = {entry[0].toString(), entry[1].toString(), entry[2].toBool()};
    }
    return palette;
}

ZoneRanges rangesFromJson(const QJsonArray& array, bool cadence) {
    ZoneRanges ranges;
    ranges.cadence = cadence;
    for (const QJsonValue value : array) {
        ranges.limits.append(value.toDouble());
    }
    return ranges;
}

}  // namespace

class TestFtpColors : public QObject {
    Q_OBJECT

private slots:
    void zoneFtpFromTheTest() {
        QCOMPARE(zoneFtp(0, {}), kUnsetFtpWatts);
        QCOMPARE(zoneFtp(0, QStringLiteral("ramp20")), 100);
        QCOMPARE(zoneFtp(0, QStringLiteral("20m")), kUnsetFtpWatts);
        QCOMPARE(zoneFtp(250, QStringLiteral("ramp20")), 250);
    }

    void powerZones() {
        const ZoneRanges ranges = powerRanges(200);
        QVERIFY(!ranges.cadence);
        const QList<double> expected{72, 174, 238, 374};
        for (int i = 0; i < 4; ++i) {
            QVERIFY(std::abs(ranges.limits[i] - expected[i]) < 1e-9);
        }
    }

    void zoneOfValues() {
        QCOMPARE(zoneOf(0, kRanges), QStringLiteral("free"));
        QCOMPARE(zoneOf(5, kRanges), QStringLiteral("0"));
        QCOMPARE(zoneOf(10, kRanges), QStringLiteral("1"));
        QCOMPARE(zoneOf(29.9, kRanges), QStringLiteral("2"));
        QCOMPARE(zoneOf(30, kRanges), QStringLiteral("3"));
        QCOMPARE(zoneOf(45, kRanges), QStringLiteral("4"));
    }

    void zonePositions() {
        // (the top zone is brightest half the span of the zones below past where it starts: 40 + 15)
        const QList<std::tuple<double, int, double>> cases = {{0, 0, 0}, {5, 0, .5}, {10, 1, 0}, {15, 1, .5},
                                                              {39.9, 3, .99}, {40, 4, 0}, {55, 4, 1}, {100, 4, 1}};
        for (const auto& [value, zone, through] : cases) {
            const auto [z, t] = zonePosition(value, kRanges);
            QCOMPARE(z, zone);
            QVERIFY(std::abs(t - through) <= 0.01);
        }
    }

    void eachZoneFadesFromPaleToItsColorAndJumpsAtTheNext() {
        const Palette palette = defaultPalette();
        for (int zone = 0; zone < kZones; ++zone) {
            const auto [start, end] = zoneSpan(zone, kRanges);
            QCOMPARE(zoneHex(start, kRanges, palette), palette[zone].pale);
            QVERIFY(colors::difference(zoneHex(end - 1e-9, kRanges, palette), palette[zone].color) < 0.01);
            // closer to the color the further through the zone
            double previous = 1e9;
            for (int k = 0; k < 4; ++k) {
                const double d = colors::difference(zoneHex(start + (end - start) * k / 4, kRanges, palette),
                                                    palette[zone].color);
                QVERIFY(d <= previous);
                previous = d;
            }
            // paler: less colorful
            QVERIFY(colors::toOklch(palette[zone].pale).C <= colors::toOklch(palette[zone].color).C + 1e-6);
        }
        // the next zone starts pale, not where the last one ended
        QVERIFY(colors::difference(zoneHex(19.99, kRanges, palette), zoneHex(20, kRanges, palette)) > 0.1);
    }

    void zonesFadesDontRepeatEachOther() {
        const Palette palette = defaultPalette();
        for (int zone = 0; zone < kZones; ++zone) {
            QVERIFY(palette[zone].pale != palette[zone].color);  // (every zone keeps a fade)
            for (int other = 0; other < kZones; ++other) {
                if (other == zone) {
                    continue;
                }
                double nearest = 1e9;
                for (int k = 0; k <= 20; ++k) {
                    if (k / 20.0 >= kZoneFadeFrom) {
                        nearest = std::min(nearest, colors::difference(palette[zone].pale,
                                                                       blend(palette[other].pale, palette[other].color, k / 20.0)));
                    }
                }
                QVERIFY(nearest >= kZoneMinDifference - 0.01);
            }
        }
        // red starts red, not the peach orange has already shown
        QVERIFY(colors::toOklch(palette[4].pale).C > 0.1);
    }

    void adjustedForTheTheme() {
        // the orange zone on DSA's red background is moved to stand out; on the default background nothing moves
        const Palette dsa = fittedPalette(kDefaultZones, themeBackgrounds(themeColors(QStringLiteral("41-dsa.json"))));
        QVERIFY(dsa[3].adjusted);
        QVERIFY(dsa[3].color != kDefaultZones[3]);
        for (const ZoneColors& zone : defaultPalette()) {
            QVERIFY(!zone.adjusted);
        }
    }

    void stops() {
        const Palette palette = defaultPalette();
        const QList<std::pair<double, QColor>> stops = zoneStops(kRanges, palette);
        QCOMPARE(stops.first().first, 0.0);
        QVERIFY(std::abs(stops.last().first - 55) < 1e-9);
        for (qsizetype i = 1; i < stops.size(); ++i) {
            QVERIFY(stops[i].first >= stops[i - 1].first);
        }
        // a hard edge where each zone starts: the last stop before it the zone's color, the one at it the next's pale end
        for (int zone = 0; zone < 4; ++zone) {
            const double limit = kRanges.limits[zone];
            qsizetype i = 0;
            while (stops[i].first < limit) {
                ++i;
            }
            QCOMPARE(stops[i - 1].second.name(), palette[zone].color);
            QCOMPARE(stops[i].second.name(), palette[zone + 1].pale);
            QVERIFY(limit - stops[i - 1].first < 0.01);
        }
    }

    void blockColorsSingleAndRange() {
        const Palette palette = defaultPalette();
        // a single power: its color to its zone's pale end
        QCOMPARE(blockColors(25, 0, kRanges, palette), std::pair(zoneHex(25, kRanges, palette), palette[2].pale));
        // a range: low power's color to high power's
        QCOMPARE(blockColors(5, 45, kRanges, palette),
                 std::pair(zoneHex(5, kRanges, palette), zoneHex(45, kRanges, palette)));
    }

    void cadenceZonesStartPartWayAlongTheirFade() {
        const Palette palette = defaultPalette();
        const ZoneRanges cadence = cadenceRanges(kCadenceScaleBottomRpm);
        // 50 RPM is over 40: green
        QCOMPARE(zoneOf(50, cadence), QStringLiteral("1"));
        // 40 RPM is already clearly green, not washed out
        QCOMPARE(zoneHex(40, cadence, palette), blend(palette[1].pale, palette[1].color, kCadenceFadeFrom));
        QCOMPARE(paleHex(1, cadence, palette), zoneHex(40, cadence, palette));
        // (the power zones still start at their pale end)
        const ZoneRanges power = powerRanges(200);
        QCOMPARE(zoneHex(power.limits[0], power, palette), palette[1].pale);
    }

    void graphColors() {
        const Palette palette = defaultPalette();
        const QColor past = graphColor(25, kRanges, palette, GraphState::Past);
        QCOMPARE(past.alpha(), kGraphAlpha);
        QCOMPARE(past.name(), zoneHex(25, kRanges, palette));
        // not ridden yet: greyer, more so paused
        const double pastChroma = colors::toOklch(past.name()).C;
        const double upcomingChroma = colors::toOklch(graphColor(25, kRanges, palette, GraphState::Upcoming).name()).C;
        const double pausedChroma = colors::toOklch(graphColor(25, kRanges, palette, GraphState::Paused).name()).C;
        QVERIFY(pastChroma >= upcomingChroma && upcomingChroma >= pausedChroma);
    }

    void cadenceZoneLimits() {
        const ZoneRanges ranges = cadenceRanges(60);
        const QList<double> expected{60, 60 + 55.0 / 3, 60 + 110.0 / 3, 115};
        for (int i = 0; i < 4; ++i) {
            QVERIFY(std::abs(ranges.limits[i] - expected[i]) < 1e-9);
        }
        QCOMPARE(zoneOf(61, ranges), QStringLiteral("1"));
        QCOMPARE(zoneOf(59, ranges), QStringLiteral("0"));
        // a very high low end still leaves room for the zones
        const ZoneRanges high = cadenceRanges(200);
        QCOMPARE(high.limits.last(), 115.0);
        QCOMPARE(QSet<double>(high.limits.begin(), high.limits.end()).size(), 4);
    }

    void filePaletteDefaults() {
        const ZonePalette palette = filePalette(QJsonObject{{"zones", QJsonObject{{"1", "#123456"}}}});
        QCOMPARE(palette[1], QStringLiteral("#123456"));
        QCOMPARE(palette[2], kDefaultZones[2]);
    }

    // ---- The same as the Python app ----

    void golden() {
        const QJsonObject all = awotest::golden(QStringLiteral("colors.json")).toObject();
        for (const QJsonValue value : all[QStringLiteral("oklch")].toArray()) {
            const colors::Oklch c = colors::toOklch(value[0].toString());
            QVERIFY(awotest::sameNumber(value[1][0], c.L, 1e-12));
            QVERIFY(awotest::sameNumber(value[1][1], c.C, 1e-12));
            QVERIFY(awotest::sameNumber(value[1][2], c.h, 1e-12));
        }
        for (const QJsonValue value : all[QStringLiteral("fromOklch")].toArray()) {
            QCOMPARE(colors::fromOklch(value[0].toDouble(), value[1].toDouble(), value[2].toDouble()), value[3].toString());
        }
        for (const QJsonValue value : all[QStringLiteral("mix")].toArray()) {
            QCOMPARE(colors::mix(value[0].toString(), value[1].toString(), value[2].toDouble()), value[3].toString());
        }
        for (const QJsonValue value : all[QStringLiteral("difference")].toArray()) {
            QVERIFY(awotest::sameNumber(value[2], colors::difference(value[0].toString(), value[1].toString()), 1e-12));
        }
        for (const QJsonValue value : all[QStringLiteral("halo")].toArray()) {
            QCOMPARE(haloColor(value[0].toString()).name(QColor::HexArgb), value[1].toString());
        }
        for (const QJsonValue value : all[QStringLiteral("cadenceRanges")].toArray()) {
            const ZoneRanges ranges = cadenceRanges(value[0].toDouble());
            for (int i = 0; i < 4; ++i) {
                QVERIFY(awotest::sameNumber(value[1][i], ranges.limits[i]));
            }
        }
        for (const QJsonValue value : all[QStringLiteral("palettes")].toArray()) {
            QHash<QString, QString> colors;
            const QJsonObject map = value[QStringLiteral("themeColors")].toObject();
            for (auto it = map.begin(); it != map.end(); ++it) {
                colors.insert(it.key(), it.value().toString());
            }
            const Backgrounds backgrounds = themeBackgrounds(colors);
            const QString where = value[QStringLiteral("theme")].toString() + u'/' + value[QStringLiteral("gradient")].toString();
            QCOMPARE(backgrounds[0], value[QStringLiteral("backgrounds")][0].toString());
            QCOMPARE(backgrounds[1], value[QStringLiteral("backgrounds")][1].toString());
            ZonePalette zones;
            for (int zone = 0; zone < kZones; ++zone) {
                zones[zone] = value[QStringLiteral("zones")][zone].toString();
            }
            const Palette palette = fittedPalette(zones, backgrounds);
            const Palette expected = paletteFromJson(value[QStringLiteral("palette")].toArray());
            for (int zone = 0; zone < kZones; ++zone) {
                QVERIFY2(palette[zone] == expected[zone],
                         qPrintable(QStringLiteral("%1 zone %2: %3 %4 %5, expected %6 %7 %8").arg(where).arg(zone)
                                        .arg(palette[zone].pale, palette[zone].color).arg(palette[zone].adjusted)
                                        .arg(expected[zone].pale, expected[zone].color).arg(expected[zone].adjusted)));
            }
        }
        const QJsonArray palettes = all[QStringLiteral("palettes")].toArray();
        for (const QJsonValue value : all[QStringLiteral("zoneValues")].toArray()) {
            const Palette palette =
                paletteFromJson(palettes[value[QStringLiteral("palette")].toInt()][QStringLiteral("palette")].toArray());
            const ZoneRanges ranges =
                rangesFromJson(value[QStringLiteral("ranges")].toArray(), value[QStringLiteral("cadence")].toBool());
            for (const QJsonValue pair : value[QStringLiteral("hex")].toArray()) {
                QCOMPARE(zoneHex(pair[0].toDouble(), ranges, palette), pair[1].toString());
            }
            for (int zone = 0; zone < kZones; ++zone) {
                QCOMPARE(paleHex(zone, ranges, palette), value[QStringLiteral("pale")][zone].toString());
            }
            const QList<std::pair<double, QColor>> stops = zoneStops(ranges, palette);
            const QJsonArray expectedStops = value[QStringLiteral("stops")].toArray();
            QCOMPARE(stops.size(), expectedStops.size());
            for (qsizetype i = 0; i < stops.size(); ++i) {
                QVERIFY(awotest::sameNumber(expectedStops[i][0], stops[i].first));
                QCOMPARE(stops[i].second.name(), expectedStops[i][1].toString());
            }
            for (const QJsonValue pair : value[QStringLiteral("graph")].toArray()) {
                const QJsonArray names = pair[1].toArray();
                QCOMPARE(graphColor(pair[0].toDouble(), ranges, palette, GraphState::Paused).name(QColor::HexArgb),
                         names[0].toString());
                QCOMPARE(graphColor(pair[0].toDouble(), ranges, palette, GraphState::Upcoming).name(QColor::HexArgb),
                         names[1].toString());
                QCOMPARE(graphColor(pair[0].toDouble(), ranges, palette, GraphState::Past).name(QColor::HexArgb),
                         names[2].toString());
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestFtpColors)
#include "tst_ftp_colors.moc"
