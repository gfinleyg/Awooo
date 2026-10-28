#include "AntBridge.hpp"

#include <QFile>

#include "Logger.hpp"
#include "PyCompat.hpp"
#include "WorkoutHelpers.hpp"
#include "ant_pages.hpp"

namespace awo::ant {

namespace {

// Every ANT response event is logged except the routine ones: the ack for every configuration call we make, a
// missed message on a dedicated channel, each acknowledged send's transfer events (a failed trainer send is logged,
// throttled, by serviceTrainerTarget instead), and two dedicated channels' slightly different periods drifting into
// the same timeslot (routine with several sensors)
const QSet<QString> kSkipLoggingEvents = {
    QStringLiteral("RESPONSE_NO_ERROR"),         QStringLiteral("EVENT_RX_FAIL"),
    QStringLiteral("EVENT_CHANNEL_COLLISION"),   QStringLiteral("EVENT_TX"),
    QStringLiteral("EVENT_TRANSFER_TX_START"),   QStringLiteral("EVENT_TRANSFER_TX_COMPLETED"),
    QStringLiteral("EVENT_TRANSFER_TX_FAILED"),
};

// The names in low battery warnings (a combined speed/cadence sensor can't report its battery)
const std::map<Role, QString> kBatteryNames = {
    {Role::Hrm, QStringLiteral("Heart rate monitor")},
    {Role::Power, QStringLiteral("Power meter")},
    {Role::Cadence, QStringLiteral("Cadence sensor")},
    {Role::Speed, QStringLiteral("Speed sensor")},
};
const QString kTrainerBatteryName = QStringLiteral("Trainer");

QString batteryStatusName(int status) {
    switch (status) {
    case 1:
        return QStringLiteral("New");
    case 2:
        return QStringLiteral("Good");
    case 3:
        return QStringLiteral("Ok");
    case 4:
        return QStringLiteral("Low");
    case 5:
        return QStringLiteral("Critical");
    default:
        return QStringLiteral("Unknown");
    }
}

// wasLow: it read low last time, so it takes kLowVoltageCleared to clear
bool batteryVoltageLow(const antnative::BatteryStatus& battery, bool wasLow) {
    if (!battery.voltage || *battery.voltage < AntBridge::kLowestJudgedVoltage) {
        return false;
    }
    return *battery.voltage < (wasLow ? AntBridge::kLowVoltageCleared : AntBridge::kLowVoltage);
}

BatteryInfo batteryInfo(const antnative::BatteryStatus& battery) {
    return {battery.status, battery.level_percent, battery.voltage, battery.is_low(), batteryStatusName(battery.status)};
}

QString hex2(int value) {
    return QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
}

// (11, 100), as the Python bridge logged identities
QString identityText(const DeviceId& id) {
    return QStringLiteral("(%1, %2)").arg(id.first).arg(id.second);
}

// What a send came to: the framer's result, or why it couldn't be sent
struct SendOutcome {
    std::optional<int> result;
    QString error;

