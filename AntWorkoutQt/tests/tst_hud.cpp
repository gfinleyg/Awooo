// The workout screen (src/ui/screens/HudScreen) built offscreen on a stand-in bridge: the Python workout screen's own
// rides (golden/hud.json) ridden through it ask for the same power and grade the same; what it gives the spiral;
// START, PAUSE and the END prompt's choices; whole workouts ridden to the end and graded; the spiral following the
// ride; and an FE-C trainer following the workout.

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QtTest>

#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "AppState.hpp"
#include "AwoParser.hpp"
#include "ConfirmEndWorkoutWindow.hpp"
#include "FakeAnt.hpp"
#include "GoldenRides.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"
#include "Performance.hpp"
#include "PowerSpiral.hpp"
#include "Session.hpp"
#include "SimulatedRider.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutData.hpp"
#include "WorkoutGrading.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

using namespace awo;

namespace {

// a bridge with everything connected and nothing on a thread: the trainer's targets are just recorded
class StubBridge : public ant::AntBridge {
public:
    explicit StubBridge(awotest::FakeAnt& fake)
        : AntBridge(
              [transport = fake.stick](antnative::Stick::MessageHandler sink) {
                  transport->sink = std::move(sink);
                  return transport;
              },
              fake.clock, [](double) {}) {}

    bool trainer = false;
    QList<std::optional<double>> targets;
    int releases = 0;

    QStringList foundDevices() const override { return {}; }
    bool isInitialized() const override { return true; }
    double scanSettleRemaining() const override { return 0; }
    std::optional<QString> takeError() override { return std::nullopt; }
    std::optional<QString> waitingForStick() const override { return std::nullopt; }
    QSet<ant::Role> selectedRoles() const override { return {}; }
    bool isConnectRequested() const override { return true; }
    bool isReady() const override { return true; }
    QStringList lowBatteryWarnings() const override { return {}; }
    bool trainerSelected() const override { return trainer; }
    std::optional<ant::DeviceReading> selectedDevice(ant::Role) const override { return std::nullopt; }
    void requestConnect(const ant::Selection&) override {}
    void requestDisconnect() override {}
    void setTrainerTargetPower(std::optional<double> watts) override { targets.append(watts); }
    void releaseTrainerPower() override { ++releases; }
};

// the rider set up as the golden rides have them, and a workout from its lines (as a file's)
bool loadWorkout(const QStringList& lines, int ftp = 200) {
    AppState& state = appState();
    state.ftp = ftp;
    state.maxHr = 185;
    state.wheelCircumference = 2.105;
    state.workoutMode = true;
    state.cadenceMode = false;
    state.ftpTestMode.clear();
    state.antHRM = state.antSpeedCadence = true;
    state.ftpRanges = zones::powerRanges();
    AwoOptions options;
    options.ftp = ftp;
    const AwoResult result = parseAwoLines(lines, options);
    if (!result.ok()) {
        return false;
    }
    state.workoutList = result.workout.entries;
    state.workoutMessages = result.workout.messages;
    state.workoutTitle = result.workout.title;
    state.workoutDurationSeconds = result.workout.seconds;
    state.workoutFilename = state.workoutFilenameShort = QStringLiteral("test.awo");
    return true;
}

QStringList workoutFileLines(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QStringList lines = QString::fromUtf8(file.readAll()).remove(u'\r').split(u'\n');
    if (!lines.isEmpty() && lines.back().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

QString scratchpad(const QString& name) {
    return awotest::pythonAppDir() + QStringLiteral("/awoWorkouts/scratchpad/") + name;
}

// one tick of the screen's clock, a second long
void tickSecond(HudScreen& hud, SimulatedRider& rider) {
    hud.processEvents(1.0);
    const double t = appState().secondsPassed;
    saveSimulatedReading(t, rider.sample(t, hud.engine().currentPower(), appState().ftp));
}

// app_flow's workout (test_app_flow.py WORKOUT): 150 W, a ramp from 100 to 200 W, 2x 240 W / free, 320 W at FTP 200
const QStringList kAppFlowWorkout = {"T:App Flow", "1:10,0.75", "2:10,0.5,1.0", "3:2;1:5,1.2;0:5", "1:10,1.6"};
// one block intervals and same power ramps, as the setup screen writes them out (test_app_flow.py CONVERTED_WORKOUT)
const QStringList kConvertedWorkout = {"T:Converted", "2:20,0.6,0.6", "3:3;2:20,0.7,0.7|Up|5|10", "3:4;1:10,1.1|Go|3",
                                       "3:2;0:10", "3:2;2:10,0.7,0.7;0:10"};

}  // namespace

class TestHud : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { keys::install(qApp); }

    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        theme::current().reset();
        theme::setUiScale(1.0);
        Performance::current().reset();
        Units::current().reset();
        appState() = AppState();
        workoutData().clear();
        fake_ = std::make_unique<awotest::FakeAnt>();
        bridge_ = std::make_unique<StubBridge>(*fake_);
    }

