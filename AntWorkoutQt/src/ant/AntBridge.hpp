#pragma once

// The live ANT connection for the life of the app (ClassAntDevices.py's AntWorkoutBridge): it opens the ANT stick,
// scans for ANT+ devices, switches to one dedicated channel per selected device once they're connected (the only way
// to send to them: the trainer's target power, a power meter's calibration), runs the recovery watchdog, and keeps
// going until stop().
//
// It runs AntNative's core directly (Manager, Recovery, the profiles): each message reaches handleMessage() on the
// stick's message pump thread with the sensor it came from and whether it brought new data, as the Python bridge's
// callback did. Tests hand it a fake transport instead of the stick (see tests/FakeAnt.hpp).
//
// The screens talk to it two ways:
//   - signals, emitted from the bridge's or the message pump's thread and queued by Qt onto the GUI thread
//   - plain thread-safe request methods (requestConnect, setTrainerTargetPower...), which only change state under
//     the bridge's lock and wake its thread
//
// The public methods are virtual so the theme picker's preview (PreviewRide) can stand in for a connected bridge.

#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QVariant>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "ant_core.hpp"
#include "ant_manager.hpp"
#include "ant_recovery.hpp"

namespace awo::ant {

Q_NAMESPACE

// The jobs a device does for a workout; enumerated in their names' alphabetical order, the order their dedicated
// channels are opened in (as the Python bridge sorted them)
enum class Role { Cadence, Hrm, Power, Speed, SpeedCadence };
Q_ENUM_NS(Role)

QString roleName(Role role);
inline size_t qHash(Role role, size_t seed = 0) {
    return ::qHash(static_cast<int>(role), seed);
}
// (11 and 17 share the power slot: a power meter or an FE-C trainer)
std::optional<Role> roleForDeviceType(int deviceType);

// (device type, device number)
using DeviceId = std::pair<int, int>;
using Selection = std::map<Role, DeviceId>;

// The UI's current device selection (see WorkoutHelpers' selectedDevices), by role: a trainer and a power meter
// share the power slot, the later one wins
Selection readSelectedDevices();

constexpr int kDeviceTypePower = 11;
constexpr int kDeviceTypeTrainer = 17;
constexpr int kDeviceTypeHeartRate = 120;
constexpr int kDeviceTypeSpeedCadence = 121;
constexpr int kDeviceTypeCadence = 122;
constexpr int kDeviceTypeSpeed = 123;

// FE-C target power range (Target Power, page 49)
constexpr int kTargetPowerMinWatts = 0;
constexpr int kTargetPowerMaxWatts = 4000;

struct BatteryInfo {
    int status = 0;  // 1 new ... 5 critical; anything else unknown
    std::optional<int> levelPercent;
    std::optional<double> voltage;
    bool low = false;
    QString statusName;  // "New", "Good", "Ok", "Low", "Critical" or "Unknown"
};

// A selected device as it is now: what its profile has decoded and the watchdog's view of it
struct DeviceReading {
    int deviceType = 0;
    int deviceNumber = 0;
    int transmissionType = 0;
    QString state;  // "ACTIVE", "STALE" or "LOST"
    bool dataStalled = false;
    bool hasProfile = false;
    // changes whenever the reading does: a new reading has a new version
    quint64 version = 0;
    // power meter (11) or trainer (17)
    std::optional<int> powerWatts;
    std::optional<int> powerCadenceRpm;
    // heart rate (120)
    std::optional<int> heartRate;
    // speed-only (123) / cadence-only (122) raw counters (1/1024 s event time, cumulative revolutions)
    std::optional<int> eventTicks;
    std::optional<int> revolutionCount;
    // combined speed & cadence (121) raw counters
    std::optional<int> cadenceEventTicks;
    std::optional<int> cadenceRevolutionCount;
    std::optional<int> speedEventTicks;
    std::optional<int> speedRevolutionCount;
    std::optional<BatteryInfo> battery;
};

class AntBridge : public QThread {
    Q_OBJECT

public:
    using Clock = std::function<double()>;
    using Sleep = std::function<void(double)>;
    // Makes the transport the manager drives, handing it where to send the stick's messages
    using TransportFactory =
        std::function<std::shared_ptr<antnative::Transport>(antnative::Stick::MessageHandler sink)>;

