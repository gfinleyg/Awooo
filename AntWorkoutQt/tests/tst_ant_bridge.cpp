// The ANT bridge (src/ant/AntBridge) on a fake stick, with AntNative's real Manager and Recovery: discovery,
// connecting, the trainer's target power, power meter calibration, battery warnings, and starting without a stick
// (tests/test_ant_bridge.py)

#include <QSignalSpy>

#include "FakeAnt.hpp"
#include "Logger.hpp"
#include "TestSupport.hpp"
#include "WorkoutHelpers.hpp"
#include "ant_pages.hpp"

using namespace awo;
using namespace awo::ant;
using awotest::FakeAnt;

namespace {

using Watched = std::set<std::pair<int, int>>;
using Channels = std::map<int, antnative::ChannelIdentity>;

const DeviceId kPower{11, 100};
const DeviceId kTrainer{17, 300};
const DeviceId kHrm{120, 200};

antnative::RawMessage power(int watts = 200, int channel = 0) {
    return awotest::powerPage(100, watts, 90, awotest::uniqueByte(), channel, 5);
}

antnative::RawMessage trainer(int watts = 200, int channel = 0) {
    return awotest::trainerPage(300, watts, 85, 0, channel, 5);
}

antnative::RawMessage hr(int channel = 0) {
    return awotest::hrPage(200, 140, awotest::uniqueByte(), 1000, channel, 1);
}

QString logText() {
    return log::read(5000);
}

QList<DeviceId> discovered(const QSignalSpy& spy) {
    QList<DeviceId> ids;
    for (const QList<QVariant>& args : spy) {
        ids.append({args[0].toInt(), args[1].toInt()});
    }
    return ids;
}

QList<Role> roles(const QSignalSpy& spy) {
    QList<Role> list;
    for (const QList<QVariant>& args : spy) {
        list.append(args[0].value<Role>());
    }
    return list;
}

}  // namespace