    QString text() const { return result ? QString::number(*result) : QStringLiteral("error: ") + error; }
};

}  // namespace

QString roleName(Role role) {
    switch (role) {
    case Role::Cadence:
        return QStringLiteral("cadence");
    case Role::Hrm:
        return QStringLiteral("hrm");
    case Role::Power:
        return QStringLiteral("power");
    case Role::Speed:
        return QStringLiteral("speed");
    case Role::SpeedCadence:
        return QStringLiteral("speed_cadence");
    }
    return {};
}

std::optional<Role> roleForDeviceType(int deviceType) {
    switch (deviceType) {
    case kDeviceTypeHeartRate:
        return Role::Hrm;
    case kDeviceTypePower:
    case kDeviceTypeTrainer:
        return Role::Power;
    case kDeviceTypeSpeedCadence:
        return Role::SpeedCadence;
    case kDeviceTypeCadence:
        return Role::Cadence;
    case kDeviceTypeSpeed:
        return Role::Speed;
    default:
        return std::nullopt;
    }
}

Selection readSelectedDevices() {
    Selection selected;
    // (selectedDevices only ever holds well formed "<type> <number>" lines)
    for (const QString& line : selectedDevices::read()) {
        const QStringList parts = line.split(u' ');
        const int type = parts[0].toInt();
        if (const std::optional<Role> role = roleForDeviceType(type)) {
            selected[*role] = {type, parts[1].toInt()};
        }
    }
    return selected;
}

const std::vector<std::uint8_t> AntBridge::kNetworkKey = {0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45};
const QList<double> AntBridge::kInitRetryDelays = {1.0, 2.0, 4.0};
const std::vector<std::uint8_t> AntBridge::kCalibrationRequest = {0x01, 0xAA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const QStringList AntBridge::kStaleFiles = {
    QStringLiteral("foundDevicesANT.txt"), QStringLiteral("selectedDevicesANT.txt"), QStringLiteral("connectANT.txt"),
    QStringLiteral("readyANT.txt"),        QStringLiteral("errorANT.txt"),          QStringLiteral("warningsANT.txt"),
    QStringLiteral("initANT.txt"),         QStringLiteral("hrmANT.txt"),            QStringLiteral("pmANT.txt"),
    QStringLiteral("scANT.txt"),
};

AntBridge::AntBridge(TransportFactory transportFactory, Clock clock, Sleep sleep, QObject* parent)
    : QThread(parent), clock_(clock ? clock : Clock(antnative::steady_seconds)), initRetryDelays_(kInitRetryDelays) {
    if (!transportFactory) {
        transportFactory = [](antnative::Stick::MessageHandler sink) {
            return std::make_shared<antnative::StickTransport>(std::make_shared<antnative::Stick>(), std::move(sink));
        };
    }
    transport_ = transportFactory([this](const antnative::RawMessage& raw) {
        handleMessage(antnative::decode_message(raw));
    });
    // (the manager's own short waits: a test's fake clock moves on instead)
    if (!sleep) {
        sleep = antnative::sleep_seconds;
    }
    manager_ = std::make_unique<antnative::Manager>(transport_, clock_, sleep, clock_, [](const std::string& line) {
        log::event(QStringLiteral("ANT"), QString::fromStdString(line));
    });
    // Looser than the watchdog's defaults: on dedicated channels a device silent for lost_timeout gets its channel
    // reopened once, and if it's still silent channel_reopen_wait later the stick is reset (tuned on real hardware,
    // where tighter timing escalated too readily)
    antnative::RecoveryOptions options;
    options.stale_timeout = 2.5;
    options.lost_timeout = 5;
    options.channel_reopen_wait = 5;
    options.device_reset_timeout = 10;
    options.device_reset_cooldown = 12;
    recovery_ = std::make_unique<antnative::Recovery>(*manager_, options, clock_, [](const std::string& line) {
        log::event(QStringLiteral("RECOVERY"), QString::fromStdString(line));
    });
}

AntBridge::~AntBridge() {
    stop();
    wait();
    shutdown();
}

// ---- For the screens ----

std::shared_ptr<antnative::Sensor> AntBridge::selectedSensor(Role role) const {
    std::optional<DeviceId> identity;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = selected_.find(role);
        if (found != selected_.end()) {
            identity = found->second;
        }
    }
    if (!identity) {
        return nullptr;
    }
    return manager_->find_sensor(identity->first, identity->second);
}

std::optional<DeviceReading> AntBridge::selectedDevice(Role role) const {
    const std::shared_ptr<antnative::Sensor> sensor = selectedSensor(role);
    if (!sensor) {
        return std::nullopt;
    }
    DeviceReading reading;
    reading.deviceType = sensor->device_type();
    reading.deviceNumber = sensor->device_number();
    reading.transmissionType = sensor->transmission_type();
    reading.state = QString::fromStdString(sensor->get(&antnative::Sensor::state));
    reading.dataStalled = sensor->get(&antnative::Sensor::data_stalled);
    if (const std::optional<antnative::BatteryStatus> battery = sensor->battery()) {
        reading.battery = batteryInfo(*battery);
    }
    const std::shared_ptr<antnative::Profile>& profile = sensor->profile();
    if (!profile) {
        return reading;
    }
    reading.hasProfile = true;
    std::lock_guard<std::mutex> lock(profile->mutex);
    reading.version = profile->version;
    if (auto* power = dynamic_cast<antnative::PowerMeterProfile*>(profile.get())) {
        if (power->latest) {
            reading.powerWatts = power->latest->instantaneous_power_watts;
            reading.powerCadenceRpm = power->latest->power_meter_cadence_rpm;
        }
    } else if (auto* trainer = dynamic_cast<antnative::FitnessEquipmentProfile*>(profile.get())) {
        if (trainer->latest) {
            reading.powerWatts = trainer->latest->instantaneous_power_watts;
            reading.powerCadenceRpm = trainer->latest->power_meter_cadence_rpm;
        }
    } else if (auto* heart = dynamic_cast<antnative::HeartRateProfile*>(profile.get())) {
        if (heart->latest) {
            reading.heartRate = heart->latest->computed_heart_rate;
        }
    } else if (auto* bsc = dynamic_cast<antnative::SpeedCadenceProfile*>(profile.get())) {
        if (bsc->latest) {
            reading.eventTicks = bsc->latest->event_ticks;
            reading.revolutionCount = bsc->latest->revolution_count;
        }
        if (bsc->latest_combined) {
            reading.cadenceEventTicks = bsc->latest_combined->cadence_event_ticks;
            reading.cadenceRevolutionCount = bsc->latest_combined->cadence_revolution_count;
            reading.speedEventTicks = bsc->latest_combined->speed_event_ticks;
            reading.speedRevolutionCount = bsc->latest_combined->speed_revolution_count;
        }
    }
    return reading;
}

QStringList AntBridge::foundDevices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    QStringList devices;
    for (const auto& [type, number] : knownDevices_) {
        devices << QStringLiteral("%1 %2").arg(type).arg(number);
    }
    return devices;
}