    void cleanup() {
        bridge_.reset();
        fake_.reset();
        dir_.reset();
    }

    // ---- The Python workout screen's rides ----

    void ridesMatchThePythonWorkoutScreen_data() { awotest::addRideRows(); }

    // as make_golden.py rode them: the screen's own tick a second at a time (its clock frozen), the difficulty
    // slider moved and PAUSE pressed at the same ticks
    void ridesMatchThePythonWorkoutScreen() {
        QFETCH(QJsonObject, golden);
        const QJsonObject ride = golden[QStringLiteral("ride")].toObject();
        QVERIFY(awotest::setUpRide(ride));
        appState().ftpRanges = zones::powerRanges();
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        SimulatedRider rider(ride[QStringLiteral("seed")].toInt());
        const auto events = awotest::rideEvents(ride);
        hud.startWorkout();
        int ticks = 0;
        for (int tick = 0; tick < ride[QStringLiteral("seconds")].toInt(); ++tick) {
            if (!hud.engine().started()) {
                break;
            }
            for (const QJsonArray& event : events.value(tick)) {
                if (event[1].toString() == u"difficulty") {
                    hud.difficultySlider()->setValue(event[2].toInt());
                } else {
                    hud.pauseRecording();
                }
            }
            tickSecond(hud, rider);
            ++ticks;
        }
        awotest::compareRide(golden, ticks, hud.engine());
        if (QTest::currentTestFailed()) {
            return;
        }
        // a workout ridden to its end shows its grades (an FTP test its result, on the synopsis)
        QCOMPARE(hud.showingGrades(), golden[QStringLiteral("completed")].toBool() && appState().ftpTestMode.isEmpty());
    }

    // ---- What the spiral gets ----

    void spiralSampleCadence_data() {
        QTest::addColumn<bool>("cadenceSensor");
        QTest::addColumn<double>("powerAt");
        QTest::addColumn<double>("cadenceAt");  // (-1: no cadence reading)
        QTest::addColumn<double>("expected");   // (-1: none, the lane's outer edge)
        QTest::newRow("no cadence sensor: the outer edge") << false << 9.5 << -1.0 << -1.0;
        QTest::newRow("both coming in: the cadence") << true << 9.5 << 9.5 << 90.0;
        QTest::newRow("the cadence sensor quiet, the power not: the outer edge") << true << 9.5 << 2.0 << -1.0;
        QTest::newRow("both quiet: not pedaling") << true << 2.0 << 2.0 << 0.0;
    }

    // (test_power_spiral.py test_hud_sample_cadence) a tick's sample: the power and cadence of the last few seconds
    void spiralSampleCadence() {
        QFETCH(bool, cadenceSensor);
        QFETCH(double, powerAt);
        QFETCH(double, cadenceAt);
        QFETCH(double, expected);
        QVERIFY(loadWorkout({"1:60,0.5"}));
        AppState& state = appState();
        state.antSpeedCadence = state.antCadence = state.antSpeed = cadenceSensor;
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        hud.startWorkout();
        // (the first tick after START starts the workout's clock; the next takes it to 10 s)
        hud.processEvents(1.0);
        QVERIFY(hud.spiral()->block().samples().isEmpty());
        state.secondsPassed = 9.0;
        state.savedPowerVals = {{powerAt}, {200}};
        state.savedCadenceVals = cadenceAt >= 0 ? SavedValues{{cadenceAt}, {90}} : SavedValues{};
        hud.processEvents(1.0);
        QCOMPARE(state.secondsPassed, 10.0);
        const QList<RingSample>& samples = hud.spiral()->block().samples();
        QCOMPARE(samples.size(), qsizetype(1));
        QCOMPARE(samples.back().power, powerAt > 5 ? 200.0 : 0.0);
        QCOMPARE(samples.back().cadence, expected >= 0 ? std::optional<double>(expected) : std::nullopt);
    }

    // ---- START, PAUSE, END ----

