// The workout screen (src/ui/screens/HudScreen) built offscreen on a stand-in bridge: the Python workout screen's own
// rides (golden/hud.json) ridden through it ask for the same power and grade the same; what it gives the spiral;
// START, PAUSE and the END prompt's choices; whole workouts ridden to the end and graded; the spiral following the
// ride; and an FE-C trainer following the workout.

#include <QApplication>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QtTest>

#include <algorithm>

#include "AnalogGauge.hpp"
#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "AppState.hpp"
#include "AwoParser.hpp"
#include "BlockBubble.hpp"
#include "ConfirmEndWorkoutWindow.hpp"
#include "FakeAnt.hpp"
#include "FtpTest.hpp"
#include "GoldenRides.hpp"
#include "GradeWidgets.hpp"
#include "HatchedLabel.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"
#include "MaxStar.hpp"
#include "Performance.hpp"
#include "PowerSpiral.hpp"
#include "PyCompat.hpp"
#include "Session.hpp"
#include "SimulatedRider.hpp"
#include "SmoothProgressBar.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutData.hpp"
#include "WorkoutGrading.hpp"
#include "WorkoutGraph.hpp"
#include "ZoneLegend.hpp"
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
constexpr double BubbleLead = HudScreen::kBubbleLeadSeconds;