bool AntBridge::isInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

double AntBridge::scanSettleRemaining() const {
    std::optional<double> started;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started = scanStarted_;
    }
    if (!started) {
        return kScanSettleSeconds;
    }
    return std::max(0.0, kScanSettleSeconds - (clock_() - *started));
}

std::optional<QString> AntBridge::takeError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(error_, std::nullopt);
}

std::optional<QString> AntBridge::waitingForStick() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waitingMessage_;
}

QSet<Role> AntBridge::selectedRoles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    QSet<Role> roles;
    for (const auto& item : selected_) {
        roles.insert(item.first);
    }
    return roles;
}

bool AntBridge::isConnectRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connectRequested_;
}

bool AntBridge::isReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

void AntBridge::clearConnectionFlags() {
    std::lock_guard<std::mutex> lock(mutex_);
    connectRequested_ = false;
    ready_ = false;
}

QStringList AntBridge::lowBatteryWarnings() const {
    QStringList warnings;
    for (const auto& [role, identity] : selection()) {
        const auto name = kBatteryNames.find(role);
        if (name == kBatteryNames.end()) {
            continue;
        }
        const std::shared_ptr<antnative::Sensor> sensor = selectedSensor(role);
        const std::optional<antnative::BatteryStatus> battery = sensor ? sensor->battery() : std::nullopt;
        if (!battery) {
            continue;
        }
        const QString deviceName = sensor->device_type() == kDeviceTypeTrainer ? kTrainerBatteryName : name->second;
        if (!battery->is_low()) {
            bool voltageLow;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                voltageLow = lowVoltage_.contains(identity);
            }
            if (voltageLow && battery->voltage) {
                warnings << QStringLiteral("%1 low battery voltage (%2V)").arg(deviceName, py::fixed(*battery->voltage, 2));
            }
            continue;
        }
        const QString statusName = batteryStatusName(battery->status);
        QString text = QStringLiteral("%1 battery %2")
                           .arg(deviceName, statusName != u"Unknown" ? statusName.toLower() : QStringLiteral("low"));
        if (battery->level_percent) {
            text += QStringLiteral(" (%1%)").arg(*battery->level_percent);
        }
        warnings << text;
    }
    return warnings;
}

bool AntBridge::trainerSelected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto power = selected_.find(Role::Power);
    return power != selected_.end() && power->second.first == kDeviceTypeTrainer;
}

Selection AntBridge::selection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return selected_;
}

void AntBridge::requestConnect(const Selection& selection) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingConnect_ = selection;
        pendingDisconnect_ = false;
        connectRequested_ = true;
        // not ready again until this selection is latched in: until then the old connection would be described
        ready_ = false;
    }
    wake();
}

void AntBridge::requestDisconnect() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingDisconnect_ = true;
        pendingConnect_.reset();
        connectRequested_ = false;
        ready_ = false;
        // a real rescan: forget what was found (devices are rediscovered as they're heard) and hold off
        // auto-selecting until the scan has run again for kScanSettleSeconds
        knownDevices_.clear();
        scanStarted_.reset();
    }
    wake();
}

