#pragma once

// A workout ridden without an ANT stick (PreviewRide.py): a stand-in bridge with everything connected, a ride's own
// AppState and WorkoutData (RideSandbox), and the theme picker's preview: the workout screen part way through a
// simulated ride, drawn offscreen in whatever the theme is now (RidePreview). LayoutViewer shows its screens the same
// way.

#include <QPixmap>
#include <QSize>

#include <memory>

#include "AntBridge.hpp"
#include "AppState.hpp"
#include "WorkoutData.hpp"

class QWidget;

namespace awo {

class HudScreen;

// A bridge with a power meter (or an FE-C trainer), heart rate monitor and speed/cadence sensor found, picked and
// connected, and nothing ever changing; it never opens a stick
class PreviewBridge : public ant::AntBridge {
public:
    explicit PreviewBridge(bool batteryWarnings = false, bool trainer = false);

    bool batteryWarnings;
    // an FE-C trainer as the power device: free mode gets the Watts slider, the setup screen its Max Watts box
    bool trainer;

    std::optional<ant::DeviceReading> selectedDevice(ant::Role role) const override;
    QStringList foundDevices() const override;
    bool isInitialized() const override { return true; }
    double scanSettleRemaining() const override { return 0; }
    std::optional<QString> takeError() override { return std::nullopt; }
    std::optional<QString> waitingForStick() const override { return std::nullopt; }
    QSet<ant::Role> selectedRoles() const override;
    bool isConnectRequested() const override { return true; }
    bool isReady() const override { return true; }
    void clearConnectionFlags() override {}
    QStringList lowBatteryWarnings() const override;
    bool trainerSelected() const override { return trainer; }
    void requestConnect(const ant::Selection&) override {}
    void requestDisconnect() override {}
    void requestPowerCalibration(int) override {}
    void cancelPowerCalibration() override {}
    void setTrainerTargetPower(std::optional<double>) override {}
    void releaseTrainerPower() override {}
    void stop() override {}

private:
    int powerType() const { return trainer ? ant::kDeviceTypeTrainer : ant::kDeviceTypePower; }
};

// AppState and WorkoutData a simulated ride's own while a Scope is alive, then the app's again (the ride's kept for the
// next time); it starts from a copy of the app's. Everything in the scope has to run straight through (no event loop:
// the app's own timers would see the ride's state)
class RideSandbox {
public:
    RideSandbox();

    class Scope {
    public:
        explicit Scope(RideSandbox& sandbox);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        RideSandbox& sandbox_;
    };

private:
    void swap();

    AppState state_;
    WorkoutData data_;
};

// A ride's state back to nothing ridden (PreviewRide.reset_ride_state): a simulated ride is the only one in its session
void resetRideState();

// the preview's ride: this far through the workout (a free ride: through kPreviewFreeSeconds)
constexpr double kPreviewProgress = 0.4;
constexpr double kPreviewFreeSeconds = 20 * 60;
extern const QString kPreviewWorkout;

// The workout screen kPreviewProgress of the way through kPreviewWorkout (ridden by the simulated rider, in its own
// sandbox) in a stand-in for the app's window, drawn offscreen: picture() draws it in the current theme
class RidePreview {
public:
    explicit RidePreview(const QSize& size);
    ~RidePreview();

    QPixmap picture();
    HudScreen* hud() const { return hud_; }

private:
    RideSandbox sandbox_;
    PreviewBridge bridge_;
    QSize size_;
    std::unique_ptr<QWidget> window_;
    HudScreen* hud_ = nullptr;
};

}  // namespace awo
