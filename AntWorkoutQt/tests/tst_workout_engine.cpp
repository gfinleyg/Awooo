// The workout engine (src/core/WorkoutEngine) against the Python workout screen's own rides (golden/hud.json): the
// same workouts and FTP tests ridden by the same simulated rider (PreviewRide's Rider, the same seed) a second a tick,
// with the same difficulty changes and pauses, ask for the same power at the same times and grade every block the
// same. Also Python's random itself, which the rider draws from.

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "AppState.hpp"
#include "AwoParser.hpp"
#include "FtpTest.hpp"
#include "PyRandom.hpp"
#include "SimulatedRider.hpp"
#include "TestSupport.hpp"
#include "WorkoutEngine.hpp"

using namespace awo;

namespace {

// a reading into the graph's recent values, as PreviewRide.save_reading keeps them
void saveReading(double t, const RiderReading& reading) {
    AppState& state = appState();
    const double window = state.graphSecondsShown - state.graphBreakpoint;
    for (const auto& [saved, value] : {std::pair{&state.savedPowerVals, static_cast<double>(reading.power)},
                                       std::pair{&state.savedHRVals, static_cast<double>(reading.heartRate)},
                                       std::pair{&state.savedCadenceVals, static_cast<double>(static_cast<int>(reading.cadence))}}) {
        saved->times.append(t);
        saved->values.append(value);
        while (saved->times.back() - saved->times.front() > window) {
            saved->times.removeFirst();
            saved->values.removeFirst();
        }
    }
}

// the workout (or FTP test) the ride is of, into AppState as the setup screen builds it
bool buildWorkout(const QJsonObject& ride) {
    AppState& state = appState();
    AwoOptions options;
    options.ftp = state.ftp;
    const QString test = ride.value(QStringLiteral("ftpTest")).toString();
    AwoResult result;
    if (!test.isEmpty()) {
        const ftptest::Test* ftpTest = ftptest::find(test);
        options.maxBlockSeconds = std::max(kMaxBlockSeconds, ftpTest->effortSeconds);
        result = parseAwoLines(ftptest::workoutLines(test, state.ftp != 0), options);
    } else {
        QStringList lines = ride.value(QStringLiteral("workout")).toString().split(u'\n');
        lines.removeLast();
        result = parseAwoLines(lines, options);
    }
    if (!result.ok()) {
        return false;
    }
    state.workoutList = result.workout.entries;
    state.workoutMessages = result.workout.messages;
    state.workoutTitle = result.workout.title;
    state.workoutDurationSeconds = result.workout.seconds;
    if (!test.isEmpty()) {
        int effortStart = 0;
        if (ftptest::isRamp(test)) {
            effortStart = state.workoutDurationSeconds;
            const WorkoutEntry first = ftptest::rampBlock(test, 0);
            state.workoutList.append(first);
            state.workoutDurationSeconds += first.block.seconds;
        } else {
            effortStart = state.workoutDurationSeconds - ftptest::find(test)->effortSeconds;
        }
        state.workoutMessages += ftptest::messages(test, effortStart, state.ftp);
    }
    return true;
}

QString diff(const QString& what, double ours, double python) {
    return QStringLiteral("%1: %2, Python %3").arg(what).arg(ours, 0, 'g', 17).arg(python, 0, 'g', 17);
}

}  // namespace

class TestWorkoutEngine : public QObject {
    Q_OBJECT

private slots:
    void init() { appState() = AppState(); }

    // CPython 3.10's random.Random(3): random() and gauss() (worked out with it)
    void pythonRandom() {
        py::Random rng(3);
        QCOMPARE(rng.random(), 0.23796462709189137);
        QCOMPARE(rng.random(), 0.5442292252959519);
        // gauss: a pair a draw, the second kept for the next call
        py::Random gauss(7);
        QCOMPARE(gauss.gauss(0, 1), -0.2558802884476004);
        QCOMPARE(gauss.gauss(0, 1), 0.511431512516514);
        QCOMPARE(gauss.gauss(0, 8), -1.8087693182648377);
        // (a seed wider than 32 bits is two words)
        QCOMPARE(py::Random((1ULL << 40) + 5).random(), 0.5043802970418443);
        QCOMPARE(py::Random(0).random(), 0.8444218515250481);
        QCOMPARE(py::Random(42).random(), 0.6394267984578837);
    }

    void ridesMatchThePythonWorkoutScreen_data() {
        QTest::addColumn<QJsonObject>("golden");
        for (const QJsonValue& value : awotest::golden(QStringLiteral("hud.json")).toArray()) {
            const QJsonObject golden = value.toObject();
            QTest::newRow(qPrintable(golden[QStringLiteral("ride")].toObject()[QStringLiteral("name")].toString()))
                << golden;
        }
    }