void AntBridge::requestPowerCalibration(int deviceNumber) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        calibration_ = Calibration{deviceNumber, clock_() + kCalibrationTimeoutSeconds, 0.0, 0, false};
    }
    log::event(QStringLiteral("CALIBRATE"), QStringLiteral("Manual zero offset requested for power meter %1").arg(deviceNumber));
    wake();
}

void AntBridge::cancelPowerCalibration() {
    std::lock_guard<std::mutex> lock(mutex_);
    calibration_.reset();
}

void AntBridge::setTrainerTargetPower(std::optional<double> watts) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!watts) {
        trainerTarget_.reset();
        return;
    }
    const int clamped =
        std::max(kTargetPowerMinWatts, std::min(kTargetPowerMaxWatts, static_cast<int>(py::round(*watts))));
    if (!trainerTarget_) {
        trainerTarget_ = TrainerTarget{};
    }
    trainerTarget_->watts = clamped;
}

void AntBridge::releaseTrainerPower() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!trainerTarget_) {
        trainerTarget_ = TrainerTarget{};
    }
    trainerTarget_->watts.reset();
}

void AntBridge::stop() {
    running_ = false;
    // (the wait in run() returns straight away rather than timing out)
    wake();
}

// ---- The thread ----

void AntBridge::wake() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wakePending_ = true;
    }
    wakeCondition_.notify_all();
}

bool AntBridge::woken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wakePending_;
}

bool AntBridge::waitForWake(double seconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    wakeCondition_.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return wakePending_; });
    return std::exchange(wakePending_, false);
}

void AntBridge::run() {
    running_ = true;
    startupCleanup();

    // No stick at startup (or one that won't initialize) isn't the end: report it, then wait for a stick to be
    // plugged in (or unplugged and plugged back in) and start over with it, so the app needn't be restarted
    while (true) {
        const std::optional<QString> error = initializeWithRetries();
        if (!running_) {
            shutdown();
            return;
        }
        if (!error) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waitingMessage_ = *error + QStringLiteral("\nPlug in an ANT USB stick, it will be detected automatically");
            error_ = error;
        }
        emit errorOccurred(*error);
        if (!waitForNewStick()) {
            shutdown();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waitingMessage_.reset();
        }
        log::event(QStringLiteral("ANT"), QStringLiteral("New ANT USB stick detected, initializing"));
        emit statusMessage(QStringLiteral("New ANT USB stick detected, initializing"));
    }

    recovery_->start();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = true;
        scanStarted_ = clock_();
    }
    log::event(QStringLiteral("ANT"), QStringLiteral("ANT initialized, scanning for devices"));
    emit statusMessage(QStringLiteral("ANT initialized, scanning for devices"));

    // woken by the requests; the timeout only a fallback (shorter while a calibration needs its resends and timeout
    // serviced even if the meter goes quiet)
    while (running_) {
        bool calibrating = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calibrating = calibration_.has_value();
        }
        waitForWake(calibrating ? 0.25 : 2.0);
        processPendingRequest();
        serviceCalibration();
        serviceTrainerTarget();
    }
    shutdown();
}

void AntBridge::startupCleanup() {
    for (const QString& path : kStaleFiles) {
        QFile::remove(path);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        knownDevices_.clear();
        selected_.clear();
        connected_ = false;
        initialized_ = false;
        error_.reset();
        waitingMessage_.reset();
        pendingConnect_.reset();
        pendingDisconnect_ = false;
        connectRequested_ = false;
        ready_ = false;
        wakePending_ = false;
    }
    // nothing is selected yet: a bystander device (someone else's sensor, or anything heard while scanning but never
    // picked) mustn't be able to set off a channel retry or a stick reset
    recovery_->set_watched_devices(std::set<std::pair<int, int>>{});
    log::event(QStringLiteral("ANT"), QStringLiteral("New ANT session starting"));
}

