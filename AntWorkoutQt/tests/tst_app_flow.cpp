// The whole app on a fake ANT stick (tests/test_app_flow.py), as hwtests' SimulANT+ runs drive it but with the
// sensors' pages made up here: MainWindow and its bridge thread, the setup screen's auto-select and connect, a ride
// on the workout screen (its clock stepped a second at a time), grading, the synopsis, back to setup and the exit.
// It doesn't replace the SimulANT+ tests: those check the radio side.

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QtTest>

#include "AnalogGauge.hpp"
#include "AntBridge.hpp"
#include "AppState.hpp"
#include "ConfirmEndWorkoutWindow.hpp"
#include "DeviceDialogs.hpp"
#include "Dialogs.hpp"
#include "FakeAnt.hpp"
#include "GradeWidgets.hpp"
#include "HudScreen.hpp"
#include "KeyShortcuts.hpp"
#include "MainWindow.hpp"
#include "Performance.hpp"
#include "PowerSpiral.hpp"
#include "Session.hpp"
#include "SetupScreen.hpp"
#include "SynopsisScreen.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutData.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"
#include "ant_pages.hpp"

using namespace awo;
using ant::Role;

namespace {

constexpr int kFtp = 200;
const QString kWorkout = QStringLiteral("T:App Flow\n1:10,0.75\n2:10,0.5,1.0\n3:2;1:5,1.2;0:5\n1:10,1.6\n");
// same power ramps and one block intervals, which the setup screen writes out as the blocks they amount to
const QString kConvertedWorkout = QStringLiteral(
    "T:Converted\n2:20,0.6,0.6\n3:3;2:20,0.7,0.7|Up|5|10\n3:4;1:10,1.1|Go|3\n3:2;0:10\n3:2;2:10,0.7,0.7;0:10\n");

template <typename T>
T* topLevel() {
    T* found = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* match = qobject_cast<T*>(widget); match && match->isVisible()) {
            found = match;
        }
    }
    return found;
}

}  // namespace

class TestAppFlow : public QObject {
    Q_OBJECT

private:
    // made up sensor pages, a round a call
    void round() {
        ++i_;
        const int i = i_;
        if (trainer_) {
            ant_->stick->deliver(awotest::trainerPage(300, watts_ + i % 3, 88, 0, channel(17), 5));
        } else {
            ant_->stick->deliver(awotest::powerPage(100, watts_ + i % 3, 90, i & 0xFF, channel(11), 5));
            ant_->stick->deliver(awotest::combinedPage(400, (i * 256) & 0xFFFF, (i * 3 / 8) & 0xFFFF, (i * 256) & 0xFFFF,
                                                       i & 0xFFFF, channel(121), 1));
        }
        ant_->stick->deliver(awotest::hrPage(200, 130 + i % 5, i & 0xFF, (i * 256) & 0xFFFF, channel(120), 1));
    }

    int channel(int deviceType) const { return channels_.value(deviceType, 0); }

    // the dedicated channels the bridge opened, for the pages to come in on
    void learnChannels() {
        channels_.clear();
        for (const auto& [channel, identity] : window_->bridge()->manager().channel_devices()) {
            channels_.insert(std::get<0>(identity), channel);
        }
    }

    // heard while scanning, until the setup screen has picked them and the bridge is on their own channels
    void connectSensors() {
        ant::AntBridge* bridge = window_->bridge();
        QTRY_VERIFY_WITH_TIMEOUT(bridge->isInitialized(), 5000);
        // (the scan has listened long enough once the clock's moved on)
        QTRY_VERIFY_WITH_TIMEOUT((round(), ant_->clock.advance(1), bridge->isReady()), 15000);
        learnChannels();
    }

