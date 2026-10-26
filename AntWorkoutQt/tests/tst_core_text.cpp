// Python's parsing and rounding (src/core/PyCompat), and the workout text helpers (src/core/WorkoutHelpers):
// tests/test_workout_helpers.py and golden/text.json

#include "PyCompat.hpp"
#include "TestSupport.hpp"
#include "WorkoutData.hpp"
#include "WorkoutHelpers.hpp"

using namespace awo;

namespace {

Block steady(int watts, int seconds = 300, int position = 0) {
    Block block;
    block.type = BlockType::Steady;
    block.seconds = seconds;
    block.position = position;
    for (int i = 0; i < kDifficulties; ++i) {
        block.power[i] = watts + i - 10;
    }
    return block;
}

Block ramp(int start, int end, int seconds = 90, int position = 1) {
    Block block;
    block.type = BlockType::Ramp;
    block.seconds = seconds;
    block.position = position;
    for (int i = 0; i < kDifficulties; ++i) {
        block.power[i] = start + i - 10;
        block.powerEnd[i] = end + i - 10;
    }
    return block;
}

const QString kWatts = QStringLiteral("Watts");

}  // namespace

class TestCoreText : public QObject {
    Q_OBJECT

private slots:
    // ---- Python's parsing ----

    void toInt_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<qlonglong>("value");
        QTest::newRow("plain") << "42" << true << 42LL;
        QTest::newRow("spaces") << " 3 " << true << 3LL;
        QTest::newRow("plus") << "+3" << true << 3LL;
        QTest::newRow("minus") << "-3" << true << -3LL;
        QTest::newRow("zeros") << "007" << true << 7LL;
        QTest::newRow("underscore") << "1_000" << true << 1000LL;
        QTest::newRow("double underscore") << "1__0" << false << 0LL;
        QTest::newRow("trailing underscore") << "10_" << false << 0LL;
        QTest::newRow("float") << "3.0" << false << 0LL;
        QTest::newRow("empty") << "" << false << 0LL;
        QTest::newRow("sign only") << "-" << false << 0LL;
        QTest::newRow("letters") << "x" << false << 0LL;
        QTest::newRow("inner space") << "1 0" << false << 0LL;
        QTest::newRow("too big") << "99999999999999999999" << false << 0LL;
    }

    void toInt() {
        QFETCH(QString, text);
        QFETCH(bool, ok);
        QFETCH(qlonglong, value);
        const std::optional<long long> result = py::toInt(text);
        QCOMPARE(result.has_value(), ok);
        if (ok) {
            QCOMPARE(*result, value);
        }
    }

    void toFloat_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<double>("value");
        QTest::newRow("plain") << "1.5" << true << 1.5;
        QTest::newRow("whole") << "2" << true << 2.0;
        QTest::newRow("exponent") << "-3e2" << true << -300.0;
        QTest::newRow("spaces") << " 4 " << true << 4.0;
        QTest::newRow("leading point") << ".5" << true << 0.5;
        QTest::newRow("trailing point") << "5." << true << 5.0;
        QTest::newRow("underscore") << "0.0_5" << true << 0.05;
        QTest::newRow("plus") << "+0.5" << true << 0.5;
        QTest::newRow("infinity") << "Infinity" << true << HUGE_VAL;
        QTest::newRow("two signs") << "+-5" << false << 0.0;
        QTest::newRow("letters") << "abc" << false << 0.0;
        QTest::newRow("empty") << "" << false << 0.0;
        QTest::newRow("bad exponent") << "1e" << false << 0.0;
        QTest::newRow("point") << "." << false << 0.0;
        QTest::newRow("hex") << "0x10" << false << 0.0;
    }

    void toFloat() {
        QFETCH(QString, text);
        QFETCH(bool, ok);
        QFETCH(double, value);
        const std::optional<double> result = py::toFloat(text);
        QCOMPARE(result.has_value(), ok);
        if (ok) {
            QCOMPARE(*result, value);
        }
        QVERIFY(std::isnan(*py::toFloat(QStringLiteral("nan"))));
    }

    void rounding() {
        // halves to even, as Python's round()
        QCOMPARE(py::round(0.5), 0.0);
        QCOMPARE(py::round(1.5), 2.0);
        QCOMPARE(py::round(2.5), 2.0);
        QCOMPARE(py::round(-2.5), -2.0);
        // to digits: the correctly rounded decimal (2.675 is just under, 0.125 an exact tie)
        QCOMPARE(py::round(2.675, 2), 2.67);
        QCOMPARE(py::round(0.125, 2), 0.12);
        QCOMPARE(py::round(0.375, 2), 0.38);
        QCOMPARE(py::fixed(0.125, 2), QStringLiteral("0.12"));
        QCOMPARE(py::fixed(4.0, 2), QStringLiteral("4.00"));
    }

    void formatG() {
        QCOMPARE(py::formatG(0.88), QStringLiteral("0.88"));
        QCOMPARE(py::formatG(3.0), QStringLiteral("3"));
        QCOMPARE(py::formatG(250.0), QStringLiteral("250"));
        QCOMPARE(py::formatG(154.3), QStringLiteral("154.3"));
        QCOMPARE(py::formatG(1234567.0), QStringLiteral("1.23457e+06"));
        QCOMPARE(py::formatG(0.0001), QStringLiteral("0.0001"));
        QCOMPARE(py::formatG(0.00001), QStringLiteral("1e-05"));
    }

    void floatMod() {
        QCOMPARE(py::mod(5.0, 3.0), 2.0);
        QCOMPARE(py::mod(-1.0, 3.0), 2.0);
        QCOMPARE(py::mod(1.0, -3.0), -2.0);
    }

    // ---- Workout text ----

    void minimizedDuration_data() {
        QTest::addColumn<double>("seconds");
        QTest::addColumn<QString>("text");
        const QList<std::pair<double, const char*>> cases = {
            {0, "0s"},       {1, "1s"},         {59, "59s"},    {59.9, "59s"},     {60, "1m"},
            {61, "1m 1s"},   {599, "9m 59s"},   {3599, "59m 59s"}, {3600, "1h"},   {3660, "1h 1m"},
            {3661, "1h 1m 1s"}, {7322, "2h 2m 2s"}, {-5, "0s"},
        };
        for (const auto& [seconds, text] : cases) {
            QTest::newRow(qPrintable(QString::number(seconds))) << seconds << QString::fromUtf8(text);
        }
    }

    void minimizedDuration() {
        QFETCH(double, seconds);
        QFETCH(QString, text);
        QCOMPARE(awo::minimizedDuration(seconds), text);
    }

    void minimizedDurationPastADay() {
        // (the Python app's hours went back to 0 after 24)
        QCOMPARE(awo::minimizedDuration(90000), QStringLiteral("25h"));
    }

    void blockTextFreeSteadyRamp() {
        Block free;
        free.seconds = 60;
        free.position = 3;
        QCOMPARE(blockText(WorkoutEntry::ofBlock(free), 10, kWatts), QStringList{QStringLiteral("Standing 1m")});
        QCOMPARE(blockText(steady(200), 10, kWatts), QStringLiteral("Seated 200 Watts 5m"));
        // the difficulty picks the power
        QCOMPARE(blockText(steady(200), 0, kWatts), QStringLiteral("Seated 190 Watts 5m"));
        QCOMPARE(blockText(steady(200), 20, kWatts), QStringLiteral("Seated 210 Watts 5m"));
        QCOMPARE(blockText(ramp(100, 200), 10, kWatts), QStringLiteral("Seated High Cadence 100-200 Watts 1m 30s"));
    }

    void blockTextIntervals() {
        Block rest;
        rest.seconds = 30;
        QCOMPARE(blockText(WorkoutEntry::interval(4, {steady(300, 30, 3), rest}), 10, kWatts),
                 (QStringList{QStringLiteral("4x"), QStringLiteral("Standing 300 Watts 30s"), QStringLiteral("Seated 30s")}));
        Block low;
        low.seconds = 15;
        low.position = 2;
        QCOMPARE(blockText(WorkoutEntry::interval(2, {steady(300, 30, 4), ramp(100, 150, 60, 5), low}), 10, kWatts),
                 (QStringList{QStringLiteral("2x"), QStringLiteral("Single Leg 300 Watts 30s"),
                              QStringLiteral("Switch Single Leg 100-150 Watts 1m"), QStringLiteral("Seated Low Cadence 15s")}));
    }

    void blockTextInRpm() {
        QCOMPARE(blockText(steady(90), 10, QStringLiteral("RPM")), QStringLiteral("Seated 90 RPM 5m"));
    }

    void workoutTextListsIntervalsIndented() {
        Block rest;
        rest.seconds = 30;
        const QList<WorkoutEntry> entries = {WorkoutEntry::ofBlock(steady(200)),
                                             WorkoutEntry::interval(2, {steady(300, 30), rest}),
                                             WorkoutEntry::intervalBlock(2, 0, steady(300, 30))};
        QCOMPARE(workoutText(entries, kWatts, {{1, QStringLiteral("3m : Band - Song")}}),
                 QStringLiteral("Seated 200 Watts 5m\n3m : Band - Song\n2x Seated 300 Watts 30s\n      Seated 30s\n"));
    }

    void targetsFromText_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("low");
        QTest::addColumn<int>("high");
        QTest::addColumn<bool>("found");
        QTest::newRow("single") << "Seated 200 Watts 5m" << 200 << 0 << true;
        QTest::newRow("range") << "Seated 100-200 Watts 1m" << 100 << 200 << true;
        QTest::newRow("rpm") << "Seated 90 RPM 5m" << 90 << 0 << true;
        QTest::newRow("free") << "Standing 1m" << 0 << 0 << false;
        QTest::newRow("first word") << "Watts 5m" << 0 << 0 << false;
        QTest::newRow("not a number") << "Seated abc Watts" << 0 << 0 << false;
        QTest::newRow("header") << "4x" << 0 << 0 << false;
    }

    void targetsFromText() {
        QFETCH(QString, text);
        QFETCH(int, low);
        QFETCH(int, high);
        QFETCH(bool, found);
        const auto targets = awo::targetsFromText(text);
        QCOMPARE(targets.has_value(), found);
        if (found) {
            QCOMPARE(targets->first, low);
            QCOMPARE(targets->second, high);
        }
    }

    void songHeaderOnlyInCadenceMode() {
        QVERIFY(!isSongHeader(QStringLiteral("3m 20s : Artist - Title"), false));
        QVERIFY(isSongHeader(QStringLiteral("3m 20s : Artist - Title"), true));
        QVERIFY(!isSongHeader(QStringLiteral("Seated 90 RPM 5m"), true));
    }

    void maxHelpers() {
        QCOMPARE(maxOrZero(QList<int>{}), 0);
        QCOMPARE(maxOrZero(QList<int>{3, 9, 2}), 9);
        QCOMPARE(maxOrZeroAndIndex(QList<int>{}), std::pair(0, qsizetype(0)));
        QCOMPARE(maxOrZeroAndIndex(QList<int>{3, 9, 2, 9}), std::pair(9, qsizetype(1)));
    }

    void dropoutsShortGapsAreJoined() {
        const QList<double> times{0, 1, 2, 7};
        const QList<double> values{100, 110, 120, 130};
        QCOMPARE(withDropoutsAsZero(times, values), std::pair(times, values));
    }

    void dropoutsLongGapDropsToZero() {
        const auto [times, values] = withDropoutsAsZero({0, 1, 10}, {100, 110, 130});
        QCOMPARE(times, (QList<double>{0, 1, 1 + kDropoutEdgeSeconds, 10 - kDropoutEdgeSeconds, 10}));
        QCOMPARE(values, (QList<double>{100, 110, 0, 0, 130}));
    }

    void dropoutsSilentAtTheEnd() {
        const auto [times, values] = withDropoutsAsZero({0, 1}, {100, 110}, 20.0);
        QCOMPARE(times, (QList<double>{0, 1, 1 + kDropoutEdgeSeconds, 20}));
        QCOMPARE(values, (QList<double>{100, 110, 0, 0}));
        QCOMPARE(withDropoutsAsZero({0, 1}, {100, 110}, 5.0), std::pair(QList<double>{0, 1}, QList<double>{100, 110}));
        QVERIFY(withDropoutsAsZero({}, {}, 5.0).first.isEmpty());
    }

    void selectedDeviceLinesAreCleaned() {
        selectedDevices::write({QStringLiteral("11 12345"), QStringLiteral("120 0023"), QStringLiteral("11 12345"),
                                QStringLiteral("17 1"), QStringLiteral("121 65535"), QStringLiteral("122 65536"),
                                QStringLiteral("123 0"), QStringLiteral("99 5"), QStringLiteral("11"),
                                QStringLiteral("11 2 3"), QStringLiteral("11 abc"), QString(), QStringLiteral("11\t7")});
        QCOMPARE(selectedDevices::read(), (QStringList{QStringLiteral("11 12345"), QStringLiteral("120 23"),
                                                      QStringLiteral("17 1"), QStringLiteral("121 65535"),
                                                      QStringLiteral("11 7")}));
        selectedDevices::clear();
        QVERIFY(selectedDevices::read().isEmpty());
    }

    void sensorHistoryRecordsRows() {
        SensorHistory history(2);
        history.append({10, 20.7});
        history.append({11, 21});
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.column(0), (QList<int>{10, 11}));
        QCOMPARE(history.column(1), (QList<int>{20, 21}));
        QVERIFY(history.times()[0] <= history.times()[1]);
        history.clear();
        QVERIFY(history.isEmpty());
    }

    void workoutDataClearEmptiesEveryHistory() {
        WorkoutData data;
        data.power.append({1});
        data.speedCadence.append({1, 2, 3, 4});
        data.cadence.append({1, 2});
        data.clear();
        QVERIFY(data.power.isEmpty() && data.speedCadence.isEmpty() && data.cadence.isEmpty());
    }

    // ---- The same as the Python app ----

    void golden() {
        const QJsonObject text = awotest::golden(QStringLiteral("text.json")).toObject();
        for (const QJsonValue value : text[QStringLiteral("durations")].toArray()) {
            const QJsonArray pair = value.toArray();
            const double seconds = pair[0].toDouble();
            if (seconds >= 86400) {
                continue;  // (see minimizedDurationPastADay)
            }
            QCOMPARE(awo::minimizedDuration(seconds), pair[1].toString());
        }
        for (const QJsonValue value : text[QStringLiteral("targets")].toArray()) {
            const QJsonArray pair = value.toArray();
            const auto targets = awo::targetsFromText(pair[0].toString());
            if (pair[1].isNull()) {
                QVERIFY2(!targets, qPrintable(pair[0].toString()));
            } else {
                QVERIFY2(targets, qPrintable(pair[0].toString()));
                QCOMPARE(targets->first, pair[1].toArray()[0].toInt());
                QCOMPARE(targets->second, pair[1].toArray()[1].toInt());
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestCoreText)
#include "tst_core_text.moc"