std::optional<QString> AntBridge::initializeWithRetries() {
    // A stick left in a bad state (by a session that didn't shut down cleanly, say) can open fine but ignore the
    // reset at the start of the scan set-up: close it, give it a moment and try again (each try sends a fresh
    // reset), backing off a little more each time. Only the last failure is reported.
    std::optional<QString> error;
    for (int attempt = 1; attempt <= kInitAttempts; ++attempt) {
        if (!running_) {
            return error;
        }
        if (attempt > 1) {
            const double delay = initRetryDelays_[std::min<qsizetype>(attempt - 2, initRetryDelays_.size() - 1)];
            log::event(QStringLiteral("ANT"), QStringLiteral("Resetting ANT USB stick and retrying initialization "
                                                             "(attempt %1/%2) in %3s")
                                                  .arg(attempt)
                                                  .arg(kInitAttempts)
                                                  .arg(py::formatG(delay)));
            try {
                manager_->close();
            } catch (const std::exception& e) {
                log::event(QStringLiteral("ANT"), QStringLiteral("Closing ANT USB stick before retry failed: %1")
                                                      .arg(QString::fromLocal8Bit(e.what())));
            }
            // (interruptible, so stop() during a retry isn't held up)
            waitForWake(delay);
            if (!running_) {
                return error;
            }
        }
        error = initializeAnt();
        if (!error) {
            break;
        }
        log::event(QStringLiteral("ANT"), *error);
    }
    if (error) {
        try {
            manager_->close();
        } catch (...) {
        }
        log::event(QStringLiteral("ANT"), QStringLiteral("ANT initialization failed after %1 attempts, waiting for an "
                                                         "ANT USB stick to be plugged in: %2")
                                              .arg(kInitAttempts)
                                              .arg(*error));
    }
    return error;
}

std::optional<QString> AntBridge::initializeAnt() {
    try {
        log::event(QStringLiteral("ANT"), QStringLiteral("Opening ANT USB stick..."));
        if (!manager_->open()) {
            return QStringLiteral("Failed to open ANT USB stick");
        }
        manager_->configure_scan(kNetworkKey, 0, 0, 57);
        manager_->start();
        return std::nullopt;
    } catch (const std::exception& e) {
        return QStringLiteral("ANT initialization failed: %1").arg(QString::fromLocal8Bit(e.what()));
    }
}

std::set<std::tuple<unsigned, unsigned, unsigned long>> AntBridge::stickIds() {
    std::set<std::tuple<unsigned, unsigned, unsigned long>> ids;
    try {
        for (const antnative::UsbDeviceInfo& device : transport_->list_devices()) {
            ids.insert({device.vid, device.pid, device.serial_number});
        }
    } catch (...) {
        return {};
    }
    return ids;
}

bool AntBridge::waitForNewStick() {
    // Only a stick that wasn't there before starts another initialization: one that's there but failing isn't
    // retried over and over
    auto present = stickIds();
    while (running_) {
        waitForWake(stickPollSeconds_);
        if (!running_) {
            return false;
        }
        const auto current = stickIds();
        for (const auto& id : current) {
            if (!present.contains(id)) {
                return true;
            }
        }
        // (an unplugged stick drops out here, so plugging it back in counts as new)
        present = current;
    }
    return false;
}

void AntBridge::processPendingRequest() {
    std::optional<Selection> pendingConnect;
    bool pendingDisconnect = false;
    bool wasConnected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingConnect = std::exchange(pendingConnect_, std::nullopt);
        pendingDisconnect = std::exchange(pendingDisconnect_, false);
        wasConnected = connected_;
    }

    if (pendingConnect) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            selected_ = *pendingConnect;
            connected_ = true;
        }
        // the devices relied on for the workout now matter to the watchdog, still nothing else it hears
        std::set<std::pair<int, int>> watched;
        QStringList described;
        for (const auto& [role, identity] : *pendingConnect) {
            watched.insert(identity);
            described << roleName(role) + u'=' + identityText(identity);
        }
        recovery_->set_watched_devices(watched);
        log::event(QStringLiteral("CONNECT"), QStringLiteral("Connecting with selection: ") + described.join(QStringLiteral(", ")));
        openDeviceChannels();
        {
            // a disconnect asked for while this was being done wins
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pendingDisconnect_) {
                ready_ = true;
            }
        }
        emit devicesConnected();
        emit batteryChanged();
    } else if (pendingDisconnect && wasConnected) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            selected_.clear();
            connected_ = false;
            trainerTarget_.reset();
        }
        recovery_->set_watched_devices(std::set<std::pair<int, int>>{});
        // back to the scan channel, so the setup screen can find devices again
        try {
            manager_->return_to_scan();
        } catch (const std::exception& e) {
            log::event(QStringLiteral("CONNECT"),
                       QStringLiteral("Returning to device scan failed: %1").arg(QString::fromLocal8Bit(e.what())));
        }
        log::event(QStringLiteral("CONNECT"), QStringLiteral("Disconnected - selection cleared"));
        emit devicesDisconnected();
        emit batteryChanged();
    }

    if (pendingDisconnect) {
        // scanning (again) from here, whether or not a connection was up
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pendingDisconnect_ && !pendingConnect_) {
            scanStarted_ = clock_();
            // cleared again now the scan is back, in case the message pump re-added an old one while
            // requestDisconnect() was clearing it; anything in range is heard again on its next message
            knownDevices_.clear();
        }
    }
}