    void loadWorkout(const QString& text = kWorkout) {
        const QString path = QDir::current().absoluteFilePath(QStringLiteral("app_flow.awo"));
        awotest::writeFile(path, text.toUtf8());
        SetupScreen* setup = window_->setupScreen();
        QVERIFY(setup->loadWorkoutFile(path));
        appState().workoutFilename = path;
        appState().workoutFilenameShort = QStringLiteral("app_flow.awo");
        setup->checkEnableStartButton();
    }

    // the workout screen's clock stepped a second at a time, a round of pages each second
    HudScreen* ride(int seconds) {
        HudScreen* hud = window_->hudScreen();
        hud->stopTimers();
        for (int s = 0; s < seconds; ++s) {
            round();
            QApplication::processEvents();
            hud->processEvents(1.0);
            if (hud->engine().completeShown()) {
                break;
            }
        }
        return hud;
    }

    SynopsisScreen* toSynopsis() {
        window_->escapePressed();
        ConfirmEndWorkoutWindow* prompt = window_->hudScreen()->endPrompt();
        if (prompt == nullptr) {
            return nullptr;
        }
        prompt->synopsisButton()->click();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(window_->synopsisScreen() && window_->synopsisScreen()->isVisible(), 5000); }();
        return window_->synopsisScreen();
    }

    SetupScreen* backToSetup() {
        ant::AntBridge* bridge = window_->bridge();
        SetupScreen* setup = window_->setupScreen();
        window_->synopsisScreen()->restartButton()->click();
        [&] { QTRY_VERIFY_WITH_TIMEOUT(bridge->manager().mode() == antnative::ChannelMode::Scan && setup->isVisible(), 5000); }();
        channels_.clear();
        [&] { QTRY_VERIFY_WITH_TIMEOUT((round(), ant_->clock.advance(1), bridge->isReady()), 15000); }();
        learnChannels();
        return setup;
    }

    void startWorkout() {
        SetupScreen* setup = window_->setupScreen();
        QTRY_VERIFY_WITH_TIMEOUT(setup->startButton()->isEnabled(), 5000);
        setup->startButton()->click();
        HudScreen* hud = window_->hudScreen();
        QVERIFY(hud && hud->isVisible() && !setup->isVisible());
        if (appState().workoutMode) {
            hud->startEndButton()->click();
            QVERIFY(hud->engine().started());
        }
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
        workoutData().clear();
        selectedDevices::clear();
        awotest::writeFile(QStringLiteral("savedSettingsANT.json"),
                           QJsonDocument(QJsonObject{{"wheelCircumference", 2.1}, {"ftp", kFtp}, {"maxHr", 185},
                                                     {"maxWatts", 1200}, {"powerDriveCutoff", 300}, {"musicVolume", 0}})
                               .toJson());
        ant_ = std::make_unique<awotest::FakeAnt>();
        auto bridge = ant_->makeBridge();
        bridge->start();
        window_ = std::make_unique<MainWindow>(std::move(bridge));
        window_->show();
        i_ = 0;
        watts_ = 180;
        trainer_ = false;
        channels_.clear();
    }

    void cleanup() {
        window_->bridge()->stop();
        window_->bridge()->wait(5000);
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            widget->hide();
        }
        window_.reset();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ant_.reset();
        selectedDevices::clear();
        dir_.reset();
    }

    void sensorsAutoConnectRideAndSynopsis() {
        connectSensors();
        ant::AntBridge* bridge = window_->bridge();
        // auto-selected: one of each, so connected without asking
        QCOMPARE(bridge->selectedRoles(), (QSet<Role>{Role::Power, Role::Hrm, Role::SpeedCadence}));
        QCOMPARE(bridge->manager().mode(), antnative::ChannelMode::Channels);
        QCOMPARE(appState().ftp, kFtp);
        QCOMPARE(appState().wheelCircumference, 2.1);

        loadWorkout();
        QCOMPARE(appState().workoutDurationSeconds, 50);
        startWorkout();
        HudScreen* hud = ride(60);
        QVERIFY(hud->engine().completeShown());
        // the readings reached the gauges and the ride's history
        QVERIFY(hud->powerGauge()->gauge()->value() >= 180);
        QVERIFY(workoutData().power.size() > 40 && workoutData().heartRate.size() > 40 &&
                workoutData().speedCadence.size() > 40);
        QVERIFY(hud->heartRateGauge()->gauge()->value() >= 130 && hud->heartRateGauge()->gauge()->value() <= 135);
        // every block and interval rep graded: 2 blocks, 2 reps of 2, 1 block
        int graded = 0;
        for (const auto& [key, grade] : appState().blockGrades.items()) {
            graded += grade.pct.has_value();
        }
        QCOMPARE(graded, 7);

        // Esc: the End Workout prompt (again: the same prompt, not a second one); then Exit to Synopsis
        window_->escapePressed();
        ConfirmEndWorkoutWindow* prompt = hud->endPrompt();
        QVERIFY(prompt && prompt->isVisible());
        window_->escapePressed();
        QCOMPARE(hud->endPrompt(), prompt);
        prompt->synopsisButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(window_->synopsisScreen() && window_->synopsisScreen()->isVisible(), 5000);
        QVERIFY(!hud->isVisible());

        // back to setup: disconnected, scanning, then connected again by itself
        backToSetup();

        // the exit prompt, and Yes closes the stick
        window_->escapePressed();
        auto* exitPrompt = topLevel<ConfirmExitWindow>();
        QVERIFY(exitPrompt && exitPrompt->isVisible());
        exitPrompt->yesButton()->click();
        QVERIFY(bridge->isFinished());
        const std::vector<std::string> calls = ant_->stick->callsCopy();
        QCOMPARE(std::vector<std::string>(calls.end() - 2, calls.end()),
                 (std::vector<std::string>{"stop_message_pump", "close"}));
    }

    void convertedBlocksRideAndGrade() {
        connectSensors();
        loadWorkout(kConvertedWorkout);
        QCOMPARE(appState().workoutDurationSeconds, 20 + 60 + 40 + 20 + 40);
        startWorkout();
        HudScreen* hud = ride(200);
        QVERIFY(hud->engine().completeShown());
        // every block graded: the steady "ramp", the 60s, 40s and 20s blocks, 2 reps of 2
        QCOMPARE(appState().blockGrades.items().size(), qsizetype(8));
        QStringList messages;
        for (const auto& message : appState().workoutMessages) {
            messages.append(message.text);
        }
        QCOMPARE(messages, (QStringList{"Up", "Up", "Up", "Go", "Go", "Go", "Go"}));
    }

    void freeModeRide() {
        connectSensors();
        window_->setupScreen()->modeButton(QStringLiteral("free"))->click();
        QVERIFY(!appState().workoutMode);
        startWorkout();
        HudScreen* hud = ride(20);
        QVERIFY(appState().secondsPassed >= 15);
        QVERIFY(hud->powerGauge()->gauge()->value() >= 180);
        QVERIFY(toSynopsis() != nullptr);
    }

    // the trainer gets the workout's targets once START is pressed; one over the Power Drive Cutoff is released,
    // never asked for; ending the ride holds it at 0 W
    void trainerFollowsTheWorkout() {
        trainer_ = true;
        ant::AntBridge* bridge = window_->bridge();
        QTRY_VERIFY_WITH_TIMEOUT(bridge->isInitialized(), 5000);
        // (no speed/cadence sensor: picked automatically but connected by hand, as with SimulANT+)
        SetupScreen* setup = window_->setupScreen();
        QTRY_VERIFY_WITH_TIMEOUT((round(), ant_->clock.advance(1), selectedDevices::read().size() == 2), 15000);
        if (!bridge->isConnectRequested()) {
            setup->connectButton()->click();
            auto* prompt = qobject_cast<ConnectDevicesWindow*>(setup->popup());
            QVERIFY(prompt != nullptr);
            prompt->connectButton()->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT((round(), ant_->clock.advance(0.25), bridge->isReady()), 15000);
        learnChannels();
        QVERIFY(bridge->trainerSelected());

        loadWorkout();
        startWorkout();
        ride(60);
        // the bridge thread sends as the trainer is heard
        for (int r = 0; r < 20; ++r) {
            round();
            ant_->clock.advance(0.25);
            QTest::qWait(20);
        }
        QSet<int> watts;
        bool released = false;
        for (const awotest::FakeTransport::Send& send : ant_->stick->sendsCopy()) {
            if (send.payload.size() == 8 && send.payload[0] == 0x31) {
                watts.insert((send.payload[6] | (send.payload[7] << 8)) / 4);
            }
            released = released || send.payload == antnative::pages::basic_resistance(0);
        }
        // 150 W first block, the ramp up from 100 to 200 W, the intervals' 240 W
        QVERIFY2(watts.contains(150) && watts.contains(240), qPrintable(QString::number(watts.size())));
        QVERIFY(std::any_of(watts.begin(), watts.end(), [](int w) { return w > 100 && w < 200 && w != 150; }));
        // the 320 W block is over the 300 W Power Drive Cutoff: released, never asked for
        QVERIFY(released && !watts.contains(320));
        // ending the ride holds the trainer at 0 W
        QVERIFY(toSynopsis() != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            (round(), ant_->clock.advance(0.25), [&] {
                for (const awotest::FakeTransport::Send& send : ant_->stick->sendsCopy()) {
                    if (send.payload == antnative::pages::target_power(0)) {
                        return true;
                    }
                }
                return false;
            }()),
            5000);
    }

    // A workout, then a free ride with the FTP changed between them: the synopsis shows both, one after the other,
    // the workout still in the zones it was ridden in; Clear Session forgets them
    void sessionKeepsEveryRideUntilCleared() {
        connectSensors();
        SetupScreen* setup = window_->setupScreen();
        // nothing ridden yet: nothing to clear
        QVERIFY(!setup->clearSessionButton()->isEnabled());

        loadWorkout();
        startWorkout();
        ride(60);
        SynopsisScreen* synopsis = toSynopsis();
        QVERIFY(synopsis != nullptr);
        QCOMPARE(appState().sessionRides.size(), qsizetype(1));
        const Ride& workout = appState().sessionRides[0];
        QCOMPARE(workout.kind, RideKind::Workout);
        // its ring, from START to the end (50 s), for the next rides' spiral
        QVERIFY(workout.ring && std::abs(workout.ring->seconds - 50) <= 1 && !workout.ring->samples.isEmpty());
        // the only ride: no rides' strip, no dividers
        QVERIFY(synopsis->rideStrip() == nullptr);
        QVERIFY(window_->windowTitle().contains(QStringLiteral("App Flow")));
        const ftpcolors::ZoneRanges workoutZones = zones::powerRanges();

        setup = backToSetup();
        QVERIFY(setup->clearSessionButton()->isEnabled());
        setup->applyFtp(kFtp + 100);
        QVERIFY(zones::powerRanges() != workoutZones);
        setup->modeButton(QStringLiteral("free"))->click();
        startWorkout();
        ride(20);
        synopsis = toSynopsis();
        QVERIFY(synopsis != nullptr);

        QCOMPARE(appState().sessionRides.size(), qsizetype(2));
        const Ride& first = appState().sessionRides[0];
        const Ride& free = appState().sessionRides[1];
        QCOMPARE(free.kind, RideKind::Free);
        QVERIFY(std::abs(free.start - first.seconds) < 1e-6);
        QVERIFY(std::abs(synopsis->totalSeconds() - (first.seconds + free.seconds)) < 1e-6);
        // a heading for each ride along the top
        QVERIFY(synopsis->rideStrip() != nullptr);
        QStringList titles;
        for (const BandStrip::Band& band : synopsis->rideStrip()->bands()) {
            titles.append(band.title);
        }
        QCOMPARE(titles, (QStringList{"Workout: App Flow", "Free Ride"}));
        // the headings at the max labels' size, the theme's Text size too
        const auto labelPoints = [&] {
            QSet<double> points;
            for (const SynopsisScreen::MaxMarker& marker : synopsis->maxMarkers()) {
                points.insert(marker.label->font().pointSizeF());
            }
            return points;
        };
        QCOMPARE(labelPoints(), QSet<double>{synopsis->rideStrip()->font().pointSizeF()});
        theme::current().fonts[theme::kRoleMain].scale = 150;
        theme::apply();
        QApplication::sendPostedEvents();
        synopsis->layoutMaxLabels();
        QCOMPARE(labelPoints(), QSet<double>{synopsis->rideStrip()->font().pointSizeF()});
        QVERIFY(std::abs(synopsis->rideStrip()->font().pointSizeF() - 1.5 * 11 * theme::uiScale()) < 0.01);
        theme::current().fonts[theme::kRoleMain].scale = 100;
        theme::apply();
        // the free ride's readings come after the workout's
        QVERIFY(synopsis->powerValues().first.back() > free.start);
        QVERIFY(*std::min_element(synopsis->powerValues().first.begin(), synopsis->powerValues().first.end()) < first.seconds);
        QCOMPARE(first.powerRanges, workoutZones);
        QCOMPARE(free.powerRanges, zones::powerRanges());
        // more than one ride: no workout name in the title
        QVERIFY(!window_->windowTitle().contains(QStringLiteral("App Flow")));
        // the grades: just the workout's, each ride under its own heading
        QVERIFY(synopsis->gradeText()->text().startsWith(QStringLiteral("Workout Grade")));
        synopsis->showBlockGrades(true);
        const QList<GradeList::Row>& rows = synopsis->blockGradeList()->rows();
        QStringList headings;
        for (const GradeList::Row& row : rows) {
            if (row.kind == GradeList::Row::Kind::Ride) {
                headings.append(row.label);
            }
        }
        QCOMPARE(headings, (QStringList{"Workout: App Flow", "Free Ride"}));
        QVERIFY(rows.front().kind == GradeList::Row::Kind::Ride && rows.back().kind == GradeList::Row::Kind::Ride &&
                rows.back().note.endsWith(QStringLiteral("not graded")));
        synopsis->showBlockGrades(false);
        // a block's grade opens from the graph, from the workout's own record
        synopsis->stepGrade(1);
        QVERIFY(synopsis->gradePopup() != nullptr);
        synopsis->closeGradePopup();
        // the free ride got a ring from its readings: the next workout's spiral starts with both
        const QList<session::RingHistory> history = session::ringHistory();
        QCOMPARE(history.size(), qsizetype(2));
        QVERIFY(std::abs(history[0].seconds - first.ring->seconds) < 1e-6);
        QVERIFY(std::abs(history[1].seconds - free.seconds) < 1e-6);
        PowerSpiral spiral;
        spiral.clear(100, history);
        QVERIFY(std::abs(spiral.workout().duration() - (first.ring->seconds + free.seconds + 100)) < 1e-6);

        // Clear Session (after a Yes) forgets them
        setup = backToSetup();
        setup->clearSessionButton()->click();
        auto* confirm = topLevel<ConfirmClearSessionWindow>();
        if (confirm == nullptr && setup->popup()) {
            confirm = qobject_cast<ConfirmClearSessionWindow*>(setup->popup());
        }
        QVERIFY(confirm != nullptr);
        confirm->yesButton()->click();
        QVERIFY(appState().sessionRides.isEmpty() && !setup->clearSessionButton()->isEnabled());
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
    std::unique_ptr<awotest::FakeAnt> ant_;
    std::unique_ptr<MainWindow> window_;
    QHash<int, int> channels_;
    int i_ = 0;
    int watts_ = 180;
    bool trainer_ = false;
};

QTEST_MAIN(TestAppFlow)
#include "tst_app_flow.moc"
