// The theme (src/ui/Theme) and the zone colors under it (src/ui/ZoneColors): tests/test_colors_and_theme.py's theme
// half, and its zone color tests that follow the theme

#include <QDirIterator>
#include <QGraphicsLineItem>
#include <QLabel>
#include <QSignalSpy>

#include "AppState.hpp"
#include "Logger.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "ZoneColors.hpp"

using namespace awo;

namespace {

const ftpcolors::ZoneRanges kRanges{{10, 20, 30, 40}};

void writeJson(const QString& path, const QJsonObject& data) {
    awotest::writeFile(path, QJsonDocument(data).toJson());
}

QStringList shippedFiles(const QString& folder) {
    QStringList files;
    QDirIterator it(awotest::pythonAppDir() + QStringLiteral("/themes/") + folder, {QStringLiteral("*.json")}, QDir::Files);
    while (it.hasNext()) {
        files << it.next();
    }
    files.sort();
    return files;
}

}  // namespace

class TestTheme : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        theme::current().reset();
        theme::forgetFiles();
        appState() = AppState();
    }

    void cleanup() {
        theme::current().reset();
        theme::forgetFiles();
        dir_.reset();
    }

    // ---- The theme ----

    void mapHex() {
        theme::Theme t;
        t.colors = {{QStringLiteral("#9fc5d1"), QStringLiteral("#ffcc66")}};
        QCOMPARE(t.mapHex(QStringLiteral("#9FC5D1")), QStringLiteral("#ffcc66"));
        // Qt's #aarrggbb keeps its alpha
        QCOMPARE(t.mapHex(QStringLiteral("#809fc5d1")), QStringLiteral("#80ffcc66"));
        QCOMPARE(t.mapHex(QStringLiteral("#123456")), QStringLiteral("#123456"));
        QCOMPARE(t.mapColor(QColor(159, 197, 209)).name(), QStringLiteral("#ffcc66"));
    }

    void cssColorsFontsAndLengths() {
        theme::Theme t;
        t.colors = {{QStringLiteral("#9fc5d1"), QStringLiteral("#ffcc66")}, {QStringLiteral("#000000"), QStringLiteral("#222222")}};
        t.fonts[theme::kRoleMain] = {QStringLiteral("Inter"), 150, QStringLiteral("700")};
        const QString css = t.css(QStringLiteral("QLabel {font: bold 20pt 'Orbitron'; color: #9fc5d1; background-color: black; "
                                                 "padding: 4px; margin: 0px; border-radius: -3px;}"),
                                  0.5);
        QVERIFY2(css.contains(QStringLiteral("font: 700 15pt 'Inter'")), qPrintable(css));
        QVERIFY(css.contains(QStringLiteral("color: #ffcc66")) && css.contains(QStringLiteral("background-color: #222222")));
        QVERIFY(css.contains(QStringLiteral("padding: 2px")) && css.contains(QStringLiteral("margin: 0px")) &&
                css.contains(QStringLiteral("border-radius: -2px")));
        // a small length never rounds away to nothing
        QCOMPARE(t.css(QStringLiteral("padding: 1px"), 0.1), QStringLiteral("padding: 1px"));
        // halves to even, as Python's round()
        QCOMPARE(t.css(QStringLiteral("padding: 5px"), 0.5), QStringLiteral("padding: 2px"));
    }

    void cssUnknownFontKeepsItsFamily() {
        QCOMPARE(theme::Theme().css(QStringLiteral("font: 10pt 'Arial'"), 2), QStringLiteral("font: 20pt 'Arial'"));
    }

    void mapFont() {
        theme::Theme t;
        t.fonts[theme::kRoleButton] = {QStringLiteral("Inter"), 200, QStringLiteral("300")};
        QFont font(QStringLiteral("Segoe UI Black"));
        font.setPointSizeF(10);
        const QFont themed = t.mapFont(font);
        QCOMPARE(themed.family(), QStringLiteral("Inter"));
        QCOMPARE(themed.pointSizeF(), 20.0);
        QCOMPARE(themed.weight(), 300);
        QCOMPARE(t.mapFont(QFont(QStringLiteral("Arial"))).family(), QStringLiteral("Arial"));
    }

    void jsonRoundTrip() {
        theme::Theme t;
        t.fromJson(QJsonObject{{"fonts", QJsonObject{{"main", QJsonObject{{"family", "Inter"}}}, {"bogus", QJsonObject{{"family", "x"}}}}},
                               {"colors", QJsonObject{{"#AABBCC", "#DDEEFF"}}},
                               {"ftpColors", QJsonObject{{"free", "#000000"}, {"blocks", QJsonObject{}}}},
                               {"layouts", QJsonObject{{"old", 1}}}});
        QCOMPARE(t.fonts[theme::kRoleMain].family, QStringLiteral("Inter"));
        QVERIFY(!t.fonts.contains(QStringLiteral("bogus")));
        QCOMPARE(t.colors, (QHash<QString, QString>{{QStringLiteral("#aabbcc"), QStringLiteral("#ddeeff")}}));
        QCOMPARE(QStringList(t.toJson().keys()), (QStringList{"colors", "fonts", "ftpColors"}));
        QCOMPARE(t.toJson(QStringLiteral("colors")), (QJsonObject{{"colors", QJsonObject{{"#aabbcc", "#ddeeff"}}}}));
        // (older gradient entries are dropped)
        QCOMPARE(t.toJson(QStringLiteral("ftpColors")), (QJsonObject{{"ftpColors", QJsonObject{{"free", "#000000"}}}}));
    }

    void colorsInCss() {
        QCOMPARE(theme::colorsInCss(QStringLiteral("color: #ABCDEF; background: #80123456; border-color: red")),
                 (QSet<QString>{QStringLiteral("#abcdef"), QStringLiteral("#123456"), QStringLiteral("#ff0000")}));
    }

    void fileKinds() {
        QCOMPARE(theme::fileKind({{"fonts", QJsonObject{}}}), QStringLiteral("theme"));
        QCOMPARE(theme::fileKind({{"colors", QJsonObject{}}}), QStringLiteral("colors"));
        QCOMPARE(theme::fileKind({{"ftpColors", QJsonObject{}}}), QStringLiteral("ftpColors"));
        QCOMPARE(theme::fileKind({{"colors", QJsonObject{}}, {"ftpColors", QJsonObject{}}}), QStringLiteral("theme"));
        QVERIFY_THROWS_EXCEPTION(theme::ThemeError, theme::fileKind({{"something", 1}}));
    }

    void loadAndSaveFiles() {
        theme::current().colors = {{QStringLiteral("#111111"), QStringLiteral("#222222")}};
        theme::saveFile(QStringLiteral("colors.json"), QStringLiteral("colors"));
        QCOMPARE(theme::readFile(QStringLiteral("colors.json")), (QJsonObject{{"colors", QJsonObject{{"#111111", "#222222"}}}}));
        theme::current().reset();
        QCOMPARE(theme::loadFile(QStringLiteral("colors.json")), QStringLiteral("colors"));
        QCOMPARE(theme::current().colors.value(QStringLiteral("#111111")), QStringLiteral("#222222"));
        QCOMPARE(theme::partFiles(), (QMap<QString, QString>{{QStringLiteral("colors"), QStringLiteral("colors.json")}}));
        // a whole theme drops the part files
        writeJson(QStringLiteral("theme.json"), {{"fonts", QJsonObject{{"main", QJsonObject{{"family", "Inter"}}}}}});
        QCOMPARE(theme::loadFile(QStringLiteral("theme.json")), QStringLiteral("theme"));
        QVERIFY(theme::partFiles().isEmpty() && theme::current().colors.isEmpty());
        awotest::writeFile(QStringLiteral("list.json"), "[1, 2]");
        QVERIFY_THROWS_EXCEPTION(theme::ThemeError, theme::loadFile(QStringLiteral("list.json")));
    }

    void defaultThemeAndClear() {
        QVERIFY(theme::defaultSource().isEmpty());
        theme::current().colors = {{QStringLiteral("#111111"), QStringLiteral("#222222")}};
        theme::clear();
        QVERIFY(theme::current().colors.isEmpty());
        writeJson(theme::kDefaultThemeFile, {{"fonts", QJsonObject{{"main", QJsonObject{{"family", "Inter"}}}}},
                                             {"colors", QJsonObject{{"#333333", "#444444"}}}});
        // part files stay on top when asked
        writeJson(QStringLiteral("grad.json"), {{"ftpColors", QJsonObject{{"free", "#555555"}}}});
        theme::loadFile(QStringLiteral("grad.json"));
        theme::clear(true);
        QCOMPARE(theme::current().colors, (QHash<QString, QString>{{QStringLiteral("#333333"), QStringLiteral("#444444")}}));
        QCOMPARE(theme::current().ftp, (QJsonObject{{"free", "#555555"}}));
        // a broken default theme falls back to the built in one, and says so
        awotest::writeFile(theme::kDefaultThemeFile, "{broken");
        theme::clear();
        QVERIFY(theme::current().colors.isEmpty());
        QVERIFY(log::read().contains(QStringLiteral("Failed to load default theme")));
    }

    void vanishedPartFileIsDropped() {
        writeJson(QStringLiteral("c.json"), {{"colors", QJsonObject{{"#111111", "#222222"}}}});
        theme::loadFile(QStringLiteral("c.json"));
        QFile::remove(QStringLiteral("c.json"));
        theme::clear(true);
        QVERIFY(theme::partFiles().isEmpty());
        QVERIFY(log::read().contains(QStringLiteral("Failed to reload colors")));
    }

    void snapshotAndRestore() {
        writeJson(QStringLiteral("c.json"), {{"colors", QJsonObject{{"#111111", "#222222"}}}});
        theme::loadFile(QStringLiteral("c.json"));
        const theme::Snapshot saved = theme::snapshot();
        theme::reset();
        QVERIFY(theme::current().colors.isEmpty());
        theme::restore(saved);
        QCOMPARE(theme::current().colors.value(QStringLiteral("#111111")), QStringLiteral("#222222"));
        QCOMPARE(theme::partFiles().value(QStringLiteral("colors")), QStringLiteral("c.json"));
    }

    void shippedThemePartsLoad() {
        QStringList files = shippedFiles(QStringLiteral("colors")) + shippedFiles(QStringLiteral("gradients"));
        QVERIFY(files.size() > 10);
        for (const QString& path : files) {
            const QJsonObject data = theme::readFile(path);
            const QString kind = theme::fileKind(data);
            QVERIFY2(theme::kParts.contains(kind), qPrintable(path));
            if (kind == u"ftpColors") {
                for (const QString& key : data.value(kind).toObject().keys()) {
                    QVERIFY2(key == u"zones" || key == u"free", qPrintable(path + u' ' + key));
                }
            }
        }
    }

    void shippedColorsSetEveryColorAndRead() {
        // text reads on what it sits on, in every shipped colors file: (text, background, or a gradient's ends and
        // where on it the text sits)
        struct Pair {
            QString text;
            QString start;
            QString end;
            double at;
        };
        QList<Pair> pairs = {{QStringLiteral("#000000"), QStringLiteral("#bdd6de"), {}, 0},
                             {QStringLiteral("#000000"), QStringLiteral("#9fc5d1"), {}, 0},
                             {theme::kButtonText, QStringLiteral("#1cb5e0"), theme::kPressed, 0.5}};
        for (double at : {0.35, 0.5, 0.65}) {
            pairs.append({theme::kButtonText, QStringLiteral("#1cb5e0"), QStringLiteral("#dadbde"), at});
        }
        for (double at : {0.0, 0.25, 0.5}) {
            pairs.append({theme::kLightText, QStringLiteral("#28275e"), QStringLiteral("#9fc5d1"), at});
        }
        for (const QString& path : shippedFiles(QStringLiteral("colors"))) {
            QHash<QString, QString> colors;
            const QJsonObject map = theme::readFile(path).value(QStringLiteral("colors")).toObject();
            for (auto it = map.begin(); it != map.end(); ++it) {
                colors.insert(it.key().toLower(), it.value().toString().toLower());
            }
            for (const QString& key : theme::kThemeKeys) {
                QVERIFY2(colors.contains(key), qPrintable(path + u' ' + key));
            }
            QVERIFY(!colors.contains(theme::kFixedDarkText) && !colors.contains(theme::kFixedLightText) &&
                    !colors.contains(zones::kBlockText));
            const auto themed = [&](const QString& color) { return colors.value(color, color); };
            for (const Pair& pair : pairs) {
                const QString background =
                    pair.end.isEmpty() ? themed(pair.start) : colors::mix(themed(pair.start), themed(pair.end), pair.at);
                QVERIFY2(theme::contrast(QColor(themed(pair.text)), QColor(background)) >= 3,
                         qPrintable(path + u' ' + pair.text + u'/' + background));
            }
        }
    }

    void textOnTheZoneAndGradeColors() {
        theme::current().colors = {{QStringLiteral("#000000"), QStringLiteral("#ffffff")}};
        QCOMPARE(theme::textOn(QStringLiteral("#9fc5d1")), QStringLiteral("#000000"));
        QCOMPARE(theme::textOn(QStringLiteral("#f5c400")), theme::kFixedDarkText);
        QCOMPARE(theme::textOn(QStringLiteral("#202060")), theme::kFixedLightText);
        QCOMPARE(theme::current().css(QStringLiteral("color: %1;").arg(theme::textOn(QStringLiteral("#f5c400")))),
                 QStringLiteral("color: %1;").arg(theme::kFixedDarkText));
        QCOMPARE(zones::blockText(QStringLiteral("#d1d1d1"), QStringLiteral("#e0c200")), zones::kBlockText);
        QCOMPARE(zones::blockText(QStringLiteral("#000426"), QStringLiteral("#001299")), theme::kFixedLightText);
    }

    void themedPensFollowTheTheme() {
        theme::current().colors = {{QStringLiteral("#000000"), QStringLiteral("#ffffff")}};
        const QPen pen = theme::themedPen(QPen(QColor(0, 0, 0, 160), 1));
        QCOMPARE(pen.color().name(), QStringLiteral("#ffffff"));
        QCOMPARE(pen.color().alpha(), 160);
    }

    void widgetSheetsFollowTheTheme() {
        QLabel label;
        theme::setSheet(&label, QStringLiteral("color: #9fc5d1; padding: 4px;"));
        QCOMPARE(label.styleSheet(), QStringLiteral("\ncolor: #9fc5d1; padding: 4px;"));
        theme::current().colors = {{QStringLiteral("#9fc5d1"), QStringLiteral("#ffcc66")}};
        theme::setUiScale(0.5);
        theme::apply();
        QVERIFY2(label.styleSheet().contains(QStringLiteral("color: #ffcc66; padding: 2px;")), qPrintable(label.styleSheet()));
        theme::setUiScale(1.0);
        // an exempt widget is never scaled
        theme::setExempt(&label);
        theme::setUiScale(0.5);
        theme::restyle(&label);
        QVERIFY(label.styleSheet().contains(QStringLiteral("padding: 4px")));
        theme::setUiScale(1.0);
    }

    // ---- The zone colors under the theme ----

    void zoneFtpFollowsTheAppState() {
        QCOMPARE(zones::powerRanges(), ftpcolors::powerRanges(ftpcolors::kUnsetFtpWatts));
        appState().ftpTestMode = QStringLiteral("ramp20");
        QCOMPARE(zones::powerRanges(), ftpcolors::powerRanges(100));
        appState().ftp = 250;
        QCOMPARE(zones::powerRanges(), ftpcolors::powerRanges(250));
        // the app's ranges are the power zones until set
        QCOMPARE(zones::appRanges(), ftpcolors::powerRanges(250));
        appState().ftpRanges = kRanges;
        QCOMPARE(zones::appRanges(), kRanges);
    }

    void setZoneColorAndBack() {
        zones::setZoneColor(2, QStringLiteral("#010101"));
        QVERIFY(!zones::isDefaultZone(2) && zones::isDefaultZone(3));
        QCOMPARE(zones::zoneColors()[2], QStringLiteral("#010101"));
        QCOMPARE(theme::current().ftp, (QJsonObject{{"zones", QJsonObject{{"2", "#010101"}}}}));
        // setting the default again, or none, drops the change
        zones::setZoneColor(2, ftpcolors::kDefaultZones[2]);
        QVERIFY(theme::current().ftp.isEmpty() && zones::isDefault());
    }

    void setFreeColor() {
        zones::setFreeColor(QStringLiteral("#123456"));
        QCOMPARE(zones::freeColor(), QStringLiteral("#123456"));
        QVERIFY(!zones::isDefaultFree());
        zones::setFreeColor(std::nullopt);
        QCOMPARE(zones::freeColor(), ftpcolors::kDefaultFree);
    }

    void themeChangesArePickedUp() {
        QCOMPARE(zones::gaugeRing(0, kRanges), QStringLiteral("d9d9d9"));
        theme::current().ftp = {{"free", "#abcdef"}};
        QCOMPARE(zones::gaugeRing(0, kRanges), QStringLiteral("abcdef"));
    }

    void partColors() {
        QCOMPARE(zones::gaugeArc(25, kRanges), std::pair(zones::paleHex(2, kRanges).mid(1), zones::zoneHex(25, kRanges).mid(1)));
        QCOMPARE(zones::gaugeArc(0, kRanges), std::pair(QStringLiteral("d9d9d9"), QStringLiteral("d9d9d9")));
        QCOMPARE(zones::gaugeRing(15, kRanges), zones::zoneHex(15, kRanges).mid(1));
    }

    void blockColorsAndCss() {
        QCOMPARE(zones::blockColors(25, 0, kRanges), std::pair(zones::zoneHex(25, kRanges), zones::paleHex(2, kRanges)));
        QCOMPARE(zones::blockColors(5, 45, kRanges), std::pair(zones::zoneHex(5, kRanges), zones::zoneHex(45, kRanges)));
        const QString css = zones::blockCss(25, 0, kRanges);
        QVERIFY(css.contains(zones::zoneHex(25, kRanges)) && css.contains(zones::kBlockText));
        QCOMPARE(zones::freeRideCss().count(u'#'), 3);
    }

    void adjustedForTheTheme() {
        QHash<QString, QString> dsa;
        const QJsonObject map = theme::readFile(awotest::pythonAppDir() + QStringLiteral("/themes/colors/41-dsa.json"))
                                    .value(QStringLiteral("colors"))
                                    .toObject();
        for (auto it = map.begin(); it != map.end(); ++it) {
            dsa.insert(it.key().toLower(), it.value().toString().toLower());
        }
        QVERIFY(zones::adjustedZones(ftpcolors::kDefaultZones, dsa).contains(3));
        QVERIFY(zones::adjustedZones(ftpcolors::kDefaultZones, {}).isEmpty());
    }

    void sensorPens() {
        const auto [line, halo] = zones::sensorPens(ftpcolors::kSensorPower);
        QVERIFY(line.isCosmetic() && halo.isCosmetic());
        QCOMPARE(line.color().name(), QStringLiteral("#0000ff"));
        // a dark line gets a light halo
        QCOMPARE(halo.color().name(), QStringLiteral("#f2f2f6"));
        QCOMPARE(zones::markerOutline(ftpcolors::kSensorCadence).color().name(), QStringLiteral("#101018"));
    }

    void notifyWatchers() {
        QSignalSpy changed(&zones::notifier(), &zones::Notifier::changed);
        zones::setFreeColor(QStringLiteral("#111111"));
        QCOMPARE(changed.count(), 1);
        zones::notifyIfChanged();
        QCOMPARE(changed.count(), 1);
        theme::current().ftp = QJsonObject();
        zones::notifyIfChanged();
        QCOMPARE(changed.count(), 2);
        // a new background: the colors are fitted to it again
        theme::current().colors = {{QStringLiteral("#28275e"), QStringLiteral("#ff0000")}};
        zones::notifyIfChanged();
        QCOMPARE(changed.count(), 3);
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_MAIN(TestTheme)
#include "tst_theme.moc"
