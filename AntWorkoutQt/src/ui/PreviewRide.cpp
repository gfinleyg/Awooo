#include "PreviewRide.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include "AnalogGauge.hpp"
#include "AppLayout.hpp"
#include "HudScreen.hpp"
#include "LongVLabels.hpp"
#include "SimulatedRider.hpp"
#include "Theme.hpp"
#include "WorkoutSelectDialog.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace {

// a stick that's never there: nothing opens, nothing's sent
class NullTransport : public antnative::Transport {
public:
    std::vector<antnative::UsbDeviceInfo> list_devices() override { return {}; }
    bool open(std::uint32_t, int) override { return false; }
    void close() override {}
    bool is_open() override { return false; }
    bool reset() override { return false; }
    bool set_network_key(int, const std::vector<std::uint8_t>&) override { return false; }
    bool assign_channel(int, int, int) override { return false; }
    bool unassign_channel(int) override { return false; }
    bool set_channel_id(int, int, int, int) override { return false; }
    bool set_channel_period(int, int) override { return false; }
    bool set_channel_rf_frequency(int, int) override { return false; }
    bool set_search_timeout(int, int) override { return false; }
    bool set_low_priority_search_timeout(int, int) override { return false; }
    bool set_rx_extended_messages_enabled(bool) override { return false; }
    bool open_rx_scan_mode() override { return false; }
    bool open_channel(int) override { return false; }
    bool close_channel(int) override { return false; }
    void start_message_pump() override {}
    void stop_message_pump() override {}
    int send_acknowledged(int, const std::vector<std::uint8_t>&, std::uint32_t) override { return 0; }
};

const QString kAppTitle = QStringLiteral("Awooo Workout");
// the app's window behind the screen, and its title (see MainWindow and CustomTitleBar)
const QString kWindowStyle = QStringLiteral(
    "#ridePreview{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}");
const QString kTitleStyle = QStringLiteral(
    "font: 900 20pt 'Orbitron'; background-color:#bdd6de; border: 2px solid #bdd6de;\n                 border-radius: 16px;");
constexpr int kTitleHeight = 36;

}  // namespace

const QString kPreviewWorkout = QStringLiteral("awoWorkouts/0_Little_bit_of_everything.awo");

// ---- PreviewBridge ----

PreviewBridge::PreviewBridge(bool batteryWarnings, bool trainer)
    : AntBridge([](antnative::Stick::MessageHandler) { return std::make_shared<NullTransport>(); }),
      batteryWarnings(batteryWarnings),
      trainer(trainer) {}

std::optional<ant::DeviceReading> PreviewBridge::selectedDevice(ant::Role) const {
    ant::DeviceReading reading;
    reading.deviceType = powerType();
    reading.deviceNumber = 12345;
    return reading;
}

QStringList PreviewBridge::foundDevices() const {
    return {QStringLiteral("%1 12345").arg(powerType()), QStringLiteral("120 23456"), QStringLiteral("121 34567")};
}

QSet<ant::Role> PreviewBridge::selectedRoles() const {
    return {ant::Role::Power, ant::Role::Hrm, ant::Role::SpeedCadence};
}

QStringList PreviewBridge::lowBatteryWarnings() const {
    if (!batteryWarnings) {
        return {};
    }
    QStringList warnings;
    for (const QString& name : {QStringLiteral("Heart rate monitor"), QStringLiteral("Power meter"),
                                QStringLiteral("Cadence sensor"), QStringLiteral("Speed sensor")}) {
        warnings.append(name + QStringLiteral(" battery low (5%)"));
    }
    return warnings;
}

void resetRideState() {
    AppState& state = appState();
    state.secondsPassed = 0;
    state.musicPlayed.clear();
    state.workoutPowerRequestsSeconds.clear();
    state.workoutPowerRequestsWatts.clear();
    state.workoutPowerRequestsSysTime.clear();
    state.savedPowerVals = {};
    state.savedHRVals = {};
    state.savedCadenceVals = {};
    state.totalRevolutions = 0;
    state.sessionRides.clear();
    state.rideStart.reset();
    state.rideRecorded = false;
    workoutData().clear();
}