void AntBridge::openDeviceChannels() {
    // Scanning hears every device but can't send (the stick rejects a send on a scan channel), so the workout's
    // devices each get their own channel. The transmission type is the one heard while scanning (0: match any)
    std::map<DeviceId, int> heard;
    for (const std::shared_ptr<antnative::Sensor>& sensor : manager_->get_devices()) {
        heard[{sensor->device_type(), sensor->device_number()}] = sensor->transmission_type();
    }
    std::vector<antnative::ChannelIdentity> identities;
    for (const auto& [role, identity] : selection()) {
        const auto found = heard.find(identity);
        identities.emplace_back(identity.first, identity.second, found != heard.end() ? found->second : 0);
    }
    try {
        manager_->open_device_channels(identities);
    } catch (const std::exception& e) {
        // still connected on the scan channel: the sensors keep working, nothing can be sent until the watchdog's
        // periodic retry gets the dedicated channels open
        log::event(QStringLiteral("CONNECT"),
                   QStringLiteral("Opening dedicated device channels failed, staying on the scan channel and retrying: %1")
                       .arg(QString::fromLocal8Bit(e.what())));
    }
}

std::optional<int> AntBridge::sendToDevice(int deviceType, int deviceNumber, const std::vector<std::uint8_t>& payload) {
    const std::optional<int> channel = manager_->channel_for(deviceType, deviceNumber);
    if (!channel) {
        return std::nullopt;
    }
    return transport_->send_acknowledged(*channel, payload, 500);
}

void AntBridge::serviceCalibration() {
    // (this thread only: the send waits for the stick's transfer result, which must never hold up the GUI thread
    // or the message pump)
    int deviceNumber = 0;
    int attempts = 0;
    bool timedOut = false;
    bool sendNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!calibration_) {
            return;
        }
        const double now = clock_();
        if (now > calibration_->deadline) {
            timedOut = true;
        } else if (calibration_->heard && now - calibration_->lastSent >= kCalibrationResendSeconds) {
            sendNow = true;
            calibration_->heard = false;
            calibration_->lastSent = now;
            ++calibration_->attempts;
        }
        deviceNumber = calibration_->deviceNumber;
        attempts = calibration_->attempts;
        if (timedOut) {
            calibration_.reset();
        }
    }
    if (timedOut) {
        log::event(QStringLiteral("CALIBRATE"), QStringLiteral("No calibration response from power meter %1 after %2 attempts")
                                                    .arg(deviceNumber)
                                                    .arg(attempts));
        emit calibrationUpdate(deviceNumber, QStringLiteral("timeout"), QVariant(), attempts);
        return;
    }
    if (!sendNow) {
        return;
    }
    SendOutcome outcome;
    try {
        outcome.result = sendToDevice(kDeviceTypePower, deviceNumber, kCalibrationRequest);
        if (!outcome.result) {
            outcome.error = QStringLiteral("device has no dedicated channel");
        }
    } catch (const std::exception& e) {
        outcome.error = QString::fromLocal8Bit(e.what());
    }
    log::event(QStringLiteral("CALIBRATE"),
               QStringLiteral("Calibration request attempt %1 sent, result=%2").arg(attempts).arg(outcome.text()));
    emit calibrationUpdate(deviceNumber, QStringLiteral("sending"), QVariant(), attempts);
}

bool AntBridge::trainerSendDue(const TrainerTarget& target, double now) {
    // A new target goes out right away; one whose send failed is tried again after kTrainerRetrySeconds,
    // kTrainerFastRetries times; otherwise the same target waits out kTrainerResendSeconds, so a stick that keeps
    // failing isn't hammered on every message
    if (!target.sameAsAttempted()) {
        return true;
    }
    const double elapsed = now - target.lastSent;
    if (target.failures > 0 && target.failures <= kTrainerFastRetries && elapsed >= kTrainerRetrySeconds) {
        return true;
    }
    return elapsed >= kTrainerResendSeconds;
}