    void startsPausesAndResumes() {
        QVERIFY(loadWorkout(kAppFlowWorkout));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        SimulatedRider rider;
        QVERIFY(!hud.pauseButton()->isEnabled());
        QVERIFY(!hud.endAvailable());
        // (before START: the clock runs, the workout doesn't)
        tickSecond(hud, rider);
        QCOMPARE(hud.engine().workoutSecondsPassed(), 0.0);
        hud.startEndButton()->click();
        QVERIFY(hud.engine().started() && hud.pauseButton()->isEnabled() && hud.endAvailable());
        QCOMPARE(layout::text(hud.startEndButton()), QStringLiteral("END"));
        // (the first tick after START starts the workout's clock)
        for (int i = 0; i < 4; ++i) {
            tickSecond(hud, rider);
        }
        QCOMPARE(hud.engine().workoutSecondsPassed(), 3.0);
        hud.pauseButton()->click();
        QVERIFY(!hud.engine().unpaused());
        tickSecond(hud, rider);
        QCOMPARE(hud.engine().workoutSecondsPassed(), 3.0);
        hud.pauseButton()->click();
        tickSecond(hud, rider);
        QCOMPARE(hud.engine().workoutSecondsPassed(), 4.0);
    }

    // END pauses a running workout while it asks; Keep Going goes on, and closing the prompt after doesn't pause again
    void endPromptPausesAndKeepGoingResumes() {
        QVERIFY(loadWorkout(kAppFlowWorkout));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        hud.startWorkout();
        hud.startEndButton()->click();
        ConfirmEndWorkoutWindow* prompt = hud.endPrompt();
        QVERIFY(prompt && prompt->isVisible());
        QVERIFY(prompt->restartButton());
        QVERIFY(!hud.engine().unpaused());
        prompt->keepGoingButton()->click();
        QVERIFY(hud.engine().unpaused() && !prompt->isVisible());
        prompt->close();
        QVERIFY(hud.engine().unpaused());
        // closing it unanswered goes on too
        hud.confirmEndWorkout();
        QVERIFY(!hud.engine().unpaused());
        hud.endPrompt()->close();
        QVERIFY(hud.engine().unpaused());
        // a workout already paused stays paused
        hud.pauseRecording();
        hud.confirmEndWorkout();
        hud.endPrompt()->keepGoingButton()->click();
        QVERIFY(!hud.engine().unpaused());
    }

    void endPromptGoesToTheSynopsis() {
        QVERIFY(loadWorkout(kAppFlowWorkout));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        SimulatedRider rider;
        hud.startWorkout();
        for (int i = 0; i < 25; ++i) {
            tickSecond(hud, rider);
        }
        QSignalSpy synopsis(&hud, &HudScreen::synopsisRequested);
        QSignalSpy setup(&hud, &HudScreen::setupRequested);
        hud.confirmEndWorkout();
        hud.endPrompt()->synopsisButton()->click();
        QCOMPARE(synopsis.count(), 1);
        QCOMPARE(setup.count(), 0);
        QVERIFY(!hud.engine().started());
        // the block being ridden is graded as far as it went
        const auto& items = appState().blockGrades.items();
        QVERIFY(items.size() >= 3);
        QVERIFY(items.back().second.endedEarly);
    }

    void endPromptGoesBackToSetup() {
        QVERIFY(loadWorkout(kAppFlowWorkout));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        hud.startWorkout();
        QSignalSpy setup(&hud, &HudScreen::setupRequested);
        hud.confirmEndWorkout();
        hud.endPrompt()->setupButton()->click();
        QCOMPARE(setup.count(), 1);
    }

    // Keep Existing Data and Restart: the workout from its start, the ride's readings kept
    void endPromptRestarts() {
        QVERIFY(loadWorkout(kAppFlowWorkout));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        SimulatedRider rider;
        hud.startWorkout();
        for (int i = 0; i < 15; ++i) {
            tickSecond(hud, rider);
        }
        const qsizetype readings = appState().savedPowerVals.times.size();
        hud.confirmEndWorkout();
        hud.endPrompt()->restartButton()->click();
        QCOMPARE(hud.engine().workoutSecondsPassed(), 0.0);
        QCOMPARE(appState().savedPowerVals.times.size(), readings);
        QCOMPARE(appState().secondsPassed, 15.0);
    }

    // free mode's prompt has no restart
    void freeRidePromptHasNoRestart() {
        AppState& state = appState();
        state.workoutMode = false;
        state.ftp = 200;
        state.ftpRanges = zones::powerRanges();
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        QVERIFY(hud.endAvailable());
        hud.startEndButton()->click();
        QVERIFY(hud.endPrompt() && !hud.endPrompt()->restartButton());
    }

    // ---- Whole workouts ----