// ---- RideSandbox ----

RideSandbox::RideSandbox() : state_(appState()) {}

void RideSandbox::swap() {
    std::swap(state_, appState());
    std::swap(data_, workoutData());
}

RideSandbox::Scope::Scope(RideSandbox& sandbox) : sandbox_(sandbox) {
    sandbox_.swap();
}

RideSandbox::Scope::~Scope() {
    sandbox_.swap();
}

// ---- RidePreview ----

RidePreview::RidePreview(const QSize& size) : size_(size) {
    const RideSandbox::Scope scope(sandbox_);
    resetRideState();
    AppState& state = appState();
    state.ftp = 250;
    state.maxHr = 185;
    state.wheelCircumference = 2.105;
    state.workoutMode = true;
    state.cadenceMode = false;
    state.ftpTestMode.clear();
    state.antHRM = true;
    state.antSpeedCadence = true;
    state.antCadence = state.antSpeed = false;
    state.ftpRanges = zones::powerRanges();
    // the workout as the setup screen would load it (its blocks kept in the sandbox's state)
    std::optional<WorkoutPreview> workout;
    if (QFileInfo(kPreviewWorkout).isFile()) {
        workout = previewWorkout(kPreviewWorkout);
    }
    state.workoutMode = workout && workout->ok;
    QString title;
    if (state.workoutMode) {
        state.workoutList = workout->workout.entries;
        state.workoutMessages = workout->workout.messages;
        state.workoutTitle = workout->workout.title;
        state.workoutDurationSeconds = workout->workout.seconds;
        state.workoutFilename = kPreviewWorkout;
        state.workoutFilenameShort = QFileInfo(kPreviewWorkout).fileName();
        title = QStringLiteral("   %1: %2").arg(kAppTitle, state.workoutDisplayName());
    } else {
        // (no workout file to show: the Free Ride screen)
        title = QStringLiteral("   ") + kAppTitle;
    }
    state.startTimestamp = QDateTime::currentDateTime();

    window_ = std::make_unique<QWidget>();
    window_->setObjectName(QStringLiteral("ridePreview"));
    window_->setAttribute(Qt::WA_DontShowOnScreen);
    window_->setAttribute(Qt::WA_StyledBackground);
    theme::setSheet(window_.get(), kWindowStyle);
    auto* layout = new QVBoxLayout(window_.get());
    auto* titleLabel = new QLabel(title);
    theme::setSheet(titleLabel, kTitleStyle);
    titleLabel->setFixedHeight(kTitleHeight);
    layout->addWidget(titleLabel);
    hud_ = new HudScreen(&bridge_, nullptr, [] { return 1000.0; });
    // (redrawn in its own sandbox, see picture: not along with the app's screens)
    hud_->detachFromColors();
    layout->addWidget(hud_);
    const double seconds = (state.workoutMode ? state.workoutDurationSeconds : kPreviewFreeSeconds) * kPreviewProgress;
    SimulatedRider rider(3);
    hud_->runSimulatedRide(seconds, rider);
    hud_->stopTimers();
}

RidePreview::~RidePreview() {
    if (hud_) {
        hud_->stopTimers();
    }
}

QPixmap RidePreview::picture() {
    const RideSandbox::Scope scope(sandbox_);
    hud_->refreshColors();
    layout::scaleSizes(window_.get());
    window_->resize(size_);
    window_->show();
    layout::settle(window_.get());
    layout::fitText(window_.get());
    hud_->fitWorkoutMessage();
    // the block lists' rows laid out now, rather than from the event loop
    for (LongVLabels* list : window_->findChildren<LongVLabels*>()) {
        for (QTimer* timer : list->findChildren<QTimer*>()) {
            timer->stop();
        }
        list->renderLines();
    }
    // the needles right at their readings (not on their way there)
    for (AnalogGauge* gauge : window_->findChildren<AnalogGauge*>()) {
        gauge->snapNeedle();
    }
    layout::settle(window_.get());
    const QPixmap picture = window_->grab();
    window_->hide();
    return picture;
}

}  // namespace awo
