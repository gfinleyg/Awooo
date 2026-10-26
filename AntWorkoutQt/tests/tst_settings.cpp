// Everything kept in savedSettingsANT.json (src/core: SavedSettings, Units, Performance, PowerCalibration):
// tests/test_settings.py and golden/units.json

#include <QSignalSpy>

#include "Performance.hpp"
#include "PowerCalibration.hpp"
#include "SavedSettings.hpp"
#include "TestSupport.hpp"
#include "Units.hpp"

using namespace awo;

namespace {

QJsonObject readSettings() {
    QFile file(settings::kSettingsFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

}  // namespace

class TestSettings : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        Units::current().reset();
        Performance::current().reset();
    }
    void cleanup() { dir_.reset(); }

    // ---- SavedSettings ----

    void loadWithNothingSaved() { QVERIFY(settings::load().isEmpty()); }

    void updateMergesAndKeepsOtherKeys() {
        QVERIFY(settings::update({{"ftp", 250}, {"maxHr", 185}}));
        QVERIFY(settings::update({{"ftp", 260}}));
        QCOMPARE(settings::load(), (QJsonObject{{"ftp", 260}, {"maxHr", 185}}));
        QCOMPARE(QDir().entryList(QDir::Files), QStringList{settings::kSettingsFile});
    }

    void unreadableOrWrongShapeFileLoadsEmpty() {
        for (const QByteArray contents : {QByteArray("not json"), QByteArray("[1, 2]"), QByteArray()}) {
            awotest::writeFile(settings::kSettingsFile, contents);
            QVERIFY(settings::load().isEmpty());
        }
    }

    void updateReportsAFailedWrite() {
        // (a folder where the file would go)
        QVERIFY(QDir().mkdir(settings::kSettingsFile));
        QVERIFY(!settings::update({{"ftp", 1}}));
    }

    void legacyFilesAreMigratedAndRemoved() {
        awotest::writeFile(settings::kLegacyStatsFile, "2.096,250,185\n");
        awotest::writeFile(settings::kLegacyCalibrationFile, R"({"11 5": {"offset_watts": 3.0, "scale": 1.0}})");
        QCOMPARE(settings::load(),
                 (QJsonObject{{"wheelCircumference", 2.096}, {"ftp", 250}, {"maxHr", 185},
                              {"powerCalibration", QJsonObject{{"11 5", QJsonObject{{"offset_watts", 3.0}, {"scale", 1.0}}}}}}));
        QVERIFY(!QFile::exists(settings::kLegacyStatsFile));
        QVERIFY(!QFile::exists(settings::kLegacyCalibrationFile));
    }

    void badLegacyStatsAreSkipped() {
        awotest::writeFile(settings::kLegacyStatsFile, "abc,250,185");
        // nothing usable: no settings file written, the legacy file left alone
        QVERIFY(settings::load().isEmpty());
        QVERIFY(QFile::exists(settings::kLegacyStatsFile));
        QVERIFY(!QFile::exists(settings::kSettingsFile));
    }

    void legacyFilesIgnoredOnceSettingsExist() {
        settings::update({{"ftp", 300}});
        awotest::writeFile(settings::kLegacyStatsFile, "2.096,250,185");
        QCOMPARE(settings::load(), (QJsonObject{{"ftp", 300}}));
    }

    // ---- Units ----

    void unitsDefaultsAndSavedValuesLoad() {
        Units& units = Units::current();
        units.load();
        QCOMPARE(units.weight, QStringLiteral("lbs"));
        QCOMPARE(units.powerWeight, QStringLiteral("W/kg"));
        QCOMPARE(units.distance, QStringLiteral("mi"));
        settings::update({{"weightUnit", "kg"}, {"powerWeightUnit", "W/lb"}, {"distanceUnit", "km"}, {"riderWeightKg", 70}});
        units.load();
        QCOMPARE(units.weight, QStringLiteral("kg"));
        QCOMPARE(units.powerWeight, QStringLiteral("W/lb"));
        QCOMPARE(units.distance, QStringLiteral("km"));
        QCOMPARE(units.riderWeightKg, 70.0);
    }