std::optional<AntBridge::TrainerTarget> AntBridge::trainerTarget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trainerTarget_;
}

bool AntBridge::calibrationHeard() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calibration_ && calibration_->heard;
}

void AntBridge::serviceTrainerTarget() {
    // (this thread only, as serviceCalibration)
    std::optional<int> watts;
    int deviceNumber = 0;
    double now = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto power = selected_.find(Role::Power);
        if (!trainerTarget_ || !trainerTarget_->heard || power == selected_.end() ||
            power->second.first != kDeviceTypeTrainer) {
            return;
        }
        now = clock_();
        if (!trainerSendDue(*trainerTarget_, now)) {
            return;
        }
        if (!trainerTarget_->sameAsAttempted()) {
            trainerTarget_->failures = 0;
        }
        trainerTarget_->heard = false;
        trainerTarget_->lastSent = now;
        trainerTarget_->hasAttempted = true;
        trainerTarget_->attemptedWatts = trainerTarget_->watts;
        watts = trainerTarget_->watts;
        deviceNumber = power->second.second;
    }
    const std::vector<std::uint8_t> payload = watts ? antnative::pages::target_power(*watts) : antnative::pages::basic_resistance(0);
    SendOutcome outcome;
    try {
        outcome.result = sendToDevice(kDeviceTypeTrainer, deviceNumber, payload);
        if (!outcome.result) {
            outcome.error = QStringLiteral("device has no dedicated channel");
        }
    } catch (const std::exception& e) {
        outcome.error = QString::fromLocal8Bit(e.what());
    }
    const bool passed = outcome.result == antnative::kFramerPass;
    // only failures are logged (a send succeeds every couple of seconds), and one that keeps repeating only when its
    // result changes or every kTrainerFailureLogSeconds
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!trainerTarget_) {
            return;
        }
        if (passed) {
            trainerTarget_->failures = 0;
            trainerTarget_->failureLogged.reset();
            return;
        }
        ++trainerTarget_->failures;
        const auto& last = trainerTarget_->failureLogged;
        if (last && last->first == outcome.text() && now - last->second < kTrainerFailureLogSeconds) {
            return;
        }
        trainerTarget_->failureLogged = {outcome.text(), now};
    }
    const QString what =
        watts ? QStringLiteral("Target power %1W").arg(*watts) : QStringLiteral("Release (basic resistance 0%)");
    log::event(QStringLiteral("TRAINER"), QStringLiteral("%1 send failed, result=%2").arg(what, outcome.text()));
}

void AntBridge::shutdown() {
    try {
        recovery_->stop();
    } catch (...) {
    }
    try {
        manager_->stop();
        manager_->close();
    } catch (...) {
    }
}

// ---- The message pump ----

void AntBridge::handleMessage(const antnative::DecodedMessage& message) {
    // Channel events go to the watchdog first (they come without a device), which only records them: reacting here,
    // on the message pump's own thread, could deadlock a later stick reset against it. Every one is logged apart
    // from the routine ones, so the log shows what the channels are doing, not just what set off a recovery
    if (message.event_name) {
        recovery_->notify_channel_event(message.event_name, message.channel);
        const QString name = QString::fromLatin1(message.event_name);
        if (!kSkipLoggingEvents.contains(name)) {
            QString raw;
            if (name == u"UNKNOWN") {
                raw = QStringLiteral(", code=0x%1, response_to=0x%2")
                          .arg(hex2(message.event_code.value_or(0)), hex2(message.response_to_message_id.value_or(0)));
            }
            log::event(QStringLiteral("ANT_EVENT"),
                       QStringLiteral("%1 (channel=%2%3)")
                           .arg(name, message.channel ? QString::number(*message.channel) : QStringLiteral("None"), raw));
        }
    }

    const antnative::Manager::MessageResult result = manager_->on_message(message);
    if (result.dropped || !result.sensor) {
        return;
    }
    const antnative::Sensor& sensor = *result.sensor;
    const DeviceId identity{sensor.device_type(), sensor.device_number()};

    checkCalibrationMessage(message, sensor);
    checkTrainerMessage(sensor);

    bool discovered = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        discovered = knownDevices_.insert(identity).second;
    }
    if (discovered) {
        log::event(QStringLiteral("SCAN"),
                   QStringLiteral("Discovered device type=%1 number=%2").arg(identity.first).arg(identity.second));
        emit deviceDiscovered(identity.first, identity.second);
    }

    checkBattery(identity, sensor);

    if (result.changed) {
        // every role-relevant device that changes, not just the selected one: the reading pipeline looks up the
        // selected device itself
        if (const std::optional<Role> role = roleForDeviceType(identity.first)) {
            emit dataUpdated(*role);
        }
    }
}