    void scratchpadWorkoutsRideToTheEnd_data() {
        QTest::addColumn<QString>("workout");
        for (const char* name : {"01_quick_message_test.awo", "04_ramp_ladder_positions.awo", "ten_x.awo",
                                 "free_first_interval.awo", "shallow_ramp.awo", "ramp_timing.awo"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    // (test_screens.py) LayoutViewer's ride of the whole workout: every block ridden graded
    void scratchpadWorkoutsRideToTheEnd() {
        QFETCH(QString, workout);
        QVERIFY(loadWorkout(workoutFileLines(scratchpad(workout)), 250));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        SimulatedRider rider(3);
        hud.runSimulatedRide(appState().workoutDurationSeconds, rider);
        const AppState& state = appState();
        QVERIFY(!state.blockGrades.isEmpty());
        const QList<grading::GradeRow> rows = grading::workoutRows(state.workoutList, state.blockGrades);
        QVERIFY(!rows.isEmpty());
        for (const grading::GradeRow& row : rows) {
            QVERIFY(row.kind == grading::GradeRow::Kind::Interval || !row.label.isEmpty());
        }
    }

    // (test_screens.py) the ride to the end of a short workout: complete, its grades shown, END left
    void rideToTheEndShowsTheGrades() {
        QVERIFY(loadWorkout(kConvertedWorkout));
        QCOMPARE(appState().workoutDurationSeconds, 20 + 60 + 40 + 20 + 40);
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        SimulatedRider rider(3);
        hud.runSimulatedRide(appState().workoutDurationSeconds + 1, rider);
        QVERIFY(hud.engine().completeShown());
        QVERIFY(hud.showingGrades());
        QVERIFY(hud.endAvailable());
        QVERIFY(appState().blockGrades.items().size() >= 7);
        QVERIFY(grading::workoutAverage(appState().blockGrades).has_value());
    }

    // (test_screens.py test_spiral_follows_the_ride) the workout's ring and the block's spiral as far along as the
    // ride, an interval's spiral all its reps, with what was ridden in them
    void spiralFollowsTheRide() {
        // (10x 4s/2s, then 3x 2s/2s/2s: 40% of the way is in the first interval's reps)
        QVERIFY(loadWorkout(workoutFileLines(scratchpad(QStringLiteral("ten_x.awo"))), 250));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        SimulatedRider rider(3);
        hud.runSimulatedRide(appState().workoutDurationSeconds * 0.4, rider);
        const WorkoutEngine& engine = hud.engine();
        QVERIFY(engine.currentEventText().has_value());
        QCOMPARE(engine.currentKind(), 3);
        QCOMPARE(engine.currentEventText()->intervalBlocks.size(), qsizetype(2));
        const PowerSpiral* spiral = hud.spiral();
        QCOMPARE(spiral->workout().duration(), static_cast<double>(appState().workoutDurationSeconds));
        QVERIFY(qAbs(spiral->workout().fraction() - engine.workoutSecondsPassed() / appState().workoutDurationSeconds) < 1e-6);
        QCOMPARE(spiral->block().duration(), 10.0 * (4 + 2));
        QVERIFY(qAbs(spiral->block().seconds - engine.spiralBlockTime().first) < 1e-6);
        QVERIFY(spiral->workout().values().front().power.has_value());
        QVERIFY(!spiral->block().samples().isEmpty());
        for (const spiral::Timeline* timeline : {&spiral->workout(), &spiral->block()}) {
            for (const RingSample& sample : timeline->samples()) {
                QVERIFY(sample.seconds >= 0 && sample.seconds <= timeline->duration());
            }
        }
    }

    // ---- An FE-C trainer ----

    // (test_app_flow.py test_trainer_follows_the_workout) the trainer gets the workout's targets once START is
    // pressed; one over the Power Drive Cutoff is released, never asked for; the readings stopped hold it at 0 W
    void trainerFollowsTheWorkout() {
        QVERIFY(loadWorkout(kAppFlowWorkout));
        appState().powerDriveCutoff = 300;
        bridge_->trainer = true;
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        hud.startReadings();
        SimulatedRider rider;
        tickSecond(hud, rider);
        QVERIFY(bridge_->targets.isEmpty());
        hud.startWorkout();
        for (int i = 0; i < 60; ++i) {
            tickSecond(hud, rider);
        }
        QSet<int> watts;
        for (const std::optional<double>& target : bridge_->targets) {
            QVERIFY(target.has_value());
            watts.insert(qRound(*target));
        }
        QVERIFY(watts.contains(150) && watts.contains(240));
        QVERIFY(std::any_of(watts.begin(), watts.end(), [](int w) { return w > 100 && w < 200 && w != 150; }));
        QVERIFY(bridge_->releases > 0);
        QVERIFY(!watts.contains(320));
        hud.confirmEndWorkout();
        hud.endPrompt()->synopsisButton()->click();
        QCOMPARE(bridge_->targets.back(), std::optional<double>(0));
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
    std::unique_ptr<awotest::FakeAnt> fake_;
    std::unique_ptr<StubBridge> bridge_;
};

QTEST_MAIN(TestHud)
#include "tst_hud.moc"