    void unitsIgnoreUnknownSavedValues() {
        for (const QJsonObject& saved :
             {QJsonObject{{"weightUnit", "stone"}, {"distanceUnit", "furlongs"}, {"powerWeightUnit", "hp"}},
              QJsonObject{{"riderWeightKg", 5}}, QJsonObject{{"riderWeightKg", 500}}, QJsonObject{{"riderWeightKg", "70"}}}) {
            QFile::remove(settings::kSettingsFile);
            Units::current().reset();
            settings::update(saved);
            Units::current().load();
            QCOMPARE(Units::current().weight, QStringLiteral("lbs"));
            QCOMPARE(Units::current().distance, QStringLiteral("mi"));
            QCOMPARE(Units::current().riderWeightKg, 0.0);
        }
    }

    void setUnitsSavesIgnoresUnknownAndSignals() {
        QSignalSpy changed(&Units::current(), &Units::changed);
        Units::current().setUnits(QStringLiteral("kg"), QStringLiteral("bogus"), QStringLiteral("km"));
        QCOMPARE(readSettings(), (QJsonObject{{"weightUnit", "kg"}, {"powerWeightUnit", "W/kg"}, {"distanceUnit", "km"}}));
        QCOMPARE(changed.count(), 1);
    }

    void weightConversions() {
        Units& units = Units::current();
        QCOMPARE(units.weightText(70), QStringLiteral("154.3 lbs"));
        QCOMPARE(units.formatWeight(units.weightToKg(165)), QStringLiteral("165"));
        const auto [low, high] = units.weightRange();
        QVERIFY(std::abs(low - 20 * Units::kLbsPerKg) < 1e-9 && std::abs(high - 300 * Units::kLbsPerKg) < 1e-9);
        units.setUnits(QStringLiteral("kg"));
        QCOMPARE(units.weightText(70), QStringLiteral("70 kg"));
        QCOMPARE(units.weightText(70.26), QStringLiteral("70.3 kg"));
        QCOMPARE(units.weightToKg(70), 70.0);
    }

    void powerToWeight() {
        Units& units = Units::current();
        QVERIFY(!units.powerToWeight(300));
        QVERIFY(units.powerToWeightText(300).isEmpty());
        units.setRiderWeightKg(75);
        QCOMPARE(readSettings()[QStringLiteral("riderWeightKg")].toDouble(), 75.0);
        QCOMPARE(*units.powerToWeight(300), 4.0);
        QCOMPARE(units.powerToWeightText(300), QStringLiteral(" (4.00 W/kg)"));
        QVERIFY(!units.powerToWeight(std::nullopt));
        units.setUnits({}, QStringLiteral("W/lb"));
        QVERIFY(std::abs(*units.powerToWeight(300) - 300 / (75 * Units::kLbsPerKg)) < 1e-12);
        QVERIFY(units.powerToWeightText(300, {}, {}).endsWith(QStringLiteral("W/lb")));
    }

    void speedAndDistance() {
        Units& units = Units::current();
        QCOMPARE(units.speed(10), 10.0);
        QCOMPARE(units.distanceFromMiles(10), 10.0);
        QCOMPARE(units.speedUnit(), QStringLiteral("MPH"));
        QCOMPARE(units.distanceUnit(), QStringLiteral("Miles"));
        QCOMPARE(units.distanceUnit(false), QStringLiteral("mi"));
        units.setUnits({}, {}, QStringLiteral("km"));
        QVERIFY(std::abs(units.speed(10) - 16.09344) < 1e-12);
        QVERIFY(std::abs(units.distanceFromMiles(1) - 1.609344) < 1e-12);
        QCOMPARE(units.speedUnit(), QStringLiteral("KM/H"));
        QCOMPARE(units.distanceUnit(), QStringLiteral("Km"));
        QCOMPARE(units.distanceUnit(false), QStringLiteral("km"));
    }

    // ---- Performance ----

    void gaugeFpsDefaultSavedAndInvalid() {
        Performance& performance = Performance::current();
        QCOMPARE(performance.gaugeFps(), Performance::kDefaultGaugeFps);
        performance.reset();
        settings::update({{"gaugeFps", 17}});
        QCOMPARE(performance.gaugeFps(), Performance::kDefaultGaugeFps);
        performance.reset();
        settings::update({{"gaugeFps", 20}});
        QCOMPARE(performance.gaugeFps(), 20);
    }

