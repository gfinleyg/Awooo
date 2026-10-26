#pragma once

// Live sensor readings into what the workout screen shows, and into the workout's sensor history (the logic of
// ClassAntDevicesPoller.py, without the widgets): the screen connects to the *Shown signals.
//
// It reacts to the bridge's dataUpdated, which fires the moment a role's decoded data changes, and only acts on a
// reading the first time it sees it. Along the way:
//   - hold-last-value smoothing: a 0 reading (cadence under 10 RPM, speed under 5 MPH) could be a misread, and shows
//     the last good value instead for a few readings
//   - cadence and speed from a sensor's raw event time and revolution counters (rollover safe), falling back to the
//     power meter's own cadence, and to a speed estimated from power, while the sensor is stale, lost or stalled
//   - the saved power calibration applied to every power reading
//   - distance from real wheel revolutions only (an estimated speed adds none)

#include <QList>
#include <QObject>

#include <optional>

#include "AntBridge.hpp"
#include "PowerCalibration.hpp"

namespace awo::ant {

class ReadingPipeline : public QObject {
    Q_OBJECT

public:
    // the power -> speed fallback learns from real, simultaneous readings: each kept this long, this many at most,
    // and none estimated from fewer than kSpeedCalibrationMinSamples
    static constexpr double kSpeedCalibrationMaxAgeSeconds = 90;
    static constexpr int kSpeedCalibrationMaxSamples = 900;
    static constexpr int kSpeedCalibrationMinSamples = 5;

    struct SpeedSample {
        double time;
        double watts;
        double mph;
    };

    explicit ReadingPipeline(AntBridge* bridge, AntBridge::Clock clock = {}, QObject* parent = nullptr);

    // a ride starts: everything from the last one forgotten, the calibrations read again
    void start();
    // the ride is over: an FE-C trainer isn't left holding the last target (a disconnect releases it entirely)
    void stop();
    bool isActive() const { return active_; }
    void setActive(bool active) { active_ = active; }
    // where the cadence and speed shown last came from: "sensor", "fallback", or empty before the first
    const QString& cadenceSource() const { return cadenceSource_; }
    const QString& speedSource() const { return speedSource_; }

    // Speed from power, by a piecewise linear lookup over recent real (power, speed) readings: on a trainer with fixed
    // resistance, power is a function of wheel speed alone (a gear only changes how the rider gets to that speed). An
    // FE-C trainer changes its own resistance, so it's never asked. Empty without enough recent readings.
    std::optional<double> estimateSpeedFromPower(double watts) const;
    void recordSpeedCalibration(double watts, double mph);
    QList<SpeedSample>& speedCalibration() { return speedCalibration_; }

signals:
    void powerShown(int watts);
    void heartRateShown(int bpm);
    // decimals: 0 for the power meter's cadence (whole RPM only), else 1
    void cadenceShown(double rpm, int decimals);
    void speedShown(double mph);
    void distanceShown(double miles);

private:
    // which reading of which device (a new reading is a new version)
    struct ReadingKey {
        int deviceType = 0;
        int deviceNumber = 0;
        quint64 version = 0;

        bool operator==(const ReadingKey&) const = default;
    };
    // the rollover baseline of a cadence or speed source
    struct Ticks {
        int deviceType = 0;
        int deviceNumber = 0;
        int ticks = 0;
        int revolutions = 0;
    };

    void onDataUpdated(Role role);
    void updatePower(const std::optional<DeviceReading>& power);
    void updateHeartRate();
    void updateCadence(const std::optional<DeviceReading>& power);
    void updateSpeed(const std::optional<DeviceReading>& power);
    int calibratedPower(const DeviceReading& device) const;
    std::optional<double> cadenceFromTicks(const DeviceReading& device);
    std::optional<double> speedFromTicks(const DeviceReading& device);

    AntBridge* bridge_;
    AntBridge::Clock clock_;
    bool active_ = false;

    std::optional<ReadingKey> lastPower_, lastHr_, lastCadenceSource_, lastCadenceFallback_, lastSpeedSource_,
        lastSpeedFallback_;
    int lastNonZeroPower_ = 0, lastNonZeroHr_ = 0;
    double lastNonZeroCadence_ = 0, lastNonZeroSpeed_ = 0;
    int powerHolds_ = 0, hrHolds_ = 0, cadenceHolds_ = 0, speedHolds_ = 0;
    // where the last cadence/speed came from ("sensor" or "fallback"): a switch starts the smoothing over (the
    // fallback takes over when the sensor has stopped changing, which is also what a stopped rider looks like, so
    // its 0 mustn't be replaced with the sensor's reading from before the stop)
    QString cadenceSource_, speedSource_;
    std::optional<Ticks> cadenceTicks_, speedTicks_;
    // the power meter's own cadence, and whether the power source is healthy and fixed resistance (not an FE-C
    // trainer), for the fallbacks
    std::optional<int> powerMeterCadence_;
    bool powerHealthy_ = false;
    bool powerFixedResistance_ = false;
    QList<SpeedSample> speedCalibration_;
    QHash<QString, calibration::Calibration> calibrations_;
};

}  // namespace awo::ant