class TestAntBridge : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        ant_ = std::make_unique<FakeAnt>();
    }

    void cleanup() {
        bridge_.reset();
        ant_.reset();
        dir_.reset();
        selectedDevices::clear();
    }

    // ---- Selection ----

    void readSelection() {
        selectedDevices::write({QStringLiteral("11 100"), QStringLiteral("120 200"), QStringLiteral("123 7"),
                                QStringLiteral("17 300")});
        // (a trainer and a power meter share the power slot: the later one wins)
        QCOMPARE(readSelectedDevices(), (Selection{{Role::Power, {17, 300}}, {Role::Hrm, {120, 200}}, {Role::Speed, {123, 7}}}));
    }

    // ---- Starting up ----

    void initializeOpensAndScans() {
        initialized();
        QCOMPARE(std::vector<std::string>(ant_->stick->calls.begin(), ant_->stick->calls.begin() + 2),
                 (std::vector<std::string>{"open", "reset"}));
        QCOMPARE(bridge_->manager().mode(), antnative::ChannelMode::Scan);
        QVERIFY(ant_->stick->pumping);
        QVERIFY(logText().contains(QStringLiteral("Opening ANT USB stick")));
    }

    void initializeRetriesThenReports() {
        ant_->stick->fail.insert("open");
        auto bridge = ant_->makeBridge();
        bridge->setRetryTiming({0.001, 0.001, 0.001}, 0.001);
        bridge->setRunning(true);
        QCOMPARE(bridge->initializeWithRetries(), std::optional<QString>(QStringLiteral("Failed to open ANT USB stick")));
        QCOMPARE(ant_->stick->count("open"), AntBridge::kInitAttempts);
        QVERIFY(logText().contains(QStringLiteral("failed after 4 attempts")));
    }

    void initializeRecoversOnALaterAttempt() {
        ant_->stick->failOnce.insert("reset");
        auto bridge = ant_->makeBridge();
        bridge->setRetryTiming({0.001, 0.001, 0.001}, 0.001);
        bridge->setRunning(true);
        QVERIFY(!bridge->initializeWithRetries());
        QCOMPARE(ant_->stick->count("open"), 2);
        QVERIFY(logText().contains(QStringLiteral("ANT initialization failed: ANT scan configuration failed")));
    }

    void initializeStopsWhenAsked() {
        ant_->stick->fail.insert("open");
        auto bridge = ant_->makeBridge();
        bridge->setRunning(false);
        bridge->initializeWithRetries();
        QCOMPARE(ant_->stick->count("open"), 0);
    }

    void waitingForANewStick() {
        auto bridge = ant_->makeBridge();
        bridge->setRetryTiming(AntBridge::kInitRetryDelays, 0.001);
        bridge->setRunning(true);
        int listing = 0;
        ant_->stick->listDevicesHook = [&listing] {
            std::vector<antnative::UsbDeviceInfo> sticks;
            if (++listing >= 3) {
                sticks.push_back({0, 1, 2, 3, 0, std::nullopt, std::nullopt});
            }
            return sticks;
        };
        QVERIFY(bridge->waitForNewStick());
        bridge->setRunning(false);
        QVERIFY(!bridge->waitForNewStick());
    }

    void stickIdsSurviveAListingError() {
        auto bridge = ant_->makeBridge();
        ant_->stick->listDevicesHook = []() -> std::vector<antnative::UsbDeviceInfo> { throw std::runtime_error("usb"); };
        QVERIFY(bridge->stickIds().empty());
    }

    void startupCleanupRemovesOldFiles() {
        for (const QString& name : AntBridge::kStaleFiles.mid(0, 3)) {
            awotest::writeFile(name, "x");
        }
        ant_->makeBridge()->startupCleanup();
        for (const QString& name : AntBridge::kStaleFiles) {
            QVERIFY(!QFile::exists(name));
        }
    }

    // ---- Discovery ----

    void discovery() {
        initialized();
        QSignalSpy found(bridge_.get(), &AntBridge::deviceDiscovered);
        QSignalSpy updated(bridge_.get(), &AntBridge::dataUpdated);
        QCOMPARE(bridge_->scanSettleRemaining(), AntBridge::kScanSettleSeconds);
        hear({power(), power(), hr()});
        QCOMPARE(bridge_->foundDevices(), (QStringList{QStringLiteral("11 100"), QStringLiteral("120 200")}));
        QCOMPARE(discovered(found), (QList<DeviceId>{{11, 100}, {120, 200}}));
        QCOMPARE(roles(updated), (QList<Role>{Role::Power, Role::Power, Role::Hrm}));
        ant_->clock.advance(AntBridge::kScanSettleSeconds + 1);
        QCOMPARE(bridge_->scanSettleRemaining(), 0.0);
        QVERIFY(logText().contains(QStringLiteral("Discovered device type=11 number=100")));
    }

    void unchangedPagesDontSignalNewData() {
        initialized();
        QSignalSpy updated(bridge_.get(), &AntBridge::dataUpdated);
        const antnative::RawMessage page = power();
        hear({page, page});
        QCOMPARE(updated.count(), 1);
    }

    void channelEvents() {
        initialized();
        hear({awotest::channelEvent(0, 0x02), awotest::channelEvent(1, 0x07), awotest::channelEvent(2, 0x99, 0x4B)});
        const QString text = logText();
        // routine events aren't logged
        QVERIFY(!text.contains(QStringLiteral("EVENT_RX_FAIL")));
        QVERIFY2(text.contains(QStringLiteral("EVENT_CHANNEL_CLOSED (channel=1)")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("UNKNOWN (channel=2, code=0x99, response_to=0x4B)")), qPrintable(text));
    }

    // ---- Connecting ----

    void connectOpensDedicatedChannels() {
        initialized();
        QSignalSpy connected(bridge_.get(), &AntBridge::devicesConnected);
        hear({power(), hr()});
        connectDevices({{Role::Power, kPower}, {Role::Hrm, kHrm}});
        QVERIFY(bridge_->isReady() && bridge_->isConnectRequested());
        QCOMPARE(bridge_->selectedRoles(), (QSet<Role>{Role::Power, Role::Hrm}));
        QCOMPARE(bridge_->manager().mode(), antnative::ChannelMode::Channels);
        // the transmission types heard while scanning; channels in role order (hrm, power)
        const Channels expected{{0, {120, 200, 1}}, {1, {11, 100, 5}}};
        QVERIFY(bridge_->manager().channel_devices() == expected);
        QVERIFY(bridge_->recovery().watched_devices() == (Watched{kPower, kHrm}));
        QCOMPARE(connected.count(), 1);
        QCOMPARE(bridge_->selectedDevice(Role::Power)->deviceNumber, 100);
        QVERIFY(!bridge_->selectedDevice(Role::Speed));
    }

    void connectToADeviceNotHeardYet() {
        initialized();
        connectDevices({{Role::Hrm, kHrm}});
        const Channels expected{{0, {120, 200, 0}}};
        QVERIFY(bridge_->manager().channel_devices() == expected);
        QVERIFY(!bridge_->selectedDevice(Role::Hrm));
        hear({hr(0)});
        QCOMPARE(bridge_->selectedDevice(Role::Hrm)->deviceNumber, 200);
    }

    void connectStaysOnScanIfChannelsFail() {
        initialized();
        ant_->stick->failOnce.insert("open_channel");
        connectDevices({{Role::Power, kPower}});
        QVERIFY(bridge_->isReady());
        QCOMPARE(bridge_->manager().mode(), antnative::ChannelMode::Scan);
        QVERIFY(logText().contains(QStringLiteral("Opening dedicated device channels failed")));
    }

    void disconnectGoesBackToScanning() {
        initialized();
        QSignalSpy disconnected(bridge_.get(), &AntBridge::devicesDisconnected);
        hear({power()});
        connectDevices({{Role::Power, kPower}});
        bridge_->setTrainerTargetPower(100);
        bridge_->requestDisconnect();
        QVERIFY(!bridge_->isReady() && !bridge_->isConnectRequested());
        QVERIFY(bridge_->foundDevices().isEmpty());
        QCOMPARE(bridge_->scanSettleRemaining(), AntBridge::kScanSettleSeconds);
        bridge_->processPendingRequest();
        QCOMPARE(bridge_->manager().mode(), antnative::ChannelMode::Scan);
        QVERIFY(bridge_->selectedRoles().isEmpty());
        QVERIFY(!bridge_->trainerTarget());
        QVERIFY(bridge_->recovery().watched_devices() == Watched{});
        QCOMPARE(disconnected.count(), 1);
        QCOMPARE(bridge_->scanSettleRemaining(), AntBridge::kScanSettleSeconds);
        ant_->clock.advance(1);
        QCOMPARE(bridge_->scanSettleRemaining(), AntBridge::kScanSettleSeconds - 1);
    }

    void connectThenDisconnectBeforeProcessing() {
        initialized();
        QSignalSpy connected(bridge_.get(), &AntBridge::devicesConnected);
        QSignalSpy disconnected(bridge_.get(), &AntBridge::devicesDisconnected);
        bridge_->requestConnect({{Role::Power, kPower}});
        bridge_->requestDisconnect();
        bridge_->processPendingRequest();
        // nothing was connected, nothing to disconnect
        QVERIFY(bridge_->selectedRoles().isEmpty() && !bridge_->isReady());
        QCOMPARE(connected.count(), 0);
        QCOMPARE(disconnected.count(), 0);
    }

    void clearConnectionFlags() {
        initialized();
        connectDevices({{Role::Power, kPower}});
        bridge_->clearConnectionFlags();
        QVERIFY(!bridge_->isReady() && !bridge_->isConnectRequested());
        // still connected
        QCOMPARE(bridge_->selectedRoles(), QSet<Role>{Role::Power});
    }

    // ---- The trainer ----

    void trainerSelected() {
        initialized();
        QVERIFY(!bridge_->trainerSelected());
        connectDevices({{Role::Power, kPower}});
        QVERIFY(!bridge_->trainerSelected());
        connectDevices({{Role::Power, kTrainer}});
        QVERIFY(bridge_->trainerSelected());
    }

    void targetPowerIsSentOnceTheTrainerIsHeard() {
        withTrainer();
        bridge_->setTrainerTargetPower(250.4);
        bridge_->serviceTrainerTarget();
        QVERIFY(ant_->stick->sends.empty());  // not heard since
        hear({trainer(201)});
        QVERIFY(bridge_->woken());
        bridge_->serviceTrainerTarget();
        QCOMPARE(payloads(), std::vector<std::vector<std::uint8_t>>{antnative::pages::target_power(250)});
        // the same target isn't sent again until the resend interval
        hear({trainer(202)});
        bridge_->serviceTrainerTarget();
        QCOMPARE(ant_->stick->sends.size(), size_t(1));
        ant_->clock.advance(AntBridge::kTrainerResendSeconds);
        hear({trainer(203)});
        bridge_->serviceTrainerTarget();
        QCOMPARE(ant_->stick->sends.size(), size_t(2));
        // a new target goes right away
        bridge_->setTrainerTargetPower(300);
        hear({trainer(204)});
        bridge_->serviceTrainerTarget();
        QVERIFY(payloads().back() == antnative::pages::target_power(300));
    }

    void targetPowerIsClamped() {
        withTrainer();
        bridge_->setTrainerTargetPower(99999);
        QCOMPARE(*bridge_->trainerTarget()->watts, 4000);
        bridge_->setTrainerTargetPower(-20);
        QCOMPARE(*bridge_->trainerTarget()->watts, 0);
        bridge_->setTrainerTargetPower(std::nullopt);
        QVERIFY(!bridge_->trainerTarget());
    }

    void releaseSendsBasicResistance() {
        withTrainer();
        bridge_->releaseTrainerPower();
        hear({trainer(210)});
        bridge_->serviceTrainerTarget();
        QCOMPARE(payloads(), std::vector<std::vector<std::uint8_t>>{antnative::pages::basic_resistance(0)});
    }

    void failedSendsAreRetriedSoonerAndLoggedOnce() {
        withTrainer();
        ant_->stick->defaultSendResult = 0;
        bridge_->setTrainerTargetPower(200);
        for (int i = 0; i < 3; ++i) {
            hear({trainer()});
            bridge_->serviceTrainerTarget();
            ant_->clock.advance(AntBridge::kTrainerRetrySeconds);
        }
        QCOMPARE(ant_->stick->sends.size(), size_t(3));
        QCOMPARE(logText().count(QStringLiteral("Target power 200W send failed")), 1);
        // after the fast retries, back to the normal interval
        for (int i = 0; i < 3; ++i) {
            hear({trainer()});
            bridge_->serviceTrainerTarget();
            ant_->clock.advance(AntBridge::kTrainerRetrySeconds);
        }
        QCOMPARE(ant_->stick->sends.size(), size_t(4));
        ant_->stick->defaultSendResult = antnative::kFramerPass;
        ant_->clock.advance(AntBridge::kTrainerResendSeconds);
        hear({trainer()});
        bridge_->serviceTrainerTarget();
        QCOMPARE(bridge_->trainerTarget()->failures, 0);
    }

    void powerMeterNeverGetsATarget() {
        initialized();
        hear({power()});
        connectDevices({{Role::Power, kPower}});
        bridge_->setTrainerTargetPower(200);
        hear({power()});
        bridge_->serviceTrainerTarget();
        QVERIFY(ant_->stick->sends.empty());
    }

    void sendWithoutADedicatedChannel() {
        initialized();
        QVERIFY(!bridge_->sendToDevice(17, 300, std::vector<std::uint8_t>(8, 0)));
    }

    void trainerSendDue_data() {
        QTest::addColumn<int>("watts");
        QTest::addColumn<bool>("attempted");
        QTest::addColumn<double>("lastSent");
        QTest::addColumn<int>("failures");
        QTest::addColumn<double>("now");
        QTest::addColumn<bool>("due");
        QTest::newRow("new") << 200 << false << 0.0 << 0 << 0.0 << true;
        QTest::newRow("too soon") << 200 << true << 10.0 << 0 << 11.0 << false;
        QTest::newRow("resend") << 200 << true << 10.0 << 0 << 12.0 << true;
        QTest::newRow("fast retry") << 200 << true << 10.0 << 1 << 10.5 << true;
        QTest::newRow("retries used") << 200 << true << 10.0 << 4 << 10.5 << false;
    }

    void trainerSendDue() {
        QFETCH(int, watts);
        QFETCH(bool, attempted);
        QFETCH(double, lastSent);
        QFETCH(int, failures);
        QFETCH(double, now);
        QFETCH(bool, due);
        AntBridge::TrainerTarget target;
        target.watts = watts;
        target.hasAttempted = attempted;
        target.attemptedWatts = attempted ? std::optional<int>(200) : std::nullopt;
        target.lastSent = lastSent;
        target.failures = failures;
        QCOMPARE(AntBridge::trainerSendDue(target, now), due);
        // a new target is always due
        target.watts = 210;
        QVERIFY(AntBridge::trainerSendDue(target, now));
    }

    // ---- Power meter calibration ----

    void calibrationRequestAndSuccess() {
        withMeter();
        QSignalSpy updates(bridge_.get(), &AntBridge::calibrationUpdate);
        bridge_->requestPowerCalibration(100);
        bridge_->serviceCalibration();
        QVERIFY(ant_->stick->sends.empty());  // waits to hear the meter
        hear({power()});
        bridge_->serviceCalibration();
        QCOMPARE(payloads(), std::vector<std::vector<std::uint8_t>>{AntBridge::kCalibrationRequest});
        QCOMPARE(updates.count(), 1);
        QCOMPARE(updates[0][1].toString(), QStringLiteral("sending"));
        QCOMPARE(updates[0][3].toInt(), 1);
        hear({awotest::message(11, 100, {0x01, 0xAC, 0, 0, 0, 0, 0xF6, 0xFF}, 0, 5)});
        const QList<QVariant> last = updates.last();
        QCOMPARE(last[0].toInt(), 100);
        QCOMPARE(last[1].toString(), QStringLiteral("success"));
        QCOMPARE(last[2].toInt(), -10);
        QCOMPARE(last[3].toInt(), 1);
    }

    void calibrationFailedAnswer() {
        withMeter();
        QSignalSpy updates(bridge_.get(), &AntBridge::calibrationUpdate);
        bridge_->requestPowerCalibration(100);
        hear({awotest::message(11, 100, {0x01, 0xAF, 0, 0, 0, 0, 5, 0}, 0, 5)});
        QCOMPARE(updates.last()[1].toString(), QStringLiteral("failed"));
        QCOMPARE(updates.last()[2].toInt(), 5);
        QCOMPARE(updates.last()[3].toInt(), 0);
    }

    void calibrationTimesOut() {
        withMeter();
        QSignalSpy updates(bridge_.get(), &AntBridge::calibrationUpdate);
        bridge_->requestPowerCalibration(100);
        ant_->clock.advance(AntBridge::kCalibrationTimeoutSeconds + 1);
        bridge_->serviceCalibration();
        QCOMPARE(updates.count(), 1);
        QCOMPARE(updates[0][1].toString(), QStringLiteral("timeout"));
        QVERIFY(!updates[0][2].isValid());
    }

    void calibrationCancelAndOtherDevices() {
        withMeter();
        bridge_->requestPowerCalibration(999);
        hear({power()});
        QVERIFY(!bridge_->calibrationHeard());
        bridge_->cancelPowerCalibration();
        bridge_->serviceCalibration();
        QVERIFY(ant_->stick->sends.empty());
    }

    // ---- Batteries ----

    void lowBatteryWarnings() {
        initialized();
        QSignalSpy changed(bridge_.get(), &AntBridge::batteryChanged);
        hear({power(), hr(), trainer()});
        QVERIFY(bridge_->lowBatteryWarnings().isEmpty());
        hear({awotest::batteryPage(11, 100, 4), awotest::batteryPage(120, 200, 5, 8, 0, 1),
              awotest::batteryPage(17, 300, 4)});
        // nothing selected yet
        QVERIFY(bridge_->lowBatteryWarnings().isEmpty());
        QCOMPARE(changed.count(), 0);
        connectDevices({{Role::Power, kPower}, {Role::Hrm, kHrm}});
        QCOMPARE(bridge_->lowBatteryWarnings(), (QStringList{QStringLiteral("Heart rate monitor battery critical (8%)"),
                                                            QStringLiteral("Power meter battery low")}));
        connectDevices({{Role::Power, kTrainer}});
        QCOMPARE(bridge_->lowBatteryWarnings(), QStringList{QStringLiteral("Trainer battery low")});
    }

    void batteryChangeSignalsOnce() {
        initialized();
        hear({power()});
        connectDevices({{Role::Power, kPower}});
        QSignalSpy changed(bridge_.get(), &AntBridge::batteryChanged);
        hear({awotest::batteryPage(11, 100, 4)});
        hear({awotest::batteryPage(11, 100, 4)});
        QCOMPARE(changed.count(), 1);
        QVERIFY(logText().contains(QStringLiteral("battery Low 3.00V")));
    }

    // ---- The whole thread ----

    void runThread() {
        // run() on its own thread: initializes, connects when asked, drives the trainer and shuts down
        auto bridge = ant_->makeBridge();
        bridge->start();
        QTRY_VERIFY_WITH_TIMEOUT(bridge->isInitialized(), 5000);
        ant_->stick->deliver(trainer());
        bridge->requestConnect({{Role::Power, kTrainer}});
        QTRY_VERIFY_WITH_TIMEOUT(bridge->isReady(), 5000);
        QCOMPARE(bridge->manager().mode(), antnative::ChannelMode::Channels);
        bridge->setTrainerTargetPower(180);
        QTRY_VERIFY_WITH_TIMEOUT((ant_->stick->deliver(trainer(awotest::uniqueByte())), !ant_->stick->sendsCopy().empty()),
                                 5000);
        QVERIFY(ant_->stick->sendsCopy()[0].payload == antnative::pages::target_power(180));
        bridge->stop();
        QVERIFY(bridge->wait(5000));
        const std::vector<std::string> calls = ant_->stick->callsCopy();
        QCOMPARE(std::vector<std::string>(calls.end() - 2, calls.end()),
                 (std::vector<std::string>{"stop_message_pump", "close"}));
    }

    void runThreadWithoutAStick() {
        ant_->stick->sticks.clear();
        auto bridge = ant_->makeBridge();
        bridge->setRetryTiming({0.001, 0.001, 0.001}, 0.01);
        bridge->start();
        QTRY_VERIFY_WITH_TIMEOUT(bridge->waitingForStick().has_value(), 5000);
        QVERIFY(bridge->waitingForStick()->contains(QStringLiteral("Plug in an ANT USB stick")));
        QCOMPARE(bridge->takeError(), std::optional<QString>(QStringLiteral("Failed to open ANT USB stick")));
        QVERIFY(!bridge->takeError());
        // plugged in
        ant_->stick->listDevicesHook = [] {
            return std::vector<antnative::UsbDeviceInfo>{{0, 1, 2, 3, 0, std::nullopt, std::nullopt}};
        };
        QTRY_VERIFY_WITH_TIMEOUT(bridge->isInitialized(), 5000);
        QVERIFY(!bridge->waitingForStick());
        bridge->stop();
        QVERIFY(bridge->wait(5000));
    }

