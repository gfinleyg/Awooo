// The synopsis (src/ui/screens/SynopsisScreen) built offscreen from the session's rides, ridden by the simulated
// rider on the workout screen (as LayoutViewer builds it) or made up here: the stats and grade, the max markers and
// their stars riding to a zoomed in selection's max, zooming and the time boxes, clicks passed down through the graph's
// views (a curve, a label, a graded block), the grade popups and Left/Right, Block Grades, an FTP test's result and
// Apply FTP, Set Max HR, the rides' strip, and best power over dropouts (tests/test_screens.py's synopsis half).

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTimeEdit>
#include <QtTest>

#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "AppState.hpp"
#include "AwoParser.hpp"
#include "FakeAnt.hpp"
#include "GradeWidgets.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"
#include "MaxStar.hpp"
#include "Performance.hpp"
#include "PlotItems.hpp"
#include "PowerSpiral.hpp"
#include "PyCompat.hpp"
#include "SavedSettings.hpp"
#include "Session.hpp"
#include "SetupScreen.hpp"
#include "SimulatedRider.hpp"
#include "SynopsisScreen.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutData.hpp"
#include "WorkoutGraph.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

using namespace awo;

namespace {

const QDateTime kStart(QDate(2026, 10, 26), QTime(8, 0, 0));

// a ride's state cleared, as going back to setup does
void resetRide() {
    AppState& state = appState();
    state.secondsPassed = 0;
    state.workoutPowerRequestsSeconds.clear();
    state.workoutPowerRequestsWatts.clear();
    state.workoutPowerRequestsSysTime.clear();
    state.savedPowerVals = {};
    state.savedHRVals = {};
    state.savedCadenceVals = {};
    state.rideStart.reset();
    state.rideRecorded = false;
    state.blockGrades.clear();
    state.ftpTestResult.reset();
    workoutData().clear();
}

// The workout (or free ride: lines empty) ridden by the simulated rider a second a tick, its readings recorded as
// LayoutViewer records them, joined to the session
void rideIntoSession(const QStringList& lines, double seconds, ant::AntBridge* bridge, int seed = 11) {
    resetRide();
    AppState& state = appState();
    state.workoutMode = !lines.isEmpty();
    state.cadenceMode = false;
    state.ftpTestMode.clear();
    state.antHRM = true;
    state.antSpeedCadence = true;
    state.ftpRanges = zones::powerRanges();
    if (state.workoutMode) {
        AwoOptions options;
        options.ftp = state.ftp;
        const AwoResult result = parseAwoLines(lines, options);
        QVERIFY(result.ok());
        state.workoutList = result.workout.entries;
        state.workoutMessages = result.workout.messages;
        state.workoutTitle = result.workout.title;
        state.workoutDurationSeconds = result.workout.seconds;
        state.workoutFilename = state.workoutFilenameShort = QStringLiteral("test.awo");
    }
    const QDateTime start = kStart.addSecs(static_cast<qint64>(session::seconds()) + 600);
    state.startTimestamp = start;
    session::beginRide();
    const double startSeconds = start.toMSecsSinceEpoch() / 1000.0;
    double revolutions = 0;
    const auto record = [&](double t, const RiderReading& reading) {
        const double ts = startSeconds + t;
        WorkoutData& data = workoutData();
        data.power.appendAt(ts, {static_cast<double>(reading.power)});
        data.heartRate.appendAt(ts, {static_cast<double>(reading.heartRate)});
        data.fallbackCadence.appendAt(ts, {py::round(reading.cadence)});
        const double revsPerSecond = reading.speedMph / 2.23694 / state.wheelCircumference;
        revolutions += revsPerSecond;
        const double lastRevTime = t - (revolutions - std::trunc(revolutions)) / std::max(revsPerSecond, 0.01);
        data.speed.appendAt(ts, {static_cast<double>(static_cast<long long>(revolutions) & 0xFFFF),
                                 static_cast<double>(static_cast<long long>(lastRevTime * 1024) & 0xFFFF)});
    };
    std::optional<Ring> ring;
    {
        HudScreen hud(bridge, nullptr, [] { return 1000.0; });
        SimulatedRider rider(seed);
        hud.runSimulatedRide(seconds, rider, record);
        hud.stopTimers();
        if (state.workoutMode) {
            hud.endGrading();
            ring = hud.spiral()->ridden();
        }
    }
    state.workoutPowerRequestsSysTime.clear();
    // (the times have one more than the targets: the first's start)
    for (const double s : state.workoutPowerRequestsSeconds) {
        state.workoutPowerRequestsSysTime.append(start.addMSecs(static_cast<qint64>(py::round(s * 1000))));
    }
    session::addCurrentRide(ring);
}

// 260 s: ridden to its end, the ride is 262 s
const QStringList kWorkout = {"T:Synopsis Test", "1:60,0.6", "1:60,1.0", "3:2;1:20,1.2;0:20", "2:60,0.5,0.9"};

// two ranges the same but for rounding (linked views are lined up through their screen geometry)
bool sameRange(const plot::Range& a, const plot::Range& b) {
    return std::abs(a.min - b.min) < 1e-6 && std::abs(a.max - b.max) < 1e-6;
}

}  // namespace