void tickSecond(HudScreen& hud, SimulatedRider& rider) {
    hud.processEvents(1.0);
    const double t = appState().secondsPassed;
    saveSimulatedReading(t, rider.sample(t, hud.engine().currentPower(), appState().ftp, hud.cadenceMode()));
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
    void animationsFollowTheFrameRate() {
        QVERIFY(loadWorkout({"1:60,0.5"}));
        HudScreen hud(bridge_.get());
        const auto timersAt = [&hud](int ms) {
            int count = 0;
            for (const QTimer* timer : hud.findChildren<QTimer*>(Qt::FindDirectChildrenOnly)) {
                count += timer->interval() == ms ? 1 : 0;
            }
            return count;
        };
        // the bars and spiral, and the graph's stars
        Performance::current().setGaugeFps(120);
        QCOMPARE(timersAt(8), 2);
        Performance::current().setGaugeFps(60);
        QCOMPARE(timersAt(16), 2);
        hud.stopTimers();
    }

    // what's queued for the layout, the side gauges following it, run
    static void settle() {
        for (int i = 0; i < 6; ++i) {
            QCoreApplication::processEvents();
        }
    }

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
        // (Musical Cadence Mode: the music silent, a second a tick)
        if (hud.cadenceMode()) {
            hud.simulateMusic(0.0);
        }
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
            if (hud.simulatedMusic()) {
                hud.simulateMusic(*hud.simulatedMusic() + 1);
            }
            tickSecond(hud, rider);
            ++ticks;
        }
        awotest::compareRide(golden, ticks, hud.engine());
        if (QTest::currentTestFailed()) {
            return;
        }
        // a workout ridden to its end shows its grades (an FTP test its result, on the synopsis), and so does one
        // ridden well into its last block (not as the Python app, which waits for the end: see CHANGES.md)
        const bool intoLastBlock = hud.engine().upcoming().isEmpty() &&
                                   hud.engine().spiralBlockTime().first >= HudScreen::kLastBlockNoteSeconds;
        QCOMPARE(hud.showingGrades(), (golden[QStringLiteral("completed")].toBool() || intoLastBlock) &&
                                          appState().ftpTestMode.isEmpty());
    }

    // ---- What the spiral gets ----

    void spiralSampleCadence_data() {
        QTest::addColumn<bool>("cadenceSensor");
        QTest::addColumn<double>("powerAt");
        QTest::addColumn<double>("cadenceAt");  // (-1: no cadence reading)
        QTest::addColumn<double>("expected");   // (-1: none, the lane's outer edge)
        QTest::newRow("no cadence sensor: the outer edge") << false << 9.5 << -1.0 << -1.0;
        QTest::newRow("both coming in: the cadence") << true << 9.5 << 9.5 << 90.0;
        QTest::newRow("between pedal strokes: the last cadence") << true << 9.5 << 7.0 << 90.0;
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

    // (test_power_spiral.py test_power_rings_put_the_power_now_in_the_middle) the power now innermost, then out from
    // it each finished block's average, newest to oldest (a reading every tick: 100 W, 300 W from 3 s, the last 200 W)
    void powerRingsPutThePowerNowInTheMiddle() {
        AppState& state = appState();
        state.ftp = 250;
        std::unique_ptr<Gauge> gauge(newPowerGauge());
        AnalogGauge* w = gauge->gauge();
        w->centerRingMinPx.reset();
        w->centerRingCount = 5;
        PowerRings rings;
        for (int tick = 1; tick <= 50; ++tick) {
            state.secondsPassed = tick / 10.0;
            const double power = tick == 50 ? 200 : tick < 30 ? 100 : 300;
            state.savedPowerVals.times.append(state.secondsPassed);
            state.savedPowerVals.values.append(power);
            w->setValue(power);
            rings.update(gauge.get());
        }
        // the blocks end half a tick after the first (0.1 s): the newest finished one ends at 4.55 s, the oldest of
        // the four shown (2.55 to 3.05 s) has four 100 W readings and one 300 W
        QVERIFY(w->centerRings());
        QList<QColor> expected;
        for (const double power : {140, 300, 300, 300, 200}) {
            expected.append(zones::zoneColor(power, zones::powerRanges()));
        }
        QCOMPARE(w->centerRings()->first, expected);
    }

    // (test_power_spiral.py test_cadence_rings_gear_turns_and_beats_with_the_chainring, the beat's half) outside
    // Musical Cadence Mode the cadence gauge's rings are brightest as the chainring comes round, fading over
    // kBeatSeconds; free ride's color
    void cadenceRingsBeatWithTheChainring() {
        AppState& state = appState();
        state.workoutMode = false;
        state.antCadence = true;
        state.ftp = 200;
        state.ftpRanges = zones::powerRanges();
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        AnalogGauge* w = hud.cadenceGauge()->gauge();
        RotatingSvgWidget* chainring = hud.cadenceGauge()->chainring();
        QVERIFY(w->centerRings());
        QCOMPARE(w->centerRings()->second, 0.0);
        w->setMaxValue(200);
        w->setValue(60);
        // each frame the chainring turns: (60 rpm, 360 degrees a second) the time since it came round
        int frames = 0;
        connect(chainring, &RotatingSvgWidget::turned, &hud, [&] {
            const double level = std::max(0.0, 1 - chainring->angle() / 360 / kBeatSeconds);
            QVERIFY(std::abs(w->centerRings()->second - level) < 0.02);
            ++frames;
        });
        QTRY_VERIFY(frames >= 5);
    }

    // outside Musical Cadence Mode the gear and circles are the band's color at the needle, whatever the block's target
    void cadenceRingsFollowTheBandAtTheNeedle() {
        AppState& state = appState();
        state.workoutMode = false;
        state.antCadence = true;
        state.ftp = 200;
        state.ftpRanges = zones::powerRanges();
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        AnalogGauge* w = hud.cadenceGauge()->gauge();
        const auto ringsAt = [](double rpm) {
            const QColor color = zones::zoneColor(rpm, ftpcolors::cadenceRanges(ftpcolors::kCadenceScaleBottomRpm));
            return QList<QColor>{color.darker(130), color};
        };
        // no pedaling: the band's start, not free ride's grey
        QCOMPARE(w->centerRings()->first, ringsAt(0));
        QVERIFY(w->centerRings()->first[1] != QColor(QStringLiteral("#") + zones::gaugeRing(0)));
        w->setValue(60);
        w->snapNeedle();
        QTRY_COMPARE(w->centerRings()->first, ringsAt(60));
        // just past a zone's start: that zone's color, the needle's and not the reading's while it's on its way
        w->setValue(91);
        w->snapNeedle();
        QTRY_COMPARE(w->centerRings()->first, ringsAt(91));
        QVERIFY(ringsAt(91) != ringsAt(89));
    }

    // standard and Musical Cadence workouts alike, with a target and paused: the gear's color is the band's at the
    // needle, and the power gauge's center and the glow behind it are the power's
    void gaugesFollowTheirReadingsNotTheWorkout_data() {
        QTest::addColumn<bool>("music");
        QTest::newRow("standard workout") << false;
        QTest::newRow("Musical Cadence workout") << true;
    }

    void gaugesFollowTheirReadingsNotTheWorkout() {
        QFETCH(bool, music);
        QVERIFY(loadWorkout(kAppFlowWorkout));
        AppState& state = appState();
        state.cadenceMode = music;
        const ftpcolors::ZoneRanges cadence = ftpcolors::cadenceRanges(ftpcolors::kCadenceScaleBottomRpm);
        if (music) {
            state.ftpRanges = cadence;
        }
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        QCOMPARE(hud.cadenceMode(), music);
        if (music) {
            hud.simulateMusic(0.0);
        }
        AnalogGauge* gear = hud.cadenceGauge()->gauge();
        const auto gearAt = [&](double rpm) {
            const QColor color = zones::zoneColor(rpm, cadence);
            return QList<QColor>{color.darker(130), color};
        };
        // on the band's gradient from the start, not white or free ride's grey
        QCOMPARE(gear->centerRings()->first, gearAt(0));
        const auto glowAt = [](int watts) {
            return QColor(QStringLiteral("#44") + zones::zoneHex(watts, zones::powerRanges()).mid(1));
        };
        AnalogGauge* power = hud.powerGauge()->gauge();
        QCOMPARE(power->outerCircleBg().back().second, glowAt(0));

        const auto check = [&] {
            gear->setValue(91);
            gear->snapNeedle();
            QTRY_COMPARE(gear->centerRings()->first, gearAt(91));
            hud.showPower(230);
            QCOMPARE(power->outerCircleBg().back().second, glowAt(230));
            hud.updatePowerRings();
            QCOMPARE(power->centerRings()->first.back(), zones::zoneColor(230, zones::powerRanges()));
            gear->setValue(40);
            gear->snapNeedle();
            QTRY_COMPARE(gear->centerRings()->first, gearAt(40));
            hud.showPower(100);
            QCOMPARE(power->outerCircleBg().back().second, glowAt(100));
        };
        check();
        QVERIFY(!QTest::currentTestFailed());
        hud.startWorkout();
        QVERIFY(hud.engine().started());
        // (the first tick after START starts the workout's clock)
        hud.processEvents(1.0);
        hud.processEvents(1.0);
        QVERIFY(hud.engine().currentPower() != 0);
        check();
        QVERIFY(!QTest::currentTestFailed());
        hud.pauseButton()->click();
        QVERIFY(!hud.engine().unpaused());
        check();
        QVERIFY(!QTest::currentTestFailed());
    }

    // outside Musical Cadence Mode the cadence gauge has a band too, colored by the cadence zones (not the power
    // zones the rest of the screen uses), filled up to the needle, the chainring inside it
    void cadenceGaugeBandFollowsTheCadenceZones() {
        AppState& state = appState();
        state.workoutMode = false;
        state.antCadence = true;
        state.ftp = 200;
        state.ftpRanges = zones::powerRanges();
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        AnalogGauge* w = hud.cadenceGauge()->gauge();
        AnalogGauge expected;
        expected.setMaxValue(w->maxValue());
        expected.setValueColors(zones::zoneStops(ftpcolors::cadenceRanges(ftpcolors::kCadenceScaleBottomRpm)));
        QCOMPARE(w->scalePolygonColors(), expected.scalePolygonColors());
        QCOMPARE(hud.cadenceGauge()->chainring()->scale, w->gaugeColorInnerRadiusFactor);
    }

    // ---- The side gauges ----

    // every side gauge's box is its outer arc's circle, nothing around it; both one size, on one line, mirrored about
    // the middle gauge and clear of everything around them; and they don't change when the grades take over (the grades
    // list gives way instead); the legend and timer rows narrow just as far as that takes
    void workoutSideGaugesMirrorAboutTheMiddleGauge() {
        QVERIFY(loadWorkout({QStringLiteral("1:60,0.5"), QStringLiteral("1:60,0.6")}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1896, 1092);
        hud.show();
        settle();
        const Gauge* left = hud.cadenceGauge();
        const Gauge* right = hud.heartRateGauge();
        const QRect leftAt = left->geometry();
        const QRect rightAt = right->geometry();
        QCOMPARE(left->gauge()->size(), leftAt.size());
        QCOMPARE(leftAt.width(), leftAt.height());
        QVERIFY(leftAt.width() > 200);
        QCOMPARE(rightAt.size(), leftAt.size());
        QCOMPARE(rightAt.center().y(), leftAt.center().y());
        const QRect middle(hud.powerGauge()->gauge()->mapTo(&hud, QPoint(0, 0)), hud.powerGauge()->gauge()->size());
        const double middleRadius = std::min(middle.width(), middle.height()) / 2.0;
        const double radius = leftAt.width() / 2.0;
        const auto fromMiddle = [&](const QRect& gauge) {
            return std::hypot(QRectF(gauge).center().x() - QRectF(middle).center().x(),
                              QRectF(gauge).center().y() - QRectF(middle).center().y()) -
                   middleRadius - radius;
        };
        constexpr double room = 10;
        QVERIFY(std::abs(fromMiddle(leftAt) - fromMiddle(rightAt)) <= 2);
        QVERIFY(fromMiddle(leftAt) >= room);
        const auto clear = [&](const QRect& gauge, const QWidget* widget) {
            const QRectF rect(widget->mapTo(&hud, QPoint(0, 0)), widget->size());
            const QPointF at = QRectF(gauge).center();
            const double dx = std::max({rect.left() - at.x(), 0.0, at.x() - rect.right()});
            const double dy = std::max({rect.top() - at.y(), 0.0, at.y() - rect.bottom()});
            return std::hypot(dx, dy) - radius >= room;
        };
        const auto* legend = hud.findChild<ZoneLegend*>();
        QVERIFY(clear(leftAt, legend));
        QVERIFY(clear(rightAt, hud.remainingTimeLabel()));
        QVERIFY(clear(rightAt, hud.workoutTimeLabel()));
        // (the rest of the column stays as wide as it was)
        QVERIFY(legend->width() < HudScreen::kUpNextWidth);
        const QList<QFrame*> bubbles = hud.findChildren<QFrame*>(QStringLiteral("infoBubble"));
        QCOMPARE(bubbles.size(), qsizetype(4));
        for (const QFrame* bubble : bubbles) {
            QVERIFY(clear(rightAt, bubble));
        }
        // the timer rows narrower than the column above them, against its right edge, each its own bubble
        const QRect spiral(hud.spiral()->mapTo(&hud, QPoint(0, 0)), hud.spiral()->size());
        for (const QWidget* row : {static_cast<const QWidget*>(hud.workoutTimeLabel()),
                                   static_cast<const QWidget*>(hud.workoutBar()),
                                   static_cast<const QWidget*>(hud.remainingTimeLabel())}) {
            const QRect rect(row->mapTo(&hud, QPoint(0, 0)), row->size());
            QCOMPARE(rect.right(), spiral.right());
            QVERIFY(rect.width() <= spiral.width());
        }

        SimulatedRider rider(3);
        hud.runSimulatedRide(appState().workoutDurationSeconds - 5, rider);
        hud.stopTimers();
        QVERIFY(hud.showingGrades());
        settle();
        QCOMPARE(left->geometry(), leftAt);
        QCOMPARE(right->geometry(), rightAt);
        QVERIFY(clear(leftAt, hud.gradeList()));
    }

    // free ride's side gauges alike, between the middle gauge and the screen's edge or the trainer's slider; its speed
    // through elapsed time in four bubbles (no battery warning's room among them, nor speed and distance's without a
    // speed sensor)
    void freeRideSideGaugesFitBetweenTheMiddleGaugeAndTheEdge() {
        for (const bool trainer : {false, true}) {
            QVERIFY(loadWorkout({QStringLiteral("1:60,0.5")}));
            AppState& state = appState();
            state.workoutMode = false;
            bridge_->trainer = trainer;
            HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
            hud.setAttribute(Qt::WA_DontShowOnScreen);
            hud.resize(1896, 1092);
            hud.show();
            settle();
            hud.stopTimers();
            bridge_->trainer = false;
            const QRect left = hud.cadenceGauge()->geometry();
            const QRect right = hud.heartRateGauge()->geometry();
            QCOMPARE(right.size(), left.size());
            QCOMPARE(right.center().y(), left.center().y());
            QVERIFY(left.left() >= 0);
            QVERIFY(left.right() < hud.graph()->mapTo(&hud, QPoint(0, 0)).x() + hud.graph()->width());
            QVERIFY(left.width() > 300);
            int edge = hud.width();
            if (trainer) {
                QVERIFY(hud.difficultySlider() != nullptr);
                edge = hud.difficultySlider()->mapTo(&hud, QPoint(0, 0)).x();
            }
            QVERIFY(right.right() < edge);
            const QList<QFrame*> bubbles = hud.findChildren<QFrame*>(QStringLiteral("infoBubble"));
            QCOMPARE(bubbles.size(), qsizetype(4));
            for (const QFrame* bubble : bubbles) {
                QVERIFY(hud.batteryWarning()->geometry().bottom() < bubble->geometry().top());
                // tall enough to be rounded, spaced like a workout's timer rows
                QVERIFY(bubble->height() >= 32);
            }
            QList<QFrame*> rows = bubbles;
            std::sort(rows.begin(), rows.end(), [](const QFrame* a, const QFrame* b) { return a->y() < b->y(); });
            for (qsizetype i = 1; i < rows.size(); ++i) {
                QCOMPARE(rows[i]->geometry().top() - rows[i - 1]->geometry().bottom() - 1, 6);
            }
        }
        QVERIFY(loadWorkout({QStringLiteral("1:60,0.5")}));
        AppState& state = appState();
        state.workoutMode = false;
        state.antSpeedCadence = false;
        state.antCadence = false;
        state.antSpeed = false;
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1896, 1092);
        hud.show();
        settle();
        hud.stopTimers();
        int shown = 0;
        for (const QFrame* bubble : hud.findChildren<QFrame*>(QStringLiteral("infoBubble"))) {
            shown += bubble->isVisible() ? 1 : 0;
        }
        QCOMPARE(shown, 2);
    }

    // a screen goes on as what it was built as: on its way out, with the next ride's mode already set, a free ride
    // screen has no workout's legend or timers to place its gauges against (nor a workout's anything to miss in free)
    void sideGaugesFollowTheScreensOwnModeNotTheNextRides() {
        QVERIFY(loadWorkout({QStringLiteral("1:60,0.5")}));
        appState().workoutMode = false;
        HudScreen freeRide(bridge_.get(), nullptr, [] { return 1000.0; });
        freeRide.setAttribute(Qt::WA_DontShowOnScreen);
        freeRide.resize(1896, 1092);
        freeRide.show();
        settle();
        freeRide.stopTimers();
        const QRect left = freeRide.cadenceGauge()->geometry();
        appState().workoutMode = true;
        freeRide.fitWorkoutMessage();
        QCOMPARE(freeRide.cadenceGauge()->geometry(), left);

        HudScreen workout(bridge_.get(), nullptr, [] { return 1000.0; });
        workout.setAttribute(Qt::WA_DontShowOnScreen);
        workout.resize(1896, 1092);
        workout.show();
        settle();
        workout.stopTimers();
        const QRect workoutLeft = workout.cadenceGauge()->geometry();
        appState().workoutMode = false;
        workout.fitWorkoutMessage();
        QCOMPARE(workout.cadenceGauge()->geometry(), workoutLeft);
    }

    // the heart rate gauge's outer arc in its hearts' colors, and no needle behind the heart
    void heartRateGaugeHasAnArcAndNoNeedleTail() {
        const int fps = Performance::current().gaugeFps();
        Performance::current().setGaugeFps(0);
        std::unique_ptr<Gauge> gauge(newHeartRateGauge(185));
        gauge->heart()->beatTimer()->stop();
        AnalogGauge* w = gauge->gauge();
        QCOMPARE(w->scalePolygonColors().front().second, QColor(kHeartPink));
        gauge->layout()->setContentsMargins(0, 0, 0, 0);
        gauge->resize(100, 100);
        w->setValue(146);
        QImage image(gauge->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        gauge->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        // (the needle points up and right: where its tail would be, down and left of the middle, outside the heart)
        QCOMPARE(qAlpha(image.pixel(50 - 20, 50 + 20)), 0);
        QVERIFY(qAlpha(image.pixel(50 + 20, 50 - 21)) > 0);
        Performance::current().setGaugeFps(fps);
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

    // click() (Space, a key's shortcut) lets the button up without asking for a repaint, and setDown(false) after it
    // has nothing left to change: unpausing still has to repaint, or the pressed color stays on
    void unpausingRepaintsTheButton_data() {
        QTest::addColumn<bool>("byMouse");
        QTest::newRow("key") << false;
        QTest::newRow("mouse") << true;
    }

    void unpausingRepaintsTheButton() {
        QFETCH(bool, byMouse);
        QVERIFY(loadWorkout(kAppFlowWorkout));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        hud.show();
        hud.startEndButton()->click();
        const auto press = [&] {
            if (byMouse) {
                QTest::mouseClick(hud.pauseButton(), Qt::LeftButton);
            } else {
                hud.pauseButton()->click();
            }
        };
        press();
        QVERIFY(!hud.engine().unpaused() && hud.pauseButton()->isDown());
        QApplication::processEvents();

        class PaintCounter : public QObject {
        public:
            using QObject::QObject;
            int paints = 0;
            bool eventFilter(QObject*, QEvent* event) override {
                paints += event->type() == QEvent::Paint;
                return false;
            }
        } counter;
        hud.pauseButton()->installEventFilter(&counter);
        press();
        QVERIFY(hud.engine().unpaused() && !hud.pauseButton()->isDown());
        QApplication::processEvents();
        QVERIFY(counter.paints > 0);
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

    // the last block's "Up Next:" and "last block" note go 30 s into it, a last block 30 s or shorter keeps them
    void lastBlockNoteGoesAfter30Seconds() {
        QVERIFY(loadWorkout({"1:10,0.5", "1:40,0.6"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        SimulatedRider rider;
        hud.startWorkout();
        // (on to the last block)
        for (int i = 0; i < 20 && hud.engine().activeWorkout() < 1; ++i) {
            tickSecond(hud, rider);
        }
        QCOMPARE(hud.upNextHeader()->text(), QStringLiteral("Up Next:"));
        QCOMPARE(hud.upNextLabel()->text(), QStringLiteral("You're on the last block, way to go!"));
        while (hud.engine().blockSecondsPassed() < 29) {
            tickSecond(hud, rider);
        }
        QCOMPARE(hud.engine().blockSecondsPassed(), 29.0);
        QCOMPARE(hud.upNextLabel()->text(), QStringLiteral("You're on the last block, way to go!"));
        tickSecond(hud, rider);
        QCOMPARE(hud.upNextHeader()->text(), QString());
        QCOMPARE(hud.upNextLabel()->text(), QString());
        // a restart brings them back
        hud.restartWorkout();
        QCOMPARE(hud.upNextHeader()->text(), QStringLiteral("Up Next:"));

        QVERIFY(loadWorkout({"1:10,0.5", "1:30,0.6"}));
        HudScreen shortLast(bridge_.get(), nullptr, [] { return 1000.0; });
        shortLast.stopTimers();
        shortLast.startWorkout();
        for (int i = 0; i < 20 && shortLast.engine().activeWorkout() < 1; ++i) {
            tickSecond(shortLast, rider);
        }
        while (shortLast.engine().blockSecondsPassed() < 29) {
            tickSecond(shortLast, rider);
        }
        QCOMPARE(shortLast.upNextLabel()->text(), QStringLiteral("You're on the last block, way to go!"));

        // a ramp test keeps them: its next block comes just before this one ends
        QVERIFY(loadWorkout(ftptest::workoutLines(QStringLiteral("ramp10"))));
        appState().ftpTestMode = QStringLiteral("ramp10");
        HudScreen ramp(bridge_.get(), nullptr, [] { return 1000.0; });
        ramp.stopTimers();
        QVERIFY(ramp.engine().rampTest());
        ramp.startWorkout();
        while (ramp.engine().blockSecondsPassed() < 40) {
            tickSecond(ramp, rider);
        }
        QCOMPARE(ramp.upNextHeader()->text(), QStringLiteral("Up Next:"));
        QVERIFY(!ramp.upNextLabel()->text().isEmpty());
        QCOMPARE(shortLast.upNextLabel()->text(), QStringLiteral("You're on the last block, way to go!"));
    }

    // the grades take over the left column as the note goes, the last block's a blinking question mark until it's set
    void lastBlockGradesShowEarly() {
        QVERIFY(loadWorkout({"1:10,0.5", "1:40,0.6"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        hud.show();
        SimulatedRider rider;
        hud.startWorkout();
        for (int i = 0; i < 20 && hud.engine().activeWorkout() < 1; ++i) {
            tickSecond(hud, rider);
        }
        QVERIFY(!hud.showingGrades());
        QVERIFY(!hud.gradeList()->isVisible());
        while (hud.engine().blockSecondsPassed() < 30) {
            tickSecond(hud, rider);
        }
        QVERIFY(hud.showingGrades());
        QVERIFY(hud.gradeList()->isVisible());
        QVERIFY(!hud.upNextLabel()->isVisible());
        QCOMPARE(hud.gradeList()->rows().size(), 2);
        QVERIFY(hud.gradeList()->rows()[0].grade != grading::kPendingGrade);
        QCOMPARE(hud.gradeList()->rows()[1].grade, grading::kPendingGrade);
        QVERIFY(hud.gradeList()->blinking());
        // the average's still to come too: its grade and percentage blink, the text beside them stays put
        QVERIFY(hud.gradeList()->averagePending());
        QVERIFY(!hud.gradeList()->average());
        QTRY_VERIFY(hud.gradeList()->pendingMarkShown());
        const QImage markOn = hud.gradeList()->grab().toImage();
        QTRY_VERIFY(!hud.gradeList()->pendingMarkShown());
        const QImage markOff = hud.gradeList()->grab().toImage();
        const double scale = theme::uiScale();
        const int averageTop = markOn.height() - static_cast<int>(GradeList::kAverageHeight * scale);
        const int badge = static_cast<int>(GradeList::kBadgeWidth * scale);
        const int percent = static_cast<int>(60 * scale);
        const auto part = [&](const QImage& image, int x, int width) {
            return image.copy(x, averageTop, width, markOn.height() - averageTop);
        };
        QVERIFY(part(markOn, 0, badge) != part(markOff, 0, badge));
        QVERIFY(part(markOn, markOn.width() - percent, percent) != part(markOff, markOn.width() - percent, percent));
        QCOMPARE(part(markOn, badge, markOn.width() - percent - badge), part(markOff, badge, markOn.width() - percent - badge));
        // set at the end
        while (hud.engine().started()) {
            tickSecond(hud, rider);
        }
        QVERIFY(hud.showingGrades());
        QCOMPARE(hud.gradeList()->rows().size(), 2);
        QVERIFY(hud.gradeList()->rows()[1].grade != grading::kPendingGrade);
        QVERIFY(!hud.gradeList()->blinking());
        QVERIFY(!hud.gradeList()->averagePending());
        QVERIFY(hud.gradeList()->average());
        QVERIFY(hud.gradeList()->average() == grading::workoutAverage(appState().blockGrades));
        // a restart brings the column back
        hud.restartWorkout();
        QVERIFY(!hud.showingGrades());
        QVERIFY(!hud.gradeList()->isVisible());
        QVERIFY(hud.upNextLabel()->isVisible());
        QVERIFY(!hud.gradeList()->blinking());
    }

    // an interval's blocks not being ridden: fainter text and lines over the box, the one being ridden plain, and the
    // lines' pixmap only made again when it has to be
    void inactiveIntervalBlocksHatched() {
        QVERIFY(loadWorkout({"1:10,0.5", "3:2;1:20,0.8;1:20,0.6;1:20,0.7", "1:60,0.5"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        hud.resize(1920, 1116);
        hud.show();
        SimulatedRider rider;
        hud.startWorkout();
        while (hud.engine().currentKind() != 3 || hud.engine().currentIntervalBlockIndex() != 1) {
            tickSecond(hud, rider);
        }
        QList<HatchedLabel*> shown;
        for (HatchedLabel* label : hud.findChildren<HatchedLabel*>()) {
            if (label->isVisible()) {
                shown.append(label);
            }
        }
        QCOMPARE(shown.size(), 3);
        std::sort(shown.begin(), shown.end(), [](auto* a, auto* b) { return a->y() < b->y(); });
        QVERIFY(!shown[0]->isEnabled() && shown[1]->isEnabled() && !shown[2]->isEnabled());
        QVERIFY(theme::originalSheet(shown[0]).contains(QStringLiteral("QLabel:disabled{color:#59")));
        const auto colorsNearEnd = [](QLabel* label) {
            const QImage image = label->grab().toImage();
            QSet<QRgb> colors;
            for (int x = 30; x < 80; ++x) {
                colors.insert(image.pixel(x, image.height() / 2));
            }
            return colors;
        };
        QVERIFY(colorsNearEnd(shown[0]).size() > 3);
        QCOMPARE(shown[1]->hatchBuilds(), 0);
        const int builds = shown[0]->hatchBuilds();
        QVERIFY(builds >= 1);
        shown[0]->grab();
        QCOMPARE(shown[0]->hatchBuilds(), builds);
    }

    // an interval's count: a narrow column the block's full height, holding the most it can be; the rest beside it, in
    // line, and the whole width again on a plain block
    void intervalCountIsANarrowColumn() {
        QVERIFY(loadWorkout({"1:10,0.5", "3:1000;1:20,0.8;1:20,0.6", "1:60,0.5"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        hud.resize(1920, 1116);
        hud.show();
        SimulatedRider rider;
        hud.startWorkout();
        QApplication::processEvents();
        QLabel* time = hud.blockRemainingLabel();
        QLabel* plain = hud.blockLabel();
        const int wholeWidth = time->width();
        const int left = time->x();
        QVERIFY(!hud.blockCountLabel()->isVisible());
        QCOMPARE(plain->x(), left);
        QCOMPARE(plain->width(), wholeWidth);

        while (hud.engine().currentKind() != 3) {
            tickSecond(hud, rider);
        }
        QApplication::processEvents();
        QLabel* count = hud.blockCountLabel();
        QCOMPARE(count->text(), QStringLiteral("1000x"));
        QVERIFY(count->isVisible());
        const int textWidth = count->fontMetrics().horizontalAdvance(count->text());
        QVERIFY2(count->width() >= textWidth + 8, qPrintable(QStringLiteral("%1 %2").arg(count->width()).arg(textWidth)));
        QVERIFY2(count->width() < wholeWidth / 8, qPrintable(QString::number(count->width())));
        QCOMPARE(count->x(), left);
        QCOMPARE(count->y(), plain->y());
        QCOMPARE(count->geometry().bottom(), time->geometry().bottom());
        QVERIFY(time->x() > count->geometry().right());
        QCOMPARE(time->x() + time->width(), left + wholeWidth);
        int shown = 0;
        for (QLabel* label : hud.intervalLabels()) {
            if (label->isVisible()) {
                ++shown;
                QCOMPARE(label->x(), time->x());
                QCOMPARE(label->width(), time->width());
            }
        }
        QCOMPARE(shown, 2);
    }

    // a last interval: each repeat's grade as it ends, each block's own once all its repeats have theirs
    void lastIntervalGradesFillIn() {
        QVERIFY(loadWorkout({"1:10,0.5", "3:2;1:20,0.8;1:20,0.6"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        SimulatedRider rider;
        hud.startWorkout();
        while (hud.engine().spiralBlockTime().first < 30 || hud.engine().currentKind() != 3) {
            tickSecond(hud, rider);
        }
        QVERIFY(hud.showingGrades());
        QList<GradeList::Row> rows = hud.gradeList()->rows();
        // (the first block, "2x", and the interval's two blocks: 30 s in, the first's first repeat is done)
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[1].kind, GradeList::Row::Kind::Interval);
        QCOMPARE(rows[2].grade, grading::kPendingGrade);
        QVERIFY(rows[2].note.endsWith(QStringLiteral(" · ?")));
        QVERIFY(!rows[2].note.startsWith(grading::kPendingGrade));
        QCOMPARE(rows[3].grade, grading::kPendingGrade);
        QCOMPARE(rows[3].note, QStringLiteral("? · ?"));
        // the first block's second repeat ends: its own grade set, the second block's still to come
        while (hud.engine().spiralBlockTime().first < 61) {
            tickSecond(hud, rider);
        }
        rows = hud.gradeList()->rows();
        QVERIFY(rows[2].grade != grading::kPendingGrade);
        QVERIFY(!rows[2].note.contains(grading::kPendingGrade));
        QCOMPARE(rows[3].grade, grading::kPendingGrade);
        QVERIFY(hud.gradeList()->blinking());
        // (the average waits for all of them, not just the blocks that are done)
        QVERIFY(hud.gradeList()->averagePending());
        while (hud.engine().started()) {
            tickSecond(hud, rider);
        }
        rows = hud.gradeList()->rows();
        QVERIFY(rows[3].grade != grading::kPendingGrade);
        QVERIFY(!rows[3].note.contains(grading::kPendingGrade));
        QVERIFY(!hud.gradeList()->blinking());
        QVERIFY(!hud.gradeList()->averagePending());
    }

    // an FTP test isn't graded: the column just clears, the same as before
    void ftpTestShowsNoGradesEarly() {
        QVERIFY(loadWorkout({"1:10,0.5", "1:40,0.6"}));
        appState().ftpTestMode = QStringLiteral("20m");
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        SimulatedRider rider;
        hud.startWorkout();
        while (hud.engine().activeWorkout() < 1 || hud.engine().blockSecondsPassed() < 31) {
            tickSecond(hud, rider);
        }
        QCOMPARE(hud.upNextLabel()->text(), QString());
        QVERIFY(!hud.showingGrades());
        QVERIFY(!hud.engine().pendingBlock());
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

    // ---- The block bubbles ----

    // each completed block gets a bubble for the graph with its power and cadence readings' averages; a restart
    // clears them
    void blockBubblesShowEachCompletedBlock() {
        QVERIFY(loadWorkout({"T:Bubbles", "1:120,0.75", "1:120,1.0", "1:120,0.5"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        while (hud.engine().workoutSecondsPassed() < 250) {
            tickSecond(hud, rider);
        }
        QCOMPARE(hud.blockBubbles().size(), qsizetype(2));
        const BlockBubble* first = hud.blockBubbles().front();
        QCOMPARE(first->lines().size(), 2);
        // (the first block's readings, its 120 s up to where it ended)
        const auto average = [first](const SavedValues& saved) {
            double sum = 0;
            int count = 0;
            for (qsizetype i = 0; i < saved.times.size() && saved.times[i] < first->end(); ++i) {
                if (saved.times[i] < first->end() - 120) {
                    continue;
                }
                sum += saved.values[i];
                ++count;
            }
            return count > 100 ? std::optional<double>(sum / count) : std::nullopt;
        };
        const std::optional<double> power = average(appState().savedPowerVals);
        const std::optional<double> cadence = average(appState().savedCadenceVals);
        QVERIFY(power && cadence);
        // (power in whole watts, cadence to one decimal)
        QCOMPARE(first->lines()[0], QStringLiteral("Avg Pwr %1W").arg(py::fixed(*power, 0)));
        QCOMPARE(first->lines()[1], QStringLiteral("Avg Cad %1rpm").arg(py::fixed(*cadence, 1)));
        hud.restartWorkout();
        QVERIFY(hud.blockBubbles().isEmpty());
    }

    // one bubble at a time on the graph as laid out: the latest block's with the target being ridden now, else the
    // latest block's; in the middle of its block, its bottom edge on the graph's bottom
    void blockBubbleShowsOneAtATime_data() {
        QTest::addColumn<QStringList>("blocks");
        QTest::addColumn<int>("shown");
        // (riding the fourth block)
        QTest::newRow("same target as the first") << QStringList{"0.6", "1.0", "0.8", "0.6"} << 0;
        QTest::newRow("a new target") << QStringList{"0.6", "1.0", "0.8", "0.7"} << 2;
        QTest::newRow("same as the latest and an earlier one") << QStringList{"0.6", "1.0", "0.6", "0.6"} << 2;
    }

    void blockBubbleShowsOneAtATime() {
        QFETCH(QStringList, blocks);
        QFETCH(int, shown);
        QStringList lines{"T:Blocks"};
        for (const QString& power : blocks) {
            lines.append(QStringLiteral("1:90,") + power);
        }
        QVERIFY(loadWorkout(lines));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        SimulatedRider rider(3);
        hud.runSimulatedRide(3 * 90 + 30, rider);
        hud.stopTimers();
        QCOMPARE(hud.blockBubbles().size(), qsizetype(3));
        for (const double now : {10.0, 10.0 + BlockBubble::kAppearSeconds}) {
            QList<QRectF> avoid;
            BlockBubble::stepAll(hud.blockBubbles(), hud.engine().currentPower(), now, avoid);
            QCOMPARE(avoid.size(), qsizetype(1));
        }
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(hud.blockBubbles()[i]->isVisible(), i == shown);
        }
        const BlockBubble* bubble = hud.blockBubbles()[shown];
        const plot::ViewBox* vbox = hud.graph()->powerVBox();
        const QRectF area = vbox->sceneBoundingRect();
        auto sceneX = [vbox](double t) { return vbox->mapViewToScene(QPointF(t, 0)).x(); };
        const QRectF rect = bubble->mapRectToScene(bubble->boundingRect()).adjusted(1, 1, -1, -1);
        QVERIFY(area.adjusted(-0.5, -0.5, 0.5, 0.5).contains(rect));
        QVERIFY(rect.right() <= sceneX(appState().secondsPassed + BlockBubble::kMaxAheadSeconds) + 0.5);
        QVERIFY(std::abs(rect.center().x() - (sceneX(bubble->start()) + sceneX(bubble->end())) / 2) <= 1);
        QVERIFY(rect.left() >= sceneX(bubble->start()) && rect.right() <= sceneX(bubble->end()));
        QVERIFY(std::abs(rect.bottom() - area.bottom()) <= 1);
        // another block's bubble taking its place (the second block's target is no other's): it pops, and is gone
        // once it has
        const double other = hud.blockBubbles()[1]->target();
        for (const double now : {11.0, 11.0 + BlockBubble::kPopSeconds / 2}) {
            QList<QRectF> avoid;
            BlockBubble::stepAll(hud.blockBubbles(), other, now, avoid);
            QVERIFY(bubble->isVisible() && bubble->popping());
            QVERIFY(hud.blockBubbles()[1]->isVisible());
        }
        QList<QRectF> avoid;
        BlockBubble::stepAll(hud.blockBubbles(), other, 11.01 + BlockBubble::kPopSeconds, avoid);
        QVERIFY(!bubble->isVisible() && !bubble->popping());
    }

    // the graph's times edge to edge, once it scrolls too
    void graphHasNoPadding() {
        QVERIFY(loadWorkout({"T:Edges", "1:600,0.75"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.stopTimers();
        QCOMPARE(hud.graph()->vb()->xRange(), (plot::Range{0, double(appState().graphSecondsShown)}));
        hud.startWorkout();
        while (appState().secondsPassed < 500) {
            hud.processEvents(1.0);
        }
        const double start = appState().secondsPassed - (appState().graphSecondsShown - appState().graphBreakpoint);
        QVERIFY(start > 0);
        QCOMPARE(hud.graph()->vb()->xRange(), (plot::Range{start, start + appState().graphSecondsShown}));
    }

    // a long block's bubble stays in its middle, then at the graph's left edge inside the block, moving right along it,
    // and pops when it no longer fits there
    void blockBubbleMovesRightThenPops() {
        QVERIFY(loadWorkout({"T:Edge", "1:200,0.6", "1:900,1.0"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        const plot::ViewBox* vbox = hud.graph()->powerVBox();
        auto sceneX = [vbox](double t) { return vbox->mapViewToScene(QPointF(t, 0)).x(); };
        const double left = vbox->sceneBoundingRect().left();
        double now = 10;
        QRectF last;
        bool popped = false, pushed = false;
        while (!popped && appState().secondsPassed < 900) {
            tickSecond(hud, rider);
            now += 1;
            QList<QRectF> avoid;
            BlockBubble::stepAll(hud.blockBubbles(), hud.engine().currentPower(), now, avoid);
            if (hud.blockBubbles().isEmpty()) {
                continue;
            }
            const BlockBubble* bubble = hud.blockBubbles().front();
            popped = bubble->popping();
            if (!popped) {
                QVERIFY(bubble->isVisible());
                last = bubble->mapRectToScene(bubble->boundingRect()).adjusted(1, 1, -1, -1);
                QVERIFY(last.left() >= left - 0.5);
                QVERIFY(last.right() <= sceneX(bubble->end()) + 0.5);
                const double middle = (sceneX(bubble->start()) + sceneX(bubble->end())) / 2;
                if (std::abs(last.center().x() - middle) > 1) {
                    QVERIFY(std::abs(last.left() - left) <= 1);
                    pushed = true;
                }
            }
        }
        QVERIFY(popped);
        QVERIFY(pushed);
        // (from the step before it stopped fitting: within a second of the block's end by then)
        const double perSecond = sceneX(1) - sceneX(0);
        QVERIFY(sceneX(hud.blockBubbles().front()->end()) - last.right() <= 1.5 * perSecond);
    }

    // one wider than its block stays in its middle, and pops when its left edge reaches the graph's
    void shortBlockBubblePopsAtTheLeftEdge() {
        QVERIFY(loadWorkout({"T:Edge", "1:100,0.8", "1:5,0.6", "1:900,1.0"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        const plot::ViewBox* vbox = hud.graph()->powerVBox();
        auto sceneX = [vbox](double t) { return vbox->mapViewToScene(QPointF(t, 0)).x(); };
        double now = 10;
        QRectF last;
        bool popped = false;
        while (!popped && appState().secondsPassed < 900) {
            tickSecond(hud, rider);
            now += 1;
            QList<QRectF> avoid;
            BlockBubble::stepAll(hud.blockBubbles(), hud.engine().currentPower(), now, avoid);
            if (hud.blockBubbles().size() < 2) {
                continue;
            }
            const BlockBubble* bubble = hud.blockBubbles()[1];
            popped = bubble->popping();
            if (!popped) {
                QVERIFY(bubble->isVisible());
                last = bubble->mapRectToScene(bubble->boundingRect()).adjusted(1, 1, -1, -1);
                // (held back from reaching too far past the time now just after its block ends)
                const double middle = (sceneX(bubble->start()) + sceneX(bubble->end())) / 2;
                const double reach = sceneX(appState().secondsPassed + BlockBubble::kMaxAheadSeconds);
                const double center = std::min(middle, reach - bubble->placedRect().width() / 2);
                QVERIFY(std::abs(last.center().x() - center) <= 1);
            }
        }
        QVERIFY(popped);
        const double left = vbox->sceneBoundingRect().left();
        QVERIFY(last.left() >= left - 0.5);
        QVERIFY(last.left() - left <= 1.5 * (sceneX(1) - sceneX(0)));
    }

    // 10 s before the next block, a block on the graph with its target takes the bubble: only from a block of 10 s or
    // more, and not for a free ride
    void blockBubbleMovesAheadOfTheNextBlock_data() {
        QTest::addColumn<QString>("current");
        QTest::addColumn<QString>("next");
        QTest::addColumn<int>("seconds");
        QTest::addColumn<bool>("moves");
        QTest::newRow("matching block") << "0.8" << "1:60,0.6" << 60 << true;
        QTest::newRow("a block of exactly 10 s") << "0.8" << "1:60,0.6" << 10 << true;
        QTest::newRow("a shorter block") << "0.8" << "1:60,0.6" << 9 << false;
        QTest::newRow("a ramp starting at the match") << "0.8" << "2:60,0.6,0.9" << 60 << true;
        QTest::newRow("an interval starting at the match") << "0.8" << "3:2;1:30,0.6;1:30,0.7" << 60 << true;
        QTest::newRow("no block to match") << "0.8" << "1:60,0.7" << 60 << false;
        QTest::newRow("a free ride") << "0.8" << "0:60" << 60 << false;
    }

    void blockBubbleMovesAheadOfTheNextBlock() {
        QFETCH(QString, current);
        QFETCH(QString, next);
        QFETCH(int, seconds);
        QFETCH(bool, moves);
        QVERIFY(loadWorkout({"T:Ahead", "1:60,0.6", "1:60,1.0", QStringLiteral("1:%1,%2").arg(seconds).arg(current), next}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        const auto stepped = [&](double now) {
            QList<QRectF> avoid;
            BlockBubble::stepAll(hud.blockBubbles(), hud.engine().currentPower(), now, avoid, hud.upcomingBubbleTarget());
        };
        // (the second block's bubble there once it's ended)
        while (hud.engine().workoutSecondsPassed() < std::max(121.0, 120 + seconds - BubbleLead - 2)) {
            tickSecond(hud, rider);
        }
        QCOMPARE(hud.blockBubbles().size(), qsizetype(2));
        const BlockBubble* first = hud.blockBubbles()[0];
        const BlockBubble* second = hud.blockBubbles()[1];
        // (a block of 10 s or less is in its last 10 s from its start)
        const bool waits = seconds > BubbleLead;
        stepped(10);
        if (waits) {
            QVERIFY(second->isVisible() && !second->popping());
            QVERIFY(!first->isVisible());
        }
        while (hud.engine().workoutSecondsPassed() < 120 + seconds - BubbleLead + 2) {
            tickSecond(hud, rider);
        }
        stepped(11);
        if (moves) {
            QVERIFY(first->isVisible() && !first->popping());
            if (waits) {
                QVERIFY(second->isVisible() && second->popping());
            }
        } else {
            QVERIFY(second->isVisible() && !second->popping());
            QVERIFY(!first->isVisible());
        }
    }

    // a switch single leg block has no bubble and isn't matched: the bubble goes by the block before it through one,
    // doesn't move ahead of one, and still moves in its last 10 s for the block after it
    void blockBubbleSkipsSwitchLegBlocks() {
        QVERIFY(loadWorkout({"T:Switch", "1:60,0.6", "1:60,1.0", "1:20,0.6,5", "1:60,0.6"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        const auto rideTo = [&](double seconds) {
            while (hud.engine().workoutSecondsPassed() < seconds) {
                tickSecond(hud, rider);
            }
        };
        const auto stepped = [&](double now) {
            QList<QRectF> avoid;
            BlockBubble::stepAll(hud.blockBubbles(), hud.currentBubbleTarget(), now, avoid, hud.upcomingBubbleTarget());
        };
        // the last 10 s before the switch block, at the first block's target: no move
        rideTo(115);
        QVERIFY(!hud.upcomingBubbleTarget());
        // through its first seconds: the second block's bubble, though the first's has the switch block's target
        rideTo(125);
        QVERIFY(hud.engine().ridingSwitchLeg());
        QCOMPARE(hud.blockBubbles().size(), qsizetype(2));
        const BlockBubble* first = hud.blockBubbles()[0];
        const BlockBubble* second = hud.blockBubbles()[1];
        QCOMPARE(hud.currentBubbleTarget(), second->target());
        QVERIFY(!hud.upcomingBubbleTarget());
        stepped(10);
        QVERIFY(second->isVisible() && !second->popping());
        QVERIFY(!first->isVisible());
        // its last 10 s: on to the block after it's match
        rideTo(135);
        QCOMPARE(*hud.upcomingBubbleTarget(), first->target());
        stepped(11);
        QVERIFY(first->isVisible() && !first->popping());
        // it ended without a bubble of its own
        rideTo(145);
        QVERIFY(!hud.engine().ridingSwitchLeg());
        QCOMPARE(hud.blockBubbles().size(), qsizetype(2));
        QCOMPARE(hud.currentBubbleTarget(), first->target());
    }

    // as the shipped workouts have them: a 10 s free ride after a single leg free ride. It goes by that block's target
    // (none), and being 10 s long it's in its last 10 s from its start, moving to the next block's match at once
    void blockBubbleSkipsAShippedSwitchLegBlock() {
        QVERIFY(loadWorkout({"T:Switch", "1:60,0.6", "0:45,4", "0:10,5", "1:60,0.6"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        while (hud.engine().workoutSecondsPassed() < 95) {
            tickSecond(hud, rider);
        }
        QVERIFY(!hud.upcomingBubbleTarget());
        while (hud.engine().workoutSecondsPassed() < 110) {
            tickSecond(hud, rider);
        }
        QVERIFY(hud.engine().ridingSwitchLeg());
        QCOMPARE(hud.blockBubbles().size(), qsizetype(2));
        QCOMPARE(hud.currentBubbleTarget(), hud.blockBubbles()[1]->target());
        QCOMPARE(*hud.upcomingBubbleTarget(), hud.blockBubbles()[0]->target());
        while (hud.engine().workoutSecondsPassed() < 120) {
            tickSecond(hud, rider);
        }
        QVERIFY(!hud.engine().ridingSwitchLeg());
        QCOMPARE(hud.blockBubbles().size(), qsizetype(2));
    }

    // the same for an interval's switch single leg blocks
    void blockBubbleSkipsSwitchLegIntervalBlocks() {
        QVERIFY(loadWorkout({"T:Switch", "3:2;1:30,0.6;1:10,0.7,5", "1:30,0.9"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        const auto rideTo = [&](double seconds) {
            while (hud.engine().workoutSecondsPassed() < seconds) {
                tickSecond(hud, rider);
            }
        };
        rideTo(35);
        QVERIFY(hud.engine().ridingSwitchLeg());
        QCOMPARE(hud.blockBubbles().size(), qsizetype(1));
        QCOMPARE(hud.currentBubbleTarget(), hud.blockBubbles()[0]->target());
        rideTo(85);
        QVERIFY(!hud.engine().ridingSwitchLeg());
        QCOMPARE(hud.blockBubbles().size(), qsizetype(2));
    }

    // no shown bubble ever touches a star or another bubble: over a whole ride, and with stars sliding along their
    // lines
    void bubblesNeverTouch() {
        QVERIFY(loadWorkout({"T:Touch", "1:40,0.6", "1:60,1.0", "1:40,0.6", "1:90,1.3", "1:30,0.6", "1:400,0.9", "1:300,0.6"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        const auto check = [&] {
            QList<QRectF> shown;
            for (const BlockBubble* bubble : hud.blockBubbles()) {
                if (bubble->isVisible() && !bubble->popping()) {
                    shown.append(bubble->placedRect());
                }
            }
            for (const MaxBubble* bubble : hud.maxBubbles()) {
                if (bubble->isVisible() && !bubble->popping()) {
                    shown.append(bubble->placedRect());
                }
            }
            for (qsizetype i = 0; i < shown.size(); ++i) {
                for (const auto& [star, name] : hud.maxStars()) {
                    if (star->isVisible() && shown[i].intersects(star->sceneBoundingRect())) {
                        return false;
                    }
                }
                for (qsizetype j = i + 1; j < shown.size(); ++j) {
                    if (shown[i].intersects(shown[j])) {
                        return false;
                    }
                }
            }
            return true;
        };
        // (past the star clock the ride's trips start on, so they're over)
        double now = 1000;
        while (appState().secondsPassed < 800) {
            tickSecond(hud, rider);
            for (int step = 0; step < 3; ++step) {
                now += 0.2;
                hud.stepGraphMarkers(now);
                QVERIFY2(check(), qPrintable(QStringLiteral("touching at %1 s").arg(appState().secondsPassed)));
            }
        }
        // each star slid along its whole line and back
        for (const auto& [star, name] : hud.maxStars()) {
            const SavedValues& saved = name == u"power" ? appState().savedPowerVals
                                       : name == u"hr"  ? appState().savedHRVals
                                                        : appState().savedCadenceVals;
            for (const qsizetype index : {qsizetype(30), saved.times.size() - 1}) {
                star->goTo(saved.times[index], saved.values[index], now);
                for (int step = 0; step < 100; ++step) {
                    now += 0.016;
                    hud.stepGraphMarkers(now);
                    QVERIFY2(check(), qPrintable(name));
                }
            }
        }
    }

    // a star touching a max bubble pops it, and a new one comes in clear of it
    void maxBubblePopsAndMovesWhenTouched() {
        QVERIFY(loadWorkout({"T:Touch", "1:600,0.75"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        while (appState().secondsPassed < 200) {
            tickSecond(hud, rider);
        }
        double now = 1000;
        hud.stepGraphMarkers(now);
        now += MaxBubble::kAppearSeconds + 0.1;
        hud.stepGraphMarkers(now);
        MaxBubble* bubble = hud.maxBubbles().front();
        QVERIFY(bubble->isVisible() && !bubble->popping());
        const QRectF was = bubble->placedRect();
        const QList<QRectF> star{QRectF(was.center(), QSizeF(4, 4))};
        QList<QRectF> avoid = star;
        bubble->step(now, avoid);
        QVERIFY(bubble->popping());
        QCOMPARE(avoid.size(), star.size());
        now += MaxBubble::kPopSeconds + 0.01;
        avoid = star;
        bubble->step(now, avoid);
        QVERIFY(bubble->isVisible() && !bubble->popping());
        QVERIFY(!bubble->placedRect().intersects(star.front()));
        QVERIFY(bubble->placedRect() != was);
    }

    // the graph's bubbles share one font at any scale, readable on a small screen, and those already shown change with
    // the scale without popping; the text's line stays clear of a max bubble's rim
    void bubbleTextFollowsTheScaleInOneReadableSize() {
        QVERIFY(loadWorkout({"T:Scale", "1:200,0.6", "1:400,0.6"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        while (appState().secondsPassed < 280) {
            tickSecond(hud, rider);
        }
        double now = 1000;
        hud.stepGraphMarkers(now);
        now += MaxBubble::kAppearSeconds + 0.1;
        hud.stepGraphMarkers(now);
        MaxBubble* maxBubble = hud.maxBubbles().front();
        BlockBubble* blockBubble = BlockBubble::pick(hud.blockBubbles(), hud.engine().currentPower());
        QVERIFY(maxBubble->isVisible() && blockBubble && blockBubble->isVisible());
        for (const auto& [scale, pixels] : {std::pair{1.0, 10}, std::pair{0.667, MaxBubble::kMinTextPixels},
                                            std::pair{0.5, MaxBubble::kMinTextPixels}, std::pair{0.3, MaxBubble::kMinTextPixels}}) {
            theme::setUiScale(scale);
            now += 0.1;
            hud.stepGraphMarkers(now);
            const QString where = QStringLiteral("at %1").arg(scale);
            QVERIFY2(!maxBubble->popping(), qPrintable(where));
            QVERIFY2(maxBubble->font() == MaxBubble::textFont(), qPrintable(where));
            QCOMPARE(MaxBubble::textFont().pixelSize(), pixels);
            QVERIFY2(blockBubble->font() == MaxBubble::textFont(), qPrintable(where));
            const double line = QFontMetricsF(maxBubble->font()).height();
            QVERIFY2(maxBubble->boundingRect().height() - 2 >= line + 2 * MaxBubble::kTextMargin - 0.001, qPrintable(where));
        }
        // (bigger screens: on up with the scale)
        theme::setUiScale(2.0);
        QCOMPARE(MaxBubble::textFont().pixelSize(), 21);
        theme::setUiScale(1.0);
    }

    // a star touching a block bubble pops it, and a new one comes in clear of it, still in its block
    void blockBubblePopsAndMovesWhenTouched() {
        QVERIFY(loadWorkout({"T:Touch", "1:200,0.6", "1:400,0.6"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        while (hud.blockBubbles().isEmpty()) {
            tickSecond(hud, rider);
        }
        const plot::ViewBox* vbox = hud.graph()->powerVBox();
        auto sceneX = [vbox](double t) { return vbox->mapViewToScene(QPointF(t, 0)).x(); };
        double now = 1000;
        QList<QRectF> avoid;
        const double target = hud.engine().currentPower();
        BlockBubble::stepAll(hud.blockBubbles(), target, now, avoid);
        const BlockBubble* bubble = hud.blockBubbles().front();
        QVERIFY(bubble->isVisible());
        const QRectF was = bubble->placedRect();
        const QList<QRectF> star{QRectF(was.center(), QSizeF(4, 4))};
        now += 1;
        avoid = star;
        BlockBubble::stepAll(hud.blockBubbles(), target, now, avoid);
        QVERIFY(bubble->popping());
        // (held while it pops)
        now += BlockBubble::kPopSeconds / 2;
        avoid = star;
        BlockBubble::stepAll(hud.blockBubbles(), target, now, avoid);
        QVERIFY(bubble->popping());
        now += BlockBubble::kPopSeconds;
        avoid = star;
        BlockBubble::stepAll(hud.blockBubbles(), target, now, avoid);
        avoid = star;
        BlockBubble::stepAll(hud.blockBubbles(), target, now, avoid);
        QVERIFY(bubble->isVisible() && !bubble->popping());
        const QRectF rect = bubble->placedRect();
        QVERIFY(!rect.intersects(star.front()));
        QVERIFY(rect.left() >= sceneX(bubble->start()) && rect.right() <= sceneX(bubble->end()));
        // (stays there, scrolling with its block)
        now += 1;
        avoid.clear();
        BlockBubble::stepAll(hud.blockBubbles(), target, now, avoid);
        QVERIFY(!bubble->popping());
        QCOMPARE(bubble->placedRect(), rect);
    }

    // a max star moves on when its left edge reaches the graph's; a max bubble on its left pops when its own does, and
    // a new one comes in on the star's right
    void maxStarAndBubbleLeaveAtTheLeftEdge() {
        QVERIFY(loadWorkout({"T:Edge", "1:1200,0.75"}));
        HudScreen hud(bridge_.get(), nullptr, [] { return 1000.0; });
        hud.setAttribute(Qt::WA_DontShowOnScreen);
        hud.resize(1920, 1116);
        hud.show();
        QCoreApplication::processEvents();
        hud.stopTimers();
        SimulatedRider rider(3);
        hud.startWorkout();
        while (appState().secondsPassed < 300) {
            tickSecond(hud, rider);
        }
        // a max that stays the max
        SavedValues& power = appState().savedPowerVals;
        power.values.back() = 5000;
        const double spike = power.times.back();
        tickSecond(hud, rider);

        MaxBubble* bubble = nullptr;
        for (MaxBubble* candidate : hud.maxBubbles()) {
            if (candidate->star()->y == 5000.0) {
                bubble = candidate;
            }
        }
        QVERIFY(bubble);
        MaxStar* star = bubble->star();
        QCOMPARE(*star->x, spike);
        const plot::ViewBox* vbox = star->vbox();
        auto sceneX = [vbox](double t) { return vbox->mapViewToScene(QPointF(t, 0)).x(); };
        const double perSecond = sceneX(1) - sceneX(0);
        const double left = vbox->sceneBoundingRect().left();
        double now = 1000;
        // (the right side taken, to put it on the left)
        const auto stepOnLeft = [&] {
            QList<QRectF> avoid{QRectF(sceneX(spike), -5000, 10000, 10000)};
            bubble->step(now, avoid);
        };
        stepOnLeft();
        now += MaxBubble::kAppearSeconds + 1;
        stepOnLeft();
        QVERIFY(bubble->isVisible() && !bubble->popping());
        QVERIFY(bubble->scenePos().x() < sceneX(spike));

        while (!bubble->popping()) {
            QVERIFY(appState().secondsPassed < 720);
            tickSecond(hud, rider);
            now += 1;
            stepOnLeft();
        }
        // (its max still on the graph, and the star's still over it)
        QCOMPARE(*star->x, spike);
        QVERIFY(sceneX(spike) - star->halfWidth() > left);
        QList<QRectF> avoid;
        now += MaxBubble::kPopSeconds + 0.01;
        bubble->step(now, avoid);
        QVERIFY(!bubble->popping());
        QVERIFY(bubble->scenePos().x() > sceneX(spike));

        while (star->x == spike) {
            QVERIFY(appState().secondsPassed < 780);
            // (still clear of the edge, to within a tick)
            QVERIFY(sceneX(spike) - star->halfWidth() > left - perSecond);
            tickSecond(hud, rider);
        }
        QVERIFY(sceneX(spike) - star->halfWidth() <= left + perSecond);
        QVERIFY(sceneX(*star->x) - star->halfWidth() > left);
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
