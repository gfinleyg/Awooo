// The setup screen (src/ui/screens/SetupScreen) and its popups on a stand-in bridge: auto-selecting and connecting,
// the status and what blinks, the settings and their checks, the modes and the workouts they build, workout files
// loaded and rejected, picking devices by hand, the connect prompt, the workout picker and power calibration

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QtTest>

#include "AnalysisCache.hpp"
#include "AntBridge.hpp"
#include "AppState.hpp"
#include "CalibratePowerWindow.hpp"
#include "ClickTrack.hpp"
#include "DeviceDialogs.hpp"
#include "Dialogs.hpp"
#include "FakeAnt.hpp"
#include "FtpTest.hpp"
#include "KeyShortcuts.hpp"
#include "LongVLabels.hpp"
#include "MusicSelectDialog.hpp"
#include "Performance.hpp"
#include "SavedSettings.hpp"
#include "Session.hpp"
#include "SetupScreen.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutHelpers.hpp"
#include "WorkoutSelectDialog.hpp"
#include "ZoneColors.hpp"

using namespace awo;
using ant::Role;

namespace {

// a bridge whose state the test sets: nothing runs on a thread, and requests are just recorded
class StubBridge : public ant::AntBridge {
public:
    explicit StubBridge(awotest::FakeAnt& fake)
        : AntBridge(
              [transport = fake.stick](antnative::Stick::MessageHandler sink) {
                  transport->sink = std::move(sink);
                  return transport;
              },
              fake.clock, [](double) {}) {}

    QStringList found;
    bool initialized = true;
    bool connecting = false;
    bool ready = false;
    double settle = 0;
    QSet<Role> roles;
    std::optional<ant::DeviceReading> power;
    QStringList batteries;
    std::optional<QString> error;
    QList<ant::Selection> connects;
    int disconnects = 0;
    QList<int> calibrations;

    QStringList foundDevices() const override { return found; }
    bool isInitialized() const override { return initialized; }
    double scanSettleRemaining() const override { return settle; }
    std::optional<QString> takeError() override { return std::exchange(error, std::nullopt); }
    std::optional<QString> waitingForStick() const override { return std::nullopt; }
    QSet<Role> selectedRoles() const override { return roles; }
    bool isConnectRequested() const override { return connecting; }
    bool isReady() const override { return ready; }
    QStringList lowBatteryWarnings() const override { return batteries; }
    std::optional<ant::DeviceReading> selectedDevice(Role role) const override {
        return role == Role::Power ? power : std::nullopt;
    }
    void requestConnect(const ant::Selection& selection) override {
        connects.append(selection);
        connecting = true;
    }
    void requestDisconnect() override {
        ++disconnects;
        connecting = ready = false;
    }
    void requestPowerCalibration(int deviceNumber) override { calibrations.append(deviceNumber); }
    void cancelPowerCalibration() override {}
};

void writeSettings(const QJsonObject& values) {
    QFile file(settings::kSettingsFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(values).toJson());
}

QJsonObject savedSettings() {
    return settings::load();
}

void writeText(const QString& path, const QString& text) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(text.toUtf8());
}

// the rider's settings saved, as a returning rider has them
const QJsonObject kRider{{"wheelCircumference", 2.1}, {"ftp", 200}, {"maxHr", 185}};

}  // namespace