class TestSynopsis : public QObject {
    Q_OBJECT

private:
    // the synopsis of the session as it is, shown at a full screen's size
    SynopsisScreen* synopsis() {
        synopsis_ = std::make_unique<SynopsisScreen>(bridge_.get(), setup_.get(), [this] { return clock_; });
        synopsis_->resize(1900, 1060);
        synopsis_->show();
        QApplication::processEvents();
        synopsis_->updateSelection();
        return synopsis_.get();
    }

    // a click on the graph at a scene point, as the mouse gives it
    void clickAt(const QPointF& scenePoint) {
        WorkoutGraph* graph = synopsis_->graph();
        const QPoint at = graph->mapFromScene(scenePoint);
        QTest::mouseClick(graph->viewport(), Qt::LeftButton, {}, at);
        QApplication::processEvents();
    }

private slots:
    void initTestCase() { keys::install(qApp); }

    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        theme::current().reset();
        theme::setUiScale(1.0);
        Performance::current().reset();
        Units::current().reset();
        appState() = AppState();
        AppState& state = appState();
        state.ftp = 250;
        state.maxHr = 185;
        state.wheelCircumference = 2.105;
        session::clear();
        fake_ = std::make_unique<awotest::FakeAnt>();
        bridge_ = fake_->makeBridge();
        setup_ = std::make_unique<SetupScreen>(bridge_.get());
        clock_ = 1000.0;
    }

    void cleanup() {
        synopsis_.reset();
        setup_.reset();
        bridge_.reset();
        fake_.reset();
        session::clear();
        dir_.reset();
    }

    // ---- What it shows ----

    void statsGradeAndMaxes() {
        rideIntoSession(kWorkout, 301, bridge_.get());
        SynopsisScreen* screen = synopsis();
        const QStringList lines = screen->statsLabel()->text().split(u'\n');
        QVERIFY(lines[0].startsWith(QStringLiteral("Distance: ")) && lines[0].endsWith(QStringLiteral(" mi")));
        QVERIFY(lines.contains(QStringLiteral("Total Time: ") + minimizedDuration(session::rides().front().secondsPassed)));
        QVERIFY(lines.contains(QStringLiteral("Best sustained power values:")));
        QVERIFY(std::any_of(lines.begin(), lines.end(), [](const QString& l) { return l.startsWith(QStringLiteral("Average Power: ")); }));
        // the best powers up to the longest the ride spans (2 m)
        QCOMPARE(screen->bestPowers().size(), qsizetype(4));
        QVERIFY(lines.back().endsWith(QStringLiteral(" Watts over 2m")));
        QVERIFY(screen->gradeText()->text().startsWith(QStringLiteral("Workout Grade\n")));
        // a marker on each sensor's max: power, heart rate, cadence (the power meter's), speed
        QCOMPARE(screen->maxMarkers().size(), qsizetype(4));
        const auto& power = screen->maxMarkers()[0];
        QCOMPARE(power.vbox, screen->graph()->powerVBox());
        QCOMPARE(power.fullY, *std::max_element(screen->powerValues().second.begin(), screen->powerValues().second.end()));
        QVERIFY(power.label->text().startsWith(QStringLiteral("Max Power: ")));
        for (const auto& marker : screen->maxMarkers()) {
            QVERIFY(marker.shown && marker.star->onGraph && marker.label->isVisible());
        }
        // Set Max HR: the ride's own max, a heart rate the setup screen takes
        QVERIFY(screen->setMaxHrButton()->isEnabled());
        screen->setMaxHrButton()->click();
        QCOMPARE(appState().maxHr, static_cast<int>(py::round(screen->maxMarkers()[1].fullY)));
        QVERIFY(!screen->setMaxHrButton()->isEnabled());
        QCOMPARE(settings::load().value(QStringLiteral("maxHr")).toInt(), appState().maxHr);
        // no FTP test: no Apply FTP
        QVERIFY(screen->applyFtpButton() == nullptr && screen->ftpTestText() == nullptr);
    }

    // (test_max_stars_ride_to_the_selection_max) zooming away from the power max sends its star along the curve to
    // the selection's max, where it settles above it
    void starRidesToTheSelectionMax() {
        rideIntoSession(kWorkout, 301, bridge_.get());
        SynopsisScreen* screen = synopsis();
        const auto& power = screen->maxMarkers()[0];
        // a minute on the far side of the ride from the max
        const double total = screen->totalSeconds();
        const double start = power.fullX > total / 2 ? 0 : total - 60;
        screen->graph()->vb()->setXRange(start, start + 60, 0.0);
        screen->updateSelection();
        MaxStar* star = power.star;
        QVERIFY(power.x != power.fullX && *star->x == power.x);
        QVERIFY(power.label->text().startsWith(QStringLiteral("(Selection) Max Power: ")));
        QVERIFY(star->progress(clock_) < 1.0);
        // there: resting above the marker
        clock_ += MaxStar::kTripMax + 0.1;
        for (int i = 0; i < 40; ++i) {
            screen->animateStars();
        }
        QCOMPARE(star->progress(clock_), 1.0);
        const QPointF pt = power.vbox->mapViewToScene(QPointF(power.x, power.y));
        QVERIFY(std::abs(star->pos().x() - pt.x()) < 0.01);
        QVERIFY(std::abs(star->pos().y() - (pt.y() - star->restOffset())) < 0.01);
        QVERIFY(star->isVisible());
        // the whole ride again: back to the session's max, the label without "(Selection)"
        screen->graph()->vb()->setXRange(0, total, 0.0);
        screen->updateSelection();
        QCOMPARE(power.x, power.fullX);
        QVERIFY(power.label->text().startsWith(QStringLiteral("Max Power: ")));
    }

    void zoomingAndTheTimeBoxes() {
        rideIntoSession(kWorkout, 301, bridge_.get());
        SynopsisScreen* screen = synopsis();
        QCOMPARE(screen->startTimeEdit()->time(), QTime(0, 0));
        QCOMPARE(screen->endTimeEdit()->time(), QTime(0, 0).addSecs(static_cast<int>(py::round(screen->totalSeconds()))));
        // zoomed in: the stats are the selection's
        screen->graph()->vb()->setXRange(60, 180, 0.0);
        screen->updateSelection();
        QCOMPARE(screen->startTimeEdit()->time(), QTime(0, 1, 0));
        QCOMPARE(screen->endTimeEdit()->time(), QTime(0, 3, 0));
        const QStringList lines = screen->statsLabel()->text().split(u'\n');
        QCOMPARE(lines[0], QStringLiteral("(Selection)"));
        QVERIFY(lines.contains(QStringLiteral("Total Time: 2m")));
        // a selection has only the best powers it spans (2 m)
        QVERIFY(lines.back().endsWith(QStringLiteral(" Watts over 2m")));
        // a From less than the narrowest zoom before To moves To along
        screen->startTimeEdit()->setTime(QTime(0, 2, 50));
        QVERIFY(std::abs(screen->viewRange().min - 170) < 1e-6 && std::abs(screen->viewRange().span() - 30) < 1e-6);
        // never narrower than that, nor past the session's ends
        screen->graph()->vb()->setXRange(100, 105, 0.0);
        QVERIFY(screen->viewRange().span() >= SynopsisScreen::kMinZoomSeconds - 1e-6);
        screen->graph()->vb()->setXRange(-100, 1000, 0.0);
        QVERIFY(screen->viewRange().min >= -1e-6 && screen->viewRange().max <= screen->totalSeconds() + 1e-6);
        // every view follows the graph's
        QVERIFY(sameRange(screen->graph()->powerVBox()->xRange(), screen->viewRange()));
        QVERIFY(sameRange(screen->graph()->hrCadenceVBox()->xRange(), screen->viewRange()));
    }

    // dragging the graph pans all three views (the one on top takes the drag, the others follow it)
    void dragPansEveryView() {
        rideIntoSession(kWorkout, 301, bridge_.get());
        SynopsisScreen* screen = synopsis();
        screen->graph()->vb()->setXRange(60, 120, 0.0);
        QApplication::processEvents();
        WorkoutGraph* graph = screen->graph();
        const QPoint from = graph->mapFromScene(graph->vb()->sceneBoundingRect().center());
        QTest::mousePress(graph->viewport(), Qt::LeftButton, {}, from);
        for (int step = 1; step <= 10; ++step) {
            QMouseEvent move(QEvent::MouseMove, QPointF(from + QPoint(step * 20, 0)),
                             graph->viewport()->mapToGlobal(QPointF(from + QPoint(step * 20, 0))), Qt::NoButton,
                             Qt::LeftButton, {});
            QApplication::sendEvent(graph->viewport(), &move);
        }
        QTest::mouseRelease(graph->viewport(), Qt::LeftButton, {}, from + QPoint(200, 0));
        QVERIFY(screen->viewRange().min < 60 - 1);
        QVERIFY(std::abs(screen->viewRange().span() - 60) < 1e-6);
        // the two over it the same, lined up with it on screen (pyqtgraph's linking: within a pixel's time)
        QVERIFY(sameRange(graph->powerVBox()->xRange(), graph->hrCadenceVBox()->xRange()));
        QVERIFY(std::abs(graph->powerVBox()->xRange().min - screen->viewRange().min) < 0.1);
        QVERIFY(std::abs(graph->powerVBox()->xRange().max - screen->viewRange().max) < 0.1);
    }

    // clicks go down through the views to what's under them: a curve, a label, a graded block; one on nothing clears
    void clicksReachWhatsUnderThem() {
        rideIntoSession(kWorkout, 301, bridge_.get());
        SynopsisScreen* screen = synopsis();
        plot::CurveItem* power = screen->sensorCurves()[0];
        // on the power line (under the heart rate/cadence view)
        const qsizetype middle = power->xData().size() / 2;
        clickAt(screen->graph()->powerVBox()->mapViewToScene(QPointF(power->xData()[middle], power->yData()[middle])));
        QCOMPARE(screen->selectedCurve(), power);
        // on nothing: cleared
        clickAt(screen->graph()->powerVBox()->mapViewToScene(QPointF(screen->totalSeconds() * 0.5, screen->graph()->powerVBox()->yRange().max * 0.97)));
        QVERIFY(screen->selectedCurve() == nullptr);
        // a max label highlights its curve
        const auto& hr = screen->maxMarkers()[1];
        clickAt(hr.label->sceneBoundingRect().center());
        QCOMPARE(screen->selectedCurve(), screen->sensorCurves()[1]);
        // again: cleared
        clickAt(hr.label->sceneBoundingRect().center());
        QVERIFY(screen->selectedCurve() == nullptr);
        // a graded block, low down away from the lines: its grade opens; again: closed
        GradeRegion* region = screen->gradeRegions().front();
        const QPointF inside = region->vbox()->mapViewToScene(QPointF(region->rect().center().x(), 3));
        clickAt(inside);
        QCOMPARE(screen->gradePopupRegion(), region);
        QVERIFY(screen->gradePopup() && screen->gradePopup()->isVisible());
        clickAt(inside);
        QVERIFY(screen->gradePopup() == nullptr);
    }

    void gradePopupsStepAndClose() {
        rideIntoSession(kWorkout, 301, bridge_.get());
        SynopsisScreen* screen = synopsis();
        // one region a graded block: 2 blocks, 2 reps of 2, the ramp
        QCOMPARE(screen->gradeRegions().size(), qsizetype(7));
        screen->stepGrade(1);
        GradeRegion* first = screen->gradePopupRegion();
        QVERIFY(first && first->rect().left() <= screen->gradeRegions()[1]->rect().left());
        screen->stepGrade(1);
        screen->stepGrade(1);
        // the third is an interval's first rep: named so
        QVERIFY(screen->gradePopup()->findChildren<QLabel*>().front()->text().contains(QStringLiteral("rep 1 of 2")) ||
                std::any_of(screen->gradePopup()->findChildren<QLabel*>().begin(), screen->gradePopup()->findChildren<QLabel*>().end(),
                            [](QLabel* l) { return l->text().contains(QStringLiteral("rep 1 of 2")); }));
        screen->stepGrade(-1);
        screen->stepGrade(-1);
        QCOMPARE(screen->gradePopupRegion(), first);
        // (no further back)
        screen->stepGrade(-1);
        QCOMPARE(screen->gradePopupRegion(), first);
        // zoomed in elsewhere: Left/Right scrolls the graph to the block
        screen->graph()->vb()->setXRange(250, 280, 0.0);
        screen->stepGrade(1);
        const QRectF rect = screen->gradePopupRegion()->rect();
        QVERIFY(rect.left() >= screen->viewRange().min - 1e-6 || rect.right() <= screen->viewRange().max + 1e-6);
        // the details: the average against the target, and the graded time
        const Ride& ride = session::rides().front();
        const QString html = screen->gradeDetailsHtml(ride.blockGrades.items().front().second, ride);
        QVERIFY(html.contains(QStringLiteral("Average vs target")) && html.contains(QStringLiteral("Graded time")));
        // Esc closes it, then has nothing to close
        QVERIFY(screen->escapePressed());
        QVERIFY(screen->gradePopup() == nullptr);
        QVERIFY(!screen->escapePressed());
    }

    void blockGradesSheet() {
        rideIntoSession(kWorkout, 301, bridge_.get());
        SynopsisScreen* screen = synopsis();
        screen->blockGradesButton()->click();
        QVERIFY(screen->blockGradesShown());
        QVERIFY(!screen->blockGradeList()->rows().isEmpty());
        // Esc closes it first
        QVERIFY(screen->escapePressed());
        QVERIFY(!screen->blockGradesShown());
    }

    // the session's rides one after the other: a heading for each along the top, which zooms to it, and again back
    // out; each ride's best power its own (a stretch doesn't run on into the next ride)
    void twoRidesStripAndBestPower() {
        rideIntoSession(kWorkout, 301, bridge_.get());
        rideIntoSession({}, 200, bridge_.get(), 5);
        SynopsisScreen* screen = synopsis();
        QVERIFY(screen->rideStrip() != nullptr);
        const QList<BandStrip::Band>& bands = screen->rideStrip()->bands();
        QCOMPARE(bands.size(), qsizetype(2));
        QCOMPARE(bands[0].title, QStringLiteral("Workout: Synopsis Test"));
        QCOMPARE(bands[1].title, QStringLiteral("Free Ride"));
        QVERIFY(std::abs(screen->totalSeconds() - (bands[0].end - bands[0].start + bands[1].end - bands[1].start)) < 1e-6);
        // clicking the free ride's heading zooms to it, clicking it again zooms back out
        const auto [index, left, right] = screen->rideStrip()->spans()[1];
        const QPointF onIt = screen->rideStrip()->mapToScene(QPointF((left + right) / 2, screen->rideStrip()->height() / 2));
        clickAt(onIt);
        QVERIFY(std::abs(screen->viewRange().min - bands[1].start) < 0.5 && std::abs(screen->viewRange().max - bands[1].end) < 0.5);
        QCOMPARE(screen->rideStrip()->spans().size(), qsizetype(1));
        clickAt(screen->rideStrip()->mapToScene(QPointF(screen->rideStrip()->boundingRect().center())));
        QVERIFY(screen->viewRange().min < 0.5 && std::abs(screen->viewRange().max - screen->totalSeconds()) < 0.5);
        // "Session Grade" only with two graded rides: here one
        QVERIFY(screen->gradeText()->text().startsWith(QStringLiteral("Workout Grade")));
        // the best 2 minutes is the workout's, the 5 minutes spans neither ride (a stretch ends with its ride)
        QCOMPARE(screen->bestPowers().size(), qsizetype(4));
    }

    // a 20m FTP test's result heads the stats; Apply FTP sets it on the setup screen
    void ftpTestResultAndApplyFtp() {
        Ride ride;
        ride.kind = RideKind::FtpTest;
        ride.ftpTestMode = QStringLiteral("20m");
        ride.ftpTestResult = ftptest::Result{};
        ride.ftpTestResult->average = 260;
        ride.ftpTestResult->completed = true;
        ride.ftp = 230;
        ride.seconds = ride.secondsPassed = 120;
        ride.startTimestamp = kStart;
        const double start = kStart.toMSecsSinceEpoch() / 1000.0;
        for (int t = 1; t <= 120; ++t) {
            ride.data.power.appendAt(start + t, {250});
        }
        ride.powerRanges = ride.targetRanges = zones::powerRanges();
        appState().sessionRides.append(ride);
        SynopsisScreen* screen = synopsis();
        // the 20m test: 95% of the effort's average
        QCOMPARE(screen->newFtp(), std::optional<int>(ftptest::ftpFromAverage(QStringLiteral("20m"), 260)));
        QVERIFY(screen->ftpTestText()->text().startsWith(QStringLiteral("Previous FTP: 230\nNew FTP: %1").arg(*screen->newFtp())));
        QVERIFY(screen->ftpTestText()->text().endsWith(QStringLiteral("Difference: %1").arg(*screen->newFtp() - 230, 0)) ||
                screen->ftpTestText()->text().contains(QStringLiteral("Difference: +")));
        QVERIFY(screen->applyFtpButton()->isEnabled());
        screen->applyFtpButton()->click();
        QCOMPARE(appState().ftp, *screen->newFtp());
        QVERIFY(!screen->applyFtpButton()->isEnabled());
        // no graded ride: no grade, no Block Grades
        QVERIFY(screen->gradeText() == nullptr && screen->blockGradesButton() == nullptr);
    }

    // an effort not ridden to the end, or no power: no new FTP, and why
    void ftpTestWithoutAResult() {
        Ride ride;
        ride.kind = RideKind::FtpTest;
        ride.ftpTestMode = QStringLiteral("20m");
        ride.ftpTestResult = ftptest::Result{};
        ride.ftpTestResult->average = 260;
        ride.ftpTestResult->completed = false;
        ride.ftp = 0;
        ride.seconds = ride.secondsPassed = 60;
        ride.startTimestamp = kStart;
        appState().sessionRides.append(ride);
        SynopsisScreen* screen = synopsis();
        QVERIFY(!screen->newFtp());
        QCOMPARE(screen->ftpTestText()->text(), QStringLiteral("Previous FTP: not set\nNew FTP: --\nEffort not completed"));
        QVERIFY(!screen->applyFtpButton()->isEnabled());
        // no heart rate recorded: nothing for Set Max HR
        QVERIFY(!screen->setMaxHrButton()->isEnabled());
    }

    // best power over unbroken readings: a dropout ends a stretch, except in a 20m test's effort, where the power
    // stays at the stretch's average through it
    void bestPowerOverDropouts() {
        for (const bool ftpTest : {false, true}) {
            session::clear();
            Ride ride;
            ride.kind = ftpTest ? RideKind::FtpTest : RideKind::Free;
            ride.ftpTestMode = ftpTest ? QStringLiteral("20m") : QString();
            ride.seconds = ride.secondsPassed = 80;
            ride.startTimestamp = kStart;
            const double start = kStart.toMSecsSinceEpoch() / 1000.0;
            // 30 s at 200 W, 10 s silent, 30 s at 200 W
            for (int t = 1; t <= 70; ++t) {
                if (t <= 30 || t > 40) {
                    ride.data.power.appendAt(start + t, {200});
                }
            }
            appState().sessionRides.append(ride);
            SynopsisScreen* screen = synopsis();
            // 10 s and 30 s (29 s between the first and last reading of a stretch is under 30 s: 10 s only), and
            // 1 m only across the dropout
            QCOMPARE(screen->bestPowers().size(), ftpTest ? qsizetype(3) : qsizetype(1));
            synopsis_.reset();
        }
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
    std::unique_ptr<awotest::FakeAnt> fake_;
    std::unique_ptr<ant::AntBridge> bridge_;
    std::unique_ptr<SetupScreen> setup_;
    std::unique_ptr<SynopsisScreen> synopsis_;
    double clock_ = 1000.0;
};

QTEST_MAIN(TestSynopsis)
#include "tst_synopsis.moc"