void AntBridge::checkTrainerMessage(const antnative::Sensor& sensor) {
    // the selected trainer was just heard: the moment to send to it
    bool due = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto power = selected_.find(Role::Power);
        if (power == selected_.end() || power->second != DeviceId{sensor.device_type(), sensor.device_number()} ||
            sensor.device_type() != kDeviceTypeTrainer || !trainerTarget_) {
            return;
        }
        trainerTarget_->heard = true;
        due = trainerSendDue(*trainerTarget_, clock_());
    }
    if (due) {
        wake();
    }
}

void AntBridge::checkCalibrationMessage(const antnative::DecodedMessage& message, const antnative::Sensor& sensor) {
    // the meter was just heard (the moment to send to it), or it's answering
    int attempts = 0;
    int offset = 0;
    std::optional<bool> success;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!calibration_ || sensor.device_type() != kDeviceTypePower ||
            sensor.device_number() != calibration_->deviceNumber) {
            return;
        }
        const std::vector<std::uint8_t> payload = message.payload.value_or(std::vector<std::uint8_t>{});
        if (payload.size() == 8 && payload[0] == 0x01 && (payload[1] == 0xAC || payload[1] == 0xAF)) {
            success = payload[1] == 0xAC;
            offset = static_cast<std::int16_t>(payload[6] | (payload[7] << 8));
            attempts = calibration_->attempts;
            calibration_.reset();
        } else {
            calibration_->heard = true;
        }
    }
    if (!success) {
        wake();
        return;
    }
    const QString result = *success ? QStringLiteral("success") : QStringLiteral("failed");
    log::event(QStringLiteral("CALIBRATE"),
               QStringLiteral("Power meter %1 calibration %2, offset=%3").arg(sensor.device_number()).arg(result).arg(offset));
    emit calibrationUpdate(sensor.device_number(), result, offset, attempts);
}

void AntBridge::checkBattery(const DeviceId& identity, const antnative::Sensor& sensor) {
    const std::optional<antnative::BatteryStatus> battery = sensor.battery();
    if (!battery) {
        return;
    }
    bool selected = false;
    bool voltageLow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voltageLow = batteryVoltageLow(*battery, lowVoltage_.contains(identity));
        if (voltageLow) {
            lowVoltage_.insert(identity);
        } else {
            lowVoltage_.erase(identity);
        }
        const std::tuple<int, std::optional<int>, bool> state{battery->status, battery->level_percent, voltageLow};
        const auto seen = batterySeen_.find(identity);
        if (seen != batterySeen_.end() && seen->second == state) {
            return;
        }
        batterySeen_[identity] = state;
        for (const auto& item : selected_) {
            selected = selected || item.second == identity;
        }
    }
    const QString level = battery->level_percent ? QStringLiteral(" %1%").arg(*battery->level_percent) : QString();
    const QString voltage = battery->voltage ? QStringLiteral(" %1V").arg(py::fixed(*battery->voltage, 2)) : QString();
    // (once the device itself says low or critical, that says it already)
    const QString voltageWarning =
        voltageLow && !battery->is_low() ? QStringLiteral(" - low battery voltage") : QString();
    log::event(QStringLiteral("BATTERY"), QStringLiteral("Device type=%1 number=%2 battery %3%4%5%6")
                                              .arg(identity.first)
                                              .arg(identity.second)
                                              .arg(batteryStatusName(battery->status), level, voltage, voltageWarning));
    if (selected) {
        emit batteryChanged();
    }
}

// ---- Test hooks ----

void AntBridge::setInitialized(bool initialized) {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = initialized;
}

void AntBridge::markScanStarted() {
    std::lock_guard<std::mutex> lock(mutex_);
    scanStarted_ = clock_();
}

void AntBridge::setRetryTiming(QList<double> initRetryDelays, double stickPollSeconds) {
    initRetryDelays_ = std::move(initRetryDelays);
    stickPollSeconds_ = stickPollSeconds;
}

}  // namespace awo::ant