    static const std::vector<std::uint8_t> kNetworkKey;

    // initialization: up to kInitAttempts tries, waiting these delays between them; once they all fail (no stick,
    // usually) the USB list is checked this often for a newly plugged in stick, which gets a fresh set of tries
    static constexpr int kInitAttempts = 4;
    static const QList<double> kInitRetryDelays;
    static constexpr double kStickPollSeconds = 2.0;
    // how long the scan channel has listened before the setup screen auto-selects: ANT+ sensors send about 4 times
    // a second, so by then every device in range has been heard and a slot with two candidates is a choice to make
    static constexpr double kScanSettleSeconds = 5.0;
    // ANT+ Bicycle Power calibration (page 0x01): a manual zero offset request, answered with 0xAC (success) or
    // 0xAF (failed) and the meter's offset; re-sent right after the meter is heard until it answers or this runs out
    static const std::vector<std::uint8_t> kCalibrationRequest;
    static constexpr double kCalibrationTimeoutSeconds = 15.0;
    static constexpr double kCalibrationResendSeconds = 1.0;
    // FE-C target power: sent when it changes (right after the trainer is heard), then again every
    // kTrainerResendSeconds so a lost command, or a trainer that dropped out and came back, still gets it; a failed
    // send is tried again sooner, kTrainerFastRetries times
    static constexpr double kTrainerResendSeconds = 2.0;
    static constexpr double kTrainerRetrySeconds = 0.5;
    static constexpr int kTrainerFastRetries = 3;
    static constexpr double kTrainerFailureLogSeconds = 30.0;
    // files older versions passed setup state and sensor data through, removed at startup
    static const QStringList kStaleFiles;

    // The real stick and clock by default
    explicit AntBridge(TransportFactory transportFactory = {}, Clock clock = {}, Sleep sleep = {},
                       QObject* parent = nullptr);
    ~AntBridge() override;

    // ---- For the screens (any thread) ----

    // The selected device for a role as it is now; empty before it's selected or heard
    virtual std::optional<DeviceReading> selectedDevice(Role role) const;
    // Every "<device type> <device number>" heard since the scan (re)started, sorted
    virtual QStringList foundDevices() const;
    virtual bool isInitialized() const;
    // seconds until the scan has listened long enough for foundDevices() to hold everything in range
    virtual double scanSettleRemaining() const;
    // the initialization failure, handed out once
    virtual std::optional<QString> takeError();
    // while initialization has failed and a newly plugged in stick is awaited, the text to show
    virtual std::optional<QString> waitingForStick() const;
    virtual QSet<Role> selectedRoles() const;
    virtual bool isConnectRequested() const;
    virtual bool isReady() const;
    // workout over: the connecting/ready state dropped without disconnecting
    virtual void clearConnectionFlags();
    // one line per selected device whose battery is low or critical
    virtual QStringList lowBatteryWarnings() const;
    virtual bool trainerSelected() const;

    virtual void requestConnect(const Selection& selection);
    // Disconnect and Rescan / back to setup: also forgets what was found, and holds off auto-selecting until the
    // scan has run again for kScanSettleSeconds
    virtual void requestDisconnect();
    // An ANT+ manual zero offset calibration; progress and the meter's answer come through calibrationUpdate
    virtual void requestPowerCalibration(int deviceNumber);
    virtual void cancelPowerCalibration();
    // The trainer's target power (empty: stop commanding it); only a change is sent right away
    virtual void setTrainerTargetPower(std::optional<double> watts);
    // Out of ERG mode (Basic Resistance 0%), refreshed like a target; a later target takes it back
    virtual void releaseTrainerPower();
    // Ends the bridge's thread (wait() for it)
    virtual void stop();

    // ---- run()'s steps, and what the tests look at ----