    void ridesMatchThePythonWorkoutScreen() {
        QFETCH(QJsonObject, golden);
        const QJsonObject ride = golden[QStringLiteral("ride")].toObject();
        AppState& state = appState();
        state.ftp = ride[QStringLiteral("ftp")].toInt();
        state.maxHr = 185;
        state.wheelCircumference = 2.105;
        state.workoutMode = true;
        state.cadenceMode = false;
        state.ftpTestMode = ride[QStringLiteral("ftpTest")].toString();
        state.antHRM = state.antSpeedCadence = true;
        QVERIFY(buildWorkout(ride));
        state.workoutFilename = state.workoutFilenameShort = QStringLiteral("golden.awo");

        WorkoutEngine engine([] { return 1000.0; });
        SimulatedRider rider(ride[QStringLiteral("seed")].toInt());
        QMap<int, QList<QJsonArray>> events;
        for (const QJsonValue& event : ride[QStringLiteral("events")].toArray()) {
            events[event.toArray()[0].toInt()].append(event.toArray());
        }
        engine.start();
        int ticks = 0;
        for (int tick = 0; tick < ride[QStringLiteral("seconds")].toInt(); ++tick) {
            if (!engine.started()) {
                break;
            }
            for (const QJsonArray& event : events.value(tick)) {
                if (event[1].toString() == u"difficulty") {
                    engine.setDifficulty(event[2].toInt());
                } else {
                    engine.pauseRecording();
                }
            }
            engine.advanceClock(1.0);
            engine.tick();
            const double t = state.secondsPassed;
            saveReading(t, rider.sample(t, engine.currentPower(), state.ftp));
            ++ticks;
        }

        QCOMPARE(ticks, golden[QStringLiteral("ticks")].toInt());
        const QList<double> seconds = [&] {
            QList<double> list;
            for (const QJsonValue& v : golden[QStringLiteral("requestSeconds")].toArray()) {
                list.append(v.toDouble());
            }
            return list;
        }();
        const QJsonArray watts = golden[QStringLiteral("requestWatts")].toArray();
        QCOMPARE(state.workoutPowerRequestsWatts.size(), watts.size());
        for (qsizetype i = 0; i < watts.size(); ++i) {
            QVERIFY2(state.workoutPowerRequestsWatts[i] == watts[i].toDouble(),
                     qPrintable(diff(QStringLiteral("request %1 watts").arg(i), state.workoutPowerRequestsWatts[i],
                                     watts[i].toDouble())));
            QVERIFY2(state.workoutPowerRequestsSeconds[i] == seconds[i],
                     qPrintable(diff(QStringLiteral("request %1 time").arg(i), state.workoutPowerRequestsSeconds[i],
                                     seconds[i])));
        }

        const QJsonObject grades = golden[QStringLiteral("grades")].toObject();
        const QJsonArray order = golden[QStringLiteral("gradeOrder")].toArray();
        QCOMPARE(state.blockGrades.items().size(), order.size());
        for (qsizetype i = 0; i < order.size(); ++i) {
            const auto& [key, grade] = state.blockGrades.items()[i];
            const QString name = QStringLiteral("%1,%2,%3").arg(key.index).arg(key.sub).arg(key.rep);
            QCOMPARE(name, order[i].toString());
            const QJsonObject python = grades[name].toObject();
            QCOMPARE(grade.label, python[QStringLiteral("label")].toString());
            QCOMPARE(grade.free, python[QStringLiteral("free")].toBool());
            QCOMPARE(grade.endedEarly, python[QStringLiteral("endedEarly")].toBool());
            QCOMPARE(grade.top, python[QStringLiteral("top")].toInt());
            const QList<std::pair<QString, std::optional<double>>> numbers = {
                {"pct", grade.pct},         {"graded", grade.graded},     {"dropped", grade.dropped},
                {"grace", grade.grace},     {"duration", grade.duration}, {"ridden", grade.ridden},
                {"pedaling", grade.pedaling}, {"avg", grade.avg},         {"low", grade.low},
                {"high", grade.high},       {"overAvg", grade.overAvg},   {"surge", grade.surge},
                {"penalty", grade.penalty}, {"bonus", grade.bonus},       {"intensity", grade.intensity},
                {"start", grade.start},     {"end", grade.end}};
            for (const auto& [field, value] : numbers) {
                QVERIFY2(awotest::sameNumber(python[field], value),
                         qPrintable(QStringLiteral("%1 %2: %3, Python %4")
                                        .arg(name, field, value ? QString::number(*value, 'g', 17) : QStringLiteral("none"),
                                             QString::number(python[field].toDouble(), 'g', 17))));
            }
        }

        const QJsonValue result = golden[QStringLiteral("ftpTestResult")];
        if (result.isNull()) {
            QVERIFY(!state.ftpTestResult);
        } else {
            QVERIFY(state.ftpTestResult.has_value());
            QVERIFY(awotest::sameNumber(result[QStringLiteral("average")], state.ftpTestResult->average));
            QCOMPARE(state.ftpTestResult->completed, result[QStringLiteral("completed")].toBool());
        }
        QCOMPARE(state.workoutDurationSeconds, golden[QStringLiteral("workoutSeconds")].toInt());
        QCOMPARE(state.secondsPassed, golden[QStringLiteral("secondsPassed")].toDouble());
        QCOMPARE(engine.workoutSecondsPassed(), golden[QStringLiteral("workoutSecondsPassed")].toDouble());
        QCOMPARE(engine.activeWorkout(), golden[QStringLiteral("activeWorkout")].toInt());
        QCOMPARE(engine.completeShown(), golden[QStringLiteral("completed")].toBool());
    }