    void setGaugeFps() {
        Performance& performance = Performance::current();
        QSignalSpy changed(&performance, &Performance::changed);
        performance.setGaugeFps(25);  // not a choice
        performance.setGaugeFps(Performance::kDefaultGaugeFps);  // no change
        QCOMPARE(changed.count(), 0);
        performance.setGaugeFps(0);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(performance.gaugeFps(), 0);
        QCOMPARE(readSettings()[QStringLiteral("gaugeFps")].toInt(), 0);
    }

    void gaugeFpsText() {
        QVERIFY(Performance::gaugeFpsText(0).startsWith(QStringLiteral("Off")));
        QCOMPARE(Performance::gaugeFpsText(60), QStringLiteral("60 fps - smoothest"));
        QCOMPARE(Performance::gaugeFpsText(30), QStringLiteral("30 fps"));
    }

    // ---- PowerCalibration ----

    void calibrationSaveGetClear() {
        QVERIFY(!calibration::get(11, 5));
        QVERIFY(calibration::save(11, 5, 4.26, 1.023456, -12));
        calibration::save(11, 6, 1);
        const calibration::Calibration saved = *calibration::get(11, 5);
        QCOMPARE(saved.offsetWatts, 4.3);
        QCOMPARE(saved.scale, 1.0235);
        QCOMPARE(*saved.antZeroOffset, -12);
        QVERIFY(saved.updated.contains(u'T'));
        QVERIFY(calibration::clear(11, 5));
        QVERIFY(!calibration::get(11, 5));
        QVERIFY(calibration::get(11, 6));
        // clearing what isn't there is fine
        QVERIFY(calibration::clear(11, 5));
    }

    void calibrationIgnoresABadSavedValue() {
        settings::update({{"powerCalibration", QJsonArray{"not", "a", "dict"}}});
        QVERIFY(calibration::loadAll().isEmpty());
    }

    void calibrationApply() {
        using calibration::Calibration;
        QCOMPARE(calibration::apply(200, std::nullopt), 200);
        QCOMPARE(calibration::apply(200, Calibration{}), 200);
        QCOMPARE(calibration::apply(0, Calibration{5, 1.1}), 0);  // coasting stays 0
        QCOMPARE(calibration::apply(200, Calibration{5, 1.0}), 195);
        QCOMPARE(calibration::apply(200, Calibration{0, 1.1}), 220);
        QCOMPARE(calibration::apply(200, Calibration{5, 1.1}), 215);  // (200 - 5) x 1.1
        QCOMPARE(calibration::apply(3, Calibration{5, 1.0}), 0);      // never negative
    }

    void calibrationDescribe() {
        QCOMPARE(calibration::describe(std::nullopt), QStringLiteral("No calibration saved"));
        calibration::Calibration saved{4.5, 1.0, std::nullopt, QStringLiteral("2026-10-12T10:00:00")};
        QCOMPARE(calibration::describe(saved), QStringLiteral("Offset 4.5 W, scale x1.000 (saved 2026-10-12 10:00:00)"));
    }

    // ---- The same as the Python app ----

    void golden() {
        const QJsonObject all = awotest::golden(QStringLiteral("units.json")).toObject();
        Units& units = Units::current();
        for (const QJsonValue value : all[QStringLiteral("weights")].toArray()) {
            units.weight = value[0].toString();
            QCOMPARE(units.formatWeight(value[1].toDouble()), value[2].toString());
        }
        units.weight = QStringLiteral("lbs");
        for (const QJsonValue value : all[QStringLiteral("powerToWeight")].toArray()) {
            units.powerWeight = value[0].toString();
            units.riderWeightKg = 72.5;
            QCOMPARE(units.powerToWeightText(value[1].toDouble()), value[2].toString());
        }
        for (const QJsonValue value : all[QStringLiteral("calibration")].toArray()) {
            const calibration::Calibration saved{value[1][QStringLiteral("offset_watts")].toDouble(),
                                                 value[1][QStringLiteral("scale")].toDouble()};
            QCOMPARE(calibration::apply(value[0].toInt(), saved), value[2].toInt());
            QCOMPARE(calibration::describe(saved), value[3].toString());
        }
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_GUILESS_MAIN(TestSettings)
#include "tst_settings.moc"