    // the message pump's handler: the stick's thread, or a test's
    void handleMessage(const antnative::DecodedMessage& message);
    void startupCleanup();
    // empty on success, otherwise the last error (the stick left closed)
    std::optional<QString> initializeWithRetries();
    std::optional<QString> initializeAnt();
    bool waitForNewStick();
    std::set<std::tuple<unsigned, unsigned, unsigned long>> stickIds();
    void processPendingRequest();
    void serviceCalibration();
    void serviceTrainerTarget();
    // the send's framer result, or empty when the device has no dedicated channel
    std::optional<int> sendToDevice(int deviceType, int deviceNumber, const std::vector<std::uint8_t>& payload);

    struct TrainerTarget {
        std::optional<int> watts;           // empty: released
        bool hasAttempted = false;          // a send was attempted
        std::optional<int> attemptedWatts;  // what it was for
        double lastSent = 0;
        bool heard = false;
        int failures = 0;
        std::optional<std::pair<QString, double>> failureLogged;  // (result, when)

        bool sameAsAttempted() const { return hasAttempted && watts == attemptedWatts; }
    };
    static bool trainerSendDue(const TrainerTarget& target, double now);
    std::optional<TrainerTarget> trainerTarget() const;
    bool calibrationHeard() const;
    bool woken() const;

    antnative::Manager& manager() { return *manager_; }
    antnative::Recovery& recovery() { return *recovery_; }
    std::shared_ptr<antnative::Sensor> selectedSensor(Role role) const;
    bool running() const { return running_; }
    void setRunning(bool running) { running_ = running; }
    void setInitialized(bool initialized);
    void markScanStarted();
    void setRetryTiming(QList<double> initRetryDelays, double stickPollSeconds);

signals:
    void deviceDiscovered(int deviceType, int deviceNumber);
    void devicesConnected();
    void devicesDisconnected();
    // a role's decoded data just changed
    void dataUpdated(awo::ant::Role role);
    void statusMessage(const QString& text);
    void errorOccurred(const QString& text);
    // result: "success", "failed", "timeout" or "sending"; offset: the meter's (an int) or invalid
    void calibrationUpdate(int deviceNumber, const QString& result, const QVariant& offset, int attempts);
    // a selected device's battery status changed, or the selection did
    void batteryChanged();

protected:
    void run() override;

private:
    struct Calibration {
        int deviceNumber = 0;
        double deadline = 0;
        double lastSent = 0;
        int attempts = 0;
        bool heard = false;
    };

    void wake();
    // waits until woken or seconds pass; true if woken
    bool waitForWake(double seconds);
    void openDeviceChannels();
    void checkTrainerMessage(const antnative::Sensor& sensor);
    void checkCalibrationMessage(const antnative::DecodedMessage& message, const antnative::Sensor& sensor);
    void checkBattery(const DeviceId& identity, const antnative::Sensor& sensor);
    void shutdown();
    Selection selection() const;

    Clock clock_;
    std::shared_ptr<antnative::Transport> transport_;
    std::unique_ptr<antnative::Manager> manager_;
    std::unique_ptr<antnative::Recovery> recovery_;
    std::atomic<bool> running_{false};

    QList<double> initRetryDelays_;
    double stickPollSeconds_ = kStickPollSeconds;

    // guards everything below; never held across a stick call or a signal
    mutable std::mutex mutex_;
    std::condition_variable wakeCondition_;
    bool wakePending_ = false;

    std::set<DeviceId> knownDevices_;
    std::optional<double> scanStarted_;
    Selection selected_;
    bool connected_ = false;
    bool initialized_ = false;
    std::optional<QString> error_;
    std::optional<QString> waitingMessage_;
    bool connectRequested_ = false;
    bool ready_ = false;
    std::optional<Selection> pendingConnect_;
    bool pendingDisconnect_ = false;
    std::optional<Calibration> calibration_;
    std::optional<TrainerTarget> trainerTarget_;
    std::map<DeviceId, std::pair<int, std::optional<int>>> batterySeen_;
};

}  // namespace awo::ant
