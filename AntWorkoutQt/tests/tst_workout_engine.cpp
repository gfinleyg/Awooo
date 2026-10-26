// The workout engine (src/core/WorkoutEngine) against the Python workout screen's own rides (golden/hud.json): the
// same workouts and FTP tests ridden by the same simulated rider (PreviewRide's Rider, the same seed) a second a tick,
// with the same difficulty changes and pauses, ask for the same power at the same times and grade every block the
// same. Also Python's random itself, which the rider draws from.

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "AppState.hpp"
#include "AwoParser.hpp"
#include "GoldenRides.hpp"
#include "PyRandom.hpp"
#include "SimulatedRider.hpp"
#include "TestSupport.hpp"
#include "WorkoutEngine.hpp"

using namespace awo;

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

    void ridesMatchThePythonWorkoutScreen_data() { awotest::addRideRows(); }

    void ridesMatchThePythonWorkoutScreen() {
        QFETCH(QJsonObject, golden);
        const QJsonObject ride = golden[QStringLiteral("ride")].toObject();
        QVERIFY(awotest::setUpRide(ride));
        const AppState& state = appState();
        WorkoutEngine engine([] { return 1000.0; });
        SimulatedRider rider(ride[QStringLiteral("seed")].toInt());
        const auto events = awotest::rideEvents(ride);
        engine.start();
        int ticks = 0;
        // (Musical Cadence Mode: the music silent, a second a tick, the workout's clock)
        double music = 0;
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
            music += 1;
            engine.tick(state.cadenceMode ? std::optional<double>(music) : std::nullopt);
            const double t = state.secondsPassed;
            saveSimulatedReading(t, rider.sample(t, engine.currentPower(), state.ftp, state.cadenceMode));
            ++ticks;
        }
        awotest::compareRide(golden, ticks, engine, false);
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
        QVERIFY(awotest::buildRideWorkout({{"ftpTest", "ramp20"}}));
        double now = 0;
        WorkoutEngine engine([&now] { return now; });
        engine.start();
        // into the ramp: past the warmup
        while (!engine.inFtpTestEffort()) {
            engine.advanceClock(1.0);
            engine.tick();
            saveSimulatedReading(state.secondsPassed, {200, 140, 90, 20});
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