    // ---- Smaller things ----

    void messagesShowWhileRunning() {
        AppState& state = appState();
        state.ftp = 200;
        AwoOptions options;
        options.ftp = 200;
        const AwoResult result = parseAwoLines({"1:60,0.5|First|20|0|Second|10|5", "1:60,0.6"}, options);
        QVERIFY(result.ok());
        state.workoutList = result.workout.entries;
        state.workoutMessages = result.workout.messages;
        state.workoutDurationSeconds = result.workout.seconds;
        WorkoutEngine engine;
        QCOMPARE(engine.message(), QString());
        engine.start();
        for (int i = 0; i < 7; ++i) {
            engine.advanceClock(1.0);
            engine.tick();
        }
        // (overlapping: the one that started last)
        QCOMPARE(engine.message(), QStringLiteral("Second"));
        QCOMPARE(engine.upcoming(), (QList<WorkoutEngine::UpcomingLine>{{"Seated 120 Watts 1m"}}));
        QCOMPARE(WorkoutEngine::difficultyFtpText(3), QStringLiteral("FTP: 212"));
        // the graph's bars: what's left of this block, then the next
        const auto [times, powers] = engine.upcomingGraph(480);
        QCOMPARE(powers, (QList<double>{100, 120}));
        QCOMPARE(times.size(), 3);
        QCOMPARE(times[1] - times[0], 54.0);
    }

    void restartGoesBackToTheStart() {
        AppState& state = appState();
        state.ftp = 200;
        AwoOptions options;
        options.ftp = 200;
        const AwoResult result = parseAwoLines({"1:30,0.5", "1:30,0.6"}, options);
        state.workoutList = result.workout.entries;
        state.workoutDurationSeconds = result.workout.seconds;
        WorkoutEngine engine;
        engine.start();
        for (int i = 0; i < 40; ++i) {
            engine.advanceClock(1.0);
            engine.tick();
        }
        QCOMPARE(engine.currentPower(), 120);
        QVERIFY(!state.blockGrades.isEmpty());
        engine.restart();
        QVERIFY(!engine.started() && engine.activeWorkout() == -1);
        QVERIFY(state.blockGrades.isEmpty());
        QCOMPARE(engine.workoutSecondsPassed(), 0.0);
        // the power asked for so far is kept
        QVERIFY(!state.workoutPowerRequestsWatts.isEmpty());
    }

    void aRampTestPausedTooLongStartsOver() {
        AppState& state = appState();
        state.ftpTestMode = QStringLiteral("ramp20");
        QVERIFY(buildWorkout({{"ftpTest", "ramp20"}}));
        double now = 0;
        WorkoutEngine engine([&now] { return now; });
        engine.start();
        // into the ramp: past the warmup
        while (!engine.inFtpTestEffort()) {
            engine.advanceClock(1.0);
            engine.tick();
            saveReading(state.secondsPassed, {200, 140, 90, 20});
        }
        engine.pauseRecording();
        QCOMPARE(*engine.rampPauseText(), QStringLiteral("PAUSE\n60s"));
        now = 61;
        QCOMPARE(*engine.rampPauseText(), QStringLiteral("PAUSE\nRESET"));
        QVERIFY(engine.pauseRecording());
        QVERIFY(engine.newWorkoutStart());
    }
};

QTEST_MAIN(TestWorkoutEngine)
#include "tst_workout_engine.moc"