class TestSetup : public QObject {
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
        selectedDevices::clear();
        fake_ = std::make_unique<awotest::FakeAnt>();
        bridge_ = std::make_unique<StubBridge>(*fake_);
    }

    void cleanup() {
        bridge_.reset();
        fake_.reset();
        selectedDevices::clear();
        dir_.reset();
    }

    // ---- Devices ----

    void picksTheOnlyCandidatesAndConnects() {
        SetupScreen setup(bridge_.get());
        bridge_->found = {"11 100", "120 200", "121 400"};
        setup.initAntScanConnect();
        QCOMPARE(selectedDevices::read(), (QStringList{"11 100", "120 200", "121 400"}));
        QCOMPARE(bridge_->connects.size(), 1);
        const ant::Selection& selection = bridge_->connects.front();
        QCOMPARE(selection.size(), std::size_t(3));
        QCOMPARE(selection.at(Role::Power), (ant::DeviceId{11, 100}));
        QCOMPARE(selection.at(Role::SpeedCadence), (ant::DeviceId{121, 400}));
        QCOMPARE(setup.selectedPowerLabel()->text(), QStringLiteral("Power Meter: 100"));
    }

    void twoCandidatesAreLeftToThePick() {
        SetupScreen setup(bridge_.get());
        bridge_->found = {"11 100", "11 101", "120 200", "122 300", "123 301"};
        setup.initAntScanConnect();
        // the heart rate monitor and the cadence and speed pair picked, the power meter left to the rider
        QCOMPARE(selectedDevices::read(), (QStringList{"120 200", "122 300", "123 301"}));
        QVERIFY(bridge_->connects.isEmpty());
        QVERIFY(setup.blinkingPower());
        QVERIFY(setup.powerButton()->isEnabled());
        QVERIFY(setup.initStatusLines().contains(QStringLiteral("Unselected Scanned Devices:")));
        // picked by hand: connected on the next check
        selectedDevices::write(selectedDevices::read() + QStringList{"11 101"});
        setup.initAntScanConnect();
        QCOMPARE(bridge_->connects.size(), 1);
        QCOMPARE(bridge_->connects.front().at(Role::Power), (ant::DeviceId{11, 101}));
    }

    void waitsForTheScanToSettle() {
        SetupScreen setup(bridge_.get());
        bridge_->found = {"11 100", "120 200", "121 400"};
        bridge_->settle = 0.2;
        setup.initAntScanConnect();
        QVERIFY(selectedDevices::read().isEmpty());
        bridge_->settle = 0;
        QTRY_COMPARE_WITH_TIMEOUT(bridge_->connects.size(), 1, 2000);
    }

    void connectedSetsTheDevicesInUse() {
        writeSettings(kRider);
        SetupScreen setup(bridge_.get());
        bridge_->found = {"11 100", "120 200", "121 400"};
        setup.initAntScanConnect();
        bridge_->ready = true;
        bridge_->roles = {Role::Power, Role::Hrm, Role::SpeedCadence};
        ant::DeviceReading meter;
        meter.deviceType = 11;
        meter.deviceNumber = 100;
        bridge_->power = meter;
        setup.initAntScanConnect();
        QVERIFY(setup.initStatusLines().contains(QStringLiteral("Connected Succesfully")));
        QVERIFY(setup.initStatusLines().front() == QStringLiteral("ANT Initialized"));
        QVERIFY(appState().antHRM && appState().antSpeedCadence && !appState().antCadence);
        QVERIFY(setup.calibratePowerButton()->isEnabled());
        QVERIFY(setup.restartScanButton()->isEnabled());
        // Disconnect and Rescan: the picks forgotten, the devices dropped
        setup.restartScanButton()->click();
        QCOMPARE(bridge_->disconnects, 1);
        QVERIFY(selectedDevices::read().isEmpty());
        QVERIFY(!appState().antHRM);
    }

    void warningsShowInRed() {
        bridge_->batteries = {QStringLiteral("Heart rate monitor battery low")};
        SetupScreen setup(bridge_.get());
        const QString status = setup.statusDisplay()->text();
        QVERIFY(status.contains(QStringLiteral("Wheel Circ. must be set!")));
        QVERIFY(status.contains(QStringLiteral("FTP must be set!")));
        QVERIFY(status.contains(QStringLiteral("<span style=\"color:#8B0000;\">Heart rate monitor battery low</span>")));
        // an error from the bridge, once
        bridge_->error = QStringLiteral("No ANT stick found\n");
        setup.initAntScanConnect();
        QVERIFY(setup.statusDisplay()->text().contains(QStringLiteral("Scanning for ANT+ devices,")));
    }

    // ---- Settings ----

    void savedSettingsAreShown() {
        writeSettings({{"wheelCircumference", 2.1}, {"ftp", 263}, {"maxHr", 190}, {"maxWatts", 900}});
        SetupScreen setup(bridge_.get());
        QCOMPARE(setup.wheelDisplay()->text(), QStringLiteral("2.1m"));
        QCOMPARE(setup.ftpDisplay()->text(), QStringLiteral("263 Watts"));
        QCOMPARE(setup.maxHrDisplay()->text(), QStringLiteral("190 BPM"));
        QCOMPARE(appState().maxWatts, 900);
        // no Power Drive Cutoff saved: 120% of FTP
        QCOMPARE(appState().powerDriveCutoff, 316);
        QVERIFY(appState().ftpRanges == zones::powerRanges());
    }

    void outOfRangeSettingsAreLeftToEnter() {
        writeSettings({{"wheelCircumference", 5.0}, {"ftp", 10}, {"maxHr", 300}});
        SetupScreen setup(bridge_.get());
        QCOMPARE(setup.wheelDisplay()->text(), QStringLiteral("0m"));
        QCOMPARE(setup.ftpDisplay()->text(), QStringLiteral("0 Watts"));
        QCOMPARE(setup.maxHrDisplay()->text(), QStringLiteral("0 BPM"));
    }

    void settingsAreCheckedAndSaved() {
        SetupScreen setup(bridge_.get());
        // out of range: back to what it was
        setup.wheelField()->setText(QStringLiteral("5"));
        setup.setWheelButton()->click();
        QCOMPARE(setup.wheelField()->text(), QStringLiteral("0"));
        QCOMPARE(setup.wheelDisplay()->text(), QStringLiteral("0m"));
        setup.wheelField()->setText(QStringLiteral("2.105"));
        setup.setWheelButton()->click();
        QCOMPARE(setup.wheelDisplay()->text(), QStringLiteral("2.105m"));
        QCOMPARE(savedSettings().value("wheelCircumference").toDouble(), 2.105);
        setup.ftpField()->setText(QStringLiteral("abc"));
        setup.setFtpButton()->click();
        QCOMPARE(setup.ftpField()->text(), QStringLiteral("0"));
        setup.ftpField()->setText(QStringLiteral("250"));
        setup.setFtpButton()->click();
        QCOMPARE(setup.ftpDisplay()->text(), QStringLiteral("250 Watts"));
        QCOMPARE(appState().ftp, 250);
        QCOMPARE(savedSettings().value("ftp").toInt(), 250);
        // a new FTP: the zones and the Power Drive Cutoff follow
        QVERIFY(appState().ftpRanges == zones::powerRanges());
        QCOMPARE(savedSettings().value("powerDriveCutoff").toInt(), 300);
        setup.maxHrField()->setText(QStringLiteral("79"));
        setup.setMaxHrButton()->click();
        QCOMPARE(setup.maxHrDisplay()->text(), QStringLiteral("0 BPM"));
        setup.maxHrField()->setText(QStringLiteral("180"));
        setup.setMaxHrButton()->click();
        QCOMPARE(setup.maxHrDisplay()->text(), QStringLiteral("180 BPM"));
        // the weight: 0 or a reasonable one, in the weight unit
        setup.weightField()->setText(QStringLiteral("5"));
        setup.setWeightButton()->click();
        QCOMPARE(Units::current().riderWeightKg, 0.0);
        setup.weightField()->setText(QStringLiteral("165"));
        setup.setWeightButton()->click();
        QCOMPARE(setup.weightDisplay()->text(), QStringLiteral("165 lbs"));
    }

    void maxWattsIsPowerDriveCutoffInWorkoutMode() {
        writeSettings(kRider);
        SetupScreen setup(bridge_.get());
        QCOMPARE(setup.maxWattsLabel()->text(), QStringLiteral("Power Drive Cutoff:"));
        setup.maxWattsField()->setText(QStringLiteral("280"));
        emit setup.maxWattsField()->editingFinished();
        QCOMPARE(appState().powerDriveCutoff, 280);
        setup.chooseMode(QStringLiteral("free"));
        QCOMPARE(setup.maxWattsLabel()->text(), QStringLiteral("Max Watts:"));
        QCOMPARE(setup.maxWattsField()->text(), QStringLiteral("1200"));
        // under the lowest: kept as it was
        setup.maxWattsField()->setText(QStringLiteral("150"));
        emit setup.maxWattsField()->editingFinished();
        QCOMPARE(appState().maxWatts, 1200);
        QCOMPARE(setup.maxWattsField()->text(), QStringLiteral("1200"));
    }

    // ---- The modes and workouts ----

    void startNeedsTheSettingsAConnectionAndAWorkout() {
        writeSettings(kRider);
        SetupScreen setup(bridge_.get());
        setup.initAntScanConnect();
        QVERIFY(!setup.startButton()->isEnabled());
        bridge_->ready = true;
        setup.initAntScanConnect();
        // workout mode: still the workout to pick
        QVERIFY(!setup.startButton()->isEnabled());
        QVERIFY(setup.blinkingWorkoutSelect());
        setup.chooseMode(QStringLiteral("free"));
        QVERIFY(setup.startButton()->isEnabled());
        QVERIFY(setup.blinkingStart());
        QSignalSpy started(&setup, &SetupScreen::startRequested);
        setup.startButton()->click();
        QCOMPARE(started.size(), 1);
        QVERIFY(appState().rideStart.has_value());
        QVERIFY(!appState().workoutMode);
    }

    void ftpTestsAreBuiltAsWorkouts() {
        writeSettings(kRider);
        SetupScreen setup(bridge_.get());
        setup.chooseMode(QStringLiteral("20m"));
        const ftptest::Test* test = ftptest::find(QStringLiteral("20m"));
        QCOMPARE(setup.workoutFileLabel()->text(), u' ' + test->name + u' ');
        QVERIFY(!appState().workoutList.isEmpty());
        QVERIFY(!appState().workoutMessages.isEmpty());
        QVERIFY(!setup.workoutFileButton()->isVisibleTo(&setup));
        QVERIFY(setup.workoutInfoLabel()->text().startsWith(QStringLiteral("Workout Duration: ")));
        // a ramp: its first step, and "more..." under it
        setup.chooseMode(QStringLiteral("ramp20"));
        QCOMPARE(setup.workoutInfoLabel()->text(), QStringLiteral("Warmup + ramp until you stop pedaling"));
        QVERIFY(setup.workoutDisplay()->fullText().endsWith(QStringLiteral("more...\n")));
        // back to workout mode with no file: nothing to ride
        setup.chooseMode(QStringLiteral("workout"));
        QVERIFY(appState().workoutList.isEmpty());
        QCOMPARE(setup.workoutFileLabel()->text(), QStringLiteral("Workout not set"));
    }

    void anEffortTestNeedsTheFtp() {
        SetupScreen setup(bridge_.get());
        setup.chooseMode(QStringLiteral("1h"));
        QCOMPARE(setup.workoutInfoLabel()->text(), QStringLiteral("Set your FTP first"));
        QVERIFY(appState().workoutList.isEmpty());
        // set: built straight away
        setup.ftpField()->setText(QStringLiteral("200"));
        setup.setFtpButton()->click();
        QVERIFY(!appState().workoutList.isEmpty());
    }

    void workoutFilesLoadOrSayWhyNot() {
        writeSettings(kRider);
        SetupScreen setup(bridge_.get());
        writeText(QStringLiteral("good.awo"), QStringLiteral("T:Good One\n1:300,0.75\n0:60\n"));
        QVERIFY(setup.loadWorkoutFile(QStringLiteral("good.awo")));
        QCOMPARE(appState().workoutTitle, QStringLiteral("Good One"));
        QCOMPARE(appState().workoutDurationSeconds, 360);
        QCOMPARE(setup.workoutInfoLabel()->text(), QStringLiteral("Workout Duration: 6m"));
        QCOMPARE(setup.workoutDisplay()->fullText(), workoutText(appState().workoutList, QStringLiteral("Watts")));
        writeText(QStringLiteral("bad.awo"), QStringLiteral("1:300,0.75\n7:10\n"));
        QVERIFY(!setup.loadWorkoutFile(QStringLiteral("bad.awo")));
        QCOMPARE(setup.workoutInfoLabel()->text(),
                 QStringLiteral("Invalid workout: file;line\nbad.awo;2\nInvalid block type: 7\n7:10"));
        QVERIFY(setup.statusDisplay()->text().contains(
            QStringLiteral("Error, line 2: &quot;Invalid block type: 7&quot;")));
        // a good one again clears it
        QVERIFY(setup.loadWorkoutFile(QStringLiteral("good.awo")));
        QVERIFY(!setup.statusDisplay()->text().contains(QStringLiteral("bad.awo")));
        // not a workout file at all
        QVERIFY(!setup.loadWorkoutFile(QStringLiteral("notes.txt")));
        QCOMPARE(setup.workoutInfoLabel()->text(),
                 QStringLiteral("Invalid workout: file\nnotes.txt\nFile must be of type .awo or .zwo"));
    }

    void aNewFtpRebuildsTheWorkout() {
        writeSettings(kRider);
        SetupScreen setup(bridge_.get());
        writeText(QStringLiteral("good.awo"), QStringLiteral("1:300,1.0\n"));
        appState().workoutFilename = QStringLiteral("good.awo");
        QVERIFY(setup.loadWorkoutFile(QStringLiteral("good.awo")));
        QCOMPARE(appState().workoutList.front().block.power[10], 200);
        setup.ftpField()->setText(QStringLiteral("300"));
        setup.setFtpButton()->click();
        QCOMPARE(appState().workoutList.front().block.power[10], 300);
    }

    void musicCadenceModeStartsWithNoMusic() {
        SetupScreen setup(bridge_.get());
        setup.chooseMode(QStringLiteral("cadence"));
        QVERIFY(appState().cadenceMode && appState().workoutMode);
        QCOMPARE(setup.workoutFileButton()->text(), QStringLiteral("Select Music"));
        QCOMPARE(setup.workoutFileLabel()->text(), QStringLiteral("Music not picked"));
        // its targets are cadences: no FTP needed
        QVERIFY(!setup.statusDisplay()->text().contains(QStringLiteral("FTP must be set!")));
        setup.chooseMode(QStringLiteral("workout"));
        QVERIFY(!appState().cadenceMode);
    }

    // ---- Musical Cadence Mode's music ----

    // the song picker analyzes each song as it's added (off the UI thread), the checked ones make the ride; the
    // setup screen builds the ride from them, and next time from the cache without the picker
    void songPickerAnalyzesAndBuildsTheRide() {
        const QString first = QDir::current().absoluteFilePath(QStringLiteral("a_100.wav"));
        const QString second = QDir::current().absoluteFilePath(QStringLiteral("b_75.wav"));
        awotest::writeClickTrack(first, 100, 40, 1);
        awotest::writeClickTrack(second, 75, 40, 2);
        const QString bad = QDir::current().absoluteFilePath(QStringLiteral("bad.mp3"));
        writeText(bad, QStringLiteral("not audio"));
        {
            MusicSelectDialog dialog;
            dialog.addToList({{first, std::nullopt}, {second, QStringLiteral("Slow One")}, {bad, std::nullopt}});
            QVERIFY(dialog.analyzing() && !dialog.okButton()->isEnabled());
            QVERIFY(dialog.list()->item(0)->text().endsWith(QStringLiteral("(analyzing...)")));
            dialog.waitForAnalysis();
            QCOMPARE(dialog.list()->item(0)->text(), QStringLiteral("a_100   40s"));
            QCOMPARE(dialog.list()->item(1)->text(), QStringLiteral("Slow One   40s"));
            QCOMPARE(dialog.list()->item(2)->text(), QStringLiteral("bad   (can't be read)"));
            QCOMPARE(dialog.list()->item(2)->checkState(), Qt::Unchecked);
            QCOMPARE(dialog.summary(), QStringLiteral("Ride: 2 songs, 1m 20s   ·   1 can't be read"));
            QVERIFY(dialog.okButton()->isEnabled());
            // a random ride of 5 minutes takes both
            dialog.randomMinutes()->setValue(5);
            dialog.checkAll(false);
            QVERIFY(!dialog.okButton()->isEnabled());
            dialog.pickRandom();
            QCOMPARE(dialog.checkedSongs().size(), qsizetype(2));
            dialog.preferredRpm()->setValue(90);
            dialog.accept();
            SetupScreen setup(bridge_.get());
            setup.chooseMode(QStringLiteral("cadence"));
            setup.setMusicSelection(dialog.selection());
        }
        // the ride: a block a cadence, the songs' titles as messages, the songs and their headers
        const AppState& state = appState();
        QCOMPARE(state.workoutTitle, QStringLiteral("Musical Cadence Mode"));
        QCOMPARE(state.musicTracks.size(), qsizetype(2));
        QCOMPARE(state.workoutDurationSeconds, 80);
        QVERIFY(!state.workoutList.isEmpty() && !state.musicPulses.isEmpty());
        QCOMPARE(state.workoutMessages.front().text.left(13), QStringLiteral("Now playing: "));
        QVERIFY(state.musicSongStarts.value(0).endsWith(QStringLiteral(" : a_100")) ||
                state.musicSongStarts.value(0).endsWith(QStringLiteral(" : Slow One")));
        // the picker's list and settings kept
        QCOMPARE(music::loadSelection().size(), qsizetype(3));
        QCOMPARE(savedSettings().value(QStringLiteral("musicPreferredRpm")).toInt(), 90);
        QCOMPARE(savedSettings().value(QStringLiteral("musicRandomMinutes")).toInt(), 5);
        // next time: the songs are still analyzed in the cache, so the ride is built straight away
        appState() = AppState();
        SetupScreen again(bridge_.get());
        again.chooseMode(QStringLiteral("cadence"));
        QCOMPARE(again.workoutFileLabel()->text(), QStringLiteral(" Musical Cadence: 2 songs "));
        QCOMPARE(appState().workoutDurationSeconds, 80);
        QVERIFY(again.musicSelection().has_value());
        QCOMPARE(again.musicSelection()->preferred, 90.0);
        // a song gone: the picker's needed again
        QFile::remove(second);
        appState() = AppState();
        SetupScreen gone(bridge_.get());
        gone.chooseMode(QStringLiteral("cadence"));
        QCOMPARE(gone.workoutFileLabel()->text(), QStringLiteral("Music not picked"));
    }

    void clearSessionNeedsRides() {
        SetupScreen setup(bridge_.get());
        QVERIFY(!setup.clearSessionButton()->isEnabled());
        session::rides().append(Ride());
        setup.updateClearSessionButton();
        QVERIFY(setup.clearSessionButton()->isEnabled());
        setup.clearSessionButton()->click();
        auto* prompt = qobject_cast<ConfirmClearSessionWindow*>(setup.popup());
        QVERIFY(prompt != nullptr);
        prompt->yesButton()->click();
        QVERIFY(session::rides().isEmpty());
        QVERIFY(!setup.clearSessionButton()->isEnabled());
    }

    // ---- Picking devices ----

    void devicesArePickedOneOfAKind() {
        const QStringList found{"121 400", "122 300", "123 301", "123 302"};
        SelectDevicesWindow window(found, SelectDevicesWindow::Kind::SpeedCadence, found);
        QCOMPARE(window.buttons().size(), 4);
        QCOMPARE(window.buttons()[0]->text(), QStringLiteral("Speed/Cadence Meter #: 400"));
        // a choice to make: they blink
        QVERIFY(window.blinking());
        // the combined one: the speed and cadence ones greyed out
        window.buttons()[0]->click();
        QCOMPARE(selectedDevices::read(), QStringList{"121 400"});
        QVERIFY(window.isPicked(window.buttons()[0]));
        QVERIFY(!window.buttons()[1]->isEnabled() && !window.buttons()[2]->isEnabled());
        // unpicked: all back
        window.buttons()[0]->click();
        QVERIFY(selectedDevices::read().isEmpty());
        QVERIFY(window.buttons()[1]->isEnabled());
        // a speed meter: the other speed meter and the combined one greyed out, a cadence meter still free
        window.buttons()[2]->click();
        QVERIFY(!window.buttons()[3]->isEnabled() && !window.buttons()[0]->isEnabled());
        QVERIFY(window.buttons()[1]->isEnabled());
        window.buttons()[1]->click();
        QCOMPARE(selectedDevices::read(), (QStringList{"123 301", "122 300"}));
    }

    void devicesPickedBeforeShowPicked() {
        selectedDevices::write({"120 200"});
        SelectDevicesWindow window({}, SelectDevicesWindow::Kind::HeartRate, {"120 201"});
        // the one picked before listed (though not found now) and pressed, without saving it again
        QCOMPARE(window.buttons().size(), 1);
        QVERIFY(window.isPicked(window.buttons()[0]));
        QCOMPARE(selectedDevices::read(), QStringList{"120 200"});
        // (one choice: no blinking)
        QVERIFY(!window.blinking());
    }

    void theConnectPromptSaysWhatsMissing() {
        QCOMPARE(ConnectDevicesWindow::connectingMessage({"11 100"}, {"11 100"}),
                 QStringLiteral("\nPower Meter: 100 - Connecting\n"));
        QCOMPARE(ConnectDevicesWindow::connectingMessage({"11 100", "120 200"}, {"11 100", "121 400"}),
                 QStringLiteral("\nNot all selected devices have been detected yet:\nPower Meter: 100 - Connecting\n"
                                "120 200 - Not Found\n\nOne Unselected Usable Device Scanned:\nSpeed/Cadence Meter: "
                                "400\n\nAvailable devices unselected, are you sure you want to connect?\n"));
        selectedDevices::write({"11 100"});
        bridge_->found = {"11 100"};
        ConnectDevicesWindow window(bridge_.get());
        window.connectButton()->click();
        QCOMPARE(bridge_->connects.size(), 1);
    }

    // ---- The workout picker ----

    void thePickerListsAndPreviewsTheWorkouts() {
        appState().ftp = 200;
        QDir().mkpath(kWorkoutFolder);
        writeText(kWorkoutFolder + QStringLiteral("/a.awo"), QStringLiteral("T:Short\n1:600,0.75\n"));
        writeText(kWorkoutFolder + QStringLiteral("/b.awo"), QStringLiteral("1:1800,0.6\n1:1800,0.6\n"));
        writeText(kWorkoutFolder + QStringLiteral("/c.awo"), QStringLiteral("1:60,0.6\n9:1\n"));
        {
            WorkoutSelectDialog dialog;
            QCOMPARE(dialog.list()->count(), 3);
            QCOMPARE(dialog.summary(), QStringLiteral("3 workouts   ·   1 can't be used"));
            QVERIFY(dialog.list()->item(0)->text().startsWith(QStringLiteral("Short   (a.awo)   10m")));
            QCOMPARE(dialog.list()->item(2)->text(), QStringLiteral("c.awo   (can't be used)"));
            QCOMPARE(dialog.previewTitle(), QStringLiteral("Short\n10m"));
            dialog.list()->setCurrentRow(2);
            QCOMPARE(dialog.previewTitle(), QStringLiteral("Can't be used:\nLine 2: Invalid block type: 9"));
            QVERIFY(!dialog.okButton()->isEnabled());
            // about an hour: b
            dialog.randomMinutes()->setValue(60);
            dialog.pickRandom();
            QVERIFY(dialog.selectedPath().endsWith(QStringLiteral("b.awo")));
            dialog.accept();
        }
        // the list and pick kept for next time
        WorkoutSelectDialog again;
        QVERIFY(again.selectedPath().endsWith(QStringLiteral("b.awo")));
        QCOMPARE(savedSettings().value("workoutRandomMinutes").toInt(), 60);
    }

    // ---- Calibrating ----

    void calibrationMeasuresAnOffsetAndAScale() {
        ant::DeviceReading meter;
        meter.deviceType = 11;
        meter.deviceNumber = 100;
        meter.powerWatts = 210;
        bridge_->power = meter;
        CalibratePowerWindow window(bridge_.get());
        QCOMPARE(window.page(), 0);
        QCOMPARE(window.liveText(), QStringLiteral("Power meter now: 210 W    with new calibration: 210 W"));
        window.nextButton()->click();
        window.sendZeroButton()->click();
        QCOMPARE(bridge_->calibrations, QList<int>{100});
        emit bridge_->calibrationUpdate(100, QStringLiteral("success"), QVariant(-12), 1);
        QCOMPARE(window.zeroStatus(), QStringLiteral("Zero offset succeeded. The meter reported offset -12.\nPress Next."));
        window.finishMeasureWith(QStringLiteral("rest"), {2, 3, 4});
        QCOMPARE(window.offsetWatts(), 3.0);
        window.finishMeasureWith(QStringLiteral("ride"), {203, 213});
        window.setReference(QStringLiteral("220"));
        window.applyScale();
        QCOMPARE(window.scale(), 1.0732);
        window.goToPage(4);
        QVERIFY(window.reviewText().contains(QStringLiteral("At-rest offset: 3 W subtracted from each reading")));
        QVERIFY(window.reviewText().contains(QStringLiteral("Accuracy scale: x1.073")));
        QVERIFY(window.reviewText().contains(QStringLiteral("Zero offset: meter reported offset -12")));
        window.nextButton()->click();
        const auto saved = calibration::get(11, 100);
        QVERIFY(saved.has_value());
        QCOMPARE(saved->offsetWatts, 3.0);
        QCOMPARE(saved->antZeroOffset, std::optional<int>(-12));
    }

    void calibrationNeedsAPowerMeter() {
        CalibratePowerWindow window(bridge_.get());
        QVERIFY(!window.nextButton()->isEnabled());
        QCOMPARE(window.liveText(), QStringLiteral("Power meter: waiting for readings..."));
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
    std::unique_ptr<awotest::FakeAnt> fake_;
    std::unique_ptr<StubBridge> bridge_;
};

QTEST_MAIN(TestSetup)
#include "tst_setup.moc"