private:
    // the Python tests' bridge fixture: started and scanning, without its thread
    void initialized() {
        bridge_ = ant_->makeBridge();
        bridge_->setRunning(true);
        bridge_->startupCleanup();
        QVERIFY(!bridge_->initializeWithRetries());
        bridge_->setInitialized(true);
        bridge_->markScanStarted();
    }

    void withTrainer() {
        initialized();
        hear({trainer()});
        connectDevices({{Role::Power, kTrainer}});
        ant_->stick->calls.clear();
    }

    void withMeter() {
        initialized();
        hear({power()});
        connectDevices({{Role::Power, kPower}});
        ant_->stick->calls.clear();
    }

    void hear(std::initializer_list<antnative::RawMessage> messages) {
        for (const antnative::RawMessage& message : messages) {
            ant_->stick->deliver(message);
        }
    }

    void connectDevices(const Selection& selection) {
        bridge_->requestConnect(selection);
        bridge_->processPendingRequest();
    }

    std::vector<std::vector<std::uint8_t>> payloads() const {
        std::vector<std::vector<std::uint8_t>> list;
        for (const auto& send : ant_->stick->sends) {
            list.push_back(send.payload);
        }
        return list;
    }

    std::unique_ptr<awotest::TempWorkingDir> dir_;
    std::unique_ptr<FakeAnt> ant_;
    std::unique_ptr<AntBridge> bridge_;
};

QTEST_GUILESS_MAIN(TestAntBridge)
#include "tst_ant_bridge.moc"
