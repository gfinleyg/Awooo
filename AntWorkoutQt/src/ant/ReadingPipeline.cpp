#include "ReadingPipeline.hpp"

#include <algorithm>

#include "AppState.hpp"
#include "WorkoutData.hpp"
#include "WorkoutHelpers.hpp"

namespace awo::ant {

namespace {

// the graph keeps this much of each sensor's readings
void keepGraphWindow(SavedValues& saved) {
    const AppState& state = appState();
    while (saved.times.last() - saved.times.first() > state.graphSecondsShown - state.graphBreakpoint) {
        saved.times.removeFirst();
        saved.values.removeFirst();
    }
}

void save(SavedValues& saved, double value) {
    saved.times.append(appState().secondsPassed);
    saved.values.append(value);
    keepGraphWindow(saved);
}

bool healthy(const std::optional<DeviceReading>& device) {
    return device && device->hasProfile && device->state == u"ACTIVE" && !device->dataStalled;
}

}  // namespace

ReadingPipeline::ReadingPipeline(AntBridge* bridge, AntBridge::Clock clock, QObject* parent)
    : QObject(parent), bridge_(bridge), clock_(clock ? std::move(clock) : AntBridge::Clock(antnative::steady_seconds)) {}

void ReadingPipeline::start() {
    lastPower_.reset();
    lastHr_.reset();
    lastCadenceSource_.reset();
    lastCadenceFallback_.reset();
    lastSpeedSource_.reset();
    lastSpeedFallback_.reset();
    lastNonZeroPower_ = lastNonZeroHr_ = 0;
    lastNonZeroCadence_ = lastNonZeroSpeed_ = 0;
    powerHolds_ = hrHolds_ = cadenceHolds_ = speedHolds_ = 0;
    cadenceSource_.clear();
    speedSource_.clear();
    cadenceTicks_.reset();
    speedTicks_.reset();
    powerMeterCadence_.reset();
    powerHealthy_ = powerFixedResistance_ = false;
    speedCalibration_.clear();
    // (no lines between rides)
    AppState& state = appState();
    state.savedHRVals.clear();
    state.savedPowerVals.clear();
    state.savedCadenceVals.clear();
    state.totalRevolutions = 0;
    calibrations_ = calibration::loadAll();
    active_ = true;
    connect(bridge_, &AntBridge::dataUpdated, this, &ReadingPipeline::onDataUpdated, Qt::UniqueConnection);
}

void ReadingPipeline::stop() {
    active_ = false;
    if (bridge_->trainerSelected()) {
        bridge_->setTrainerTargetPower(0);
    }
    disconnect(bridge_, &AntBridge::dataUpdated, this, &ReadingPipeline::onDataUpdated);
}

void ReadingPipeline::onDataUpdated(Role role) {
    if (!active_) {
        return;
    }
    // only what the role feeds is looked at: the power meter feeds the power and the cadence/speed fallbacks, the
    // heart rate monitor its gauge, a cadence/speed sensor the cadence gauge and speed
    // (no power reading until one is decoded: an FE-C trainer only fills it in from its trainer data page)
    const std::optional<DeviceReading> power = bridge_->selectedDevice(Role::Power);
    const bool hasPower = power && power->hasProfile && power->powerWatts;
    if (role == Role::Power) {
        updatePower(power);
    }
    if (hasPower) {
        powerMeterCadence_ = power->powerCadenceRpm;
        powerHealthy_ = power->state == u"ACTIVE" && !power->dataStalled;
        powerFixedResistance_ = power->deviceType == kDeviceTypePower;
    } else {
        powerMeterCadence_.reset();
        powerHealthy_ = false;
        powerFixedResistance_ = false;
    }
    const AppState& state = appState();
    if (role == Role::Hrm) {
        if (state.antHRM) {
            updateHeartRate();
        }
    } else if (state.showsCadence()) {
        updateCadence(hasPower ? power : std::nullopt);
        // (Musical Cadence Mode shows cadence, from the power meter, with no speed/cadence source selected)
        if (state.anySpeedCadence()) {
            updateSpeed(hasPower ? power : std::nullopt);
        }
    }
}

int ReadingPipeline::calibratedPower(const DeviceReading& device) const {
    const auto found = calibrations_.constFind(calibration::key(device.deviceType, device.deviceNumber));
    return calibration::apply(*device.powerWatts, found != calibrations_.constEnd()
                                                      ? std::optional<calibration::Calibration>(*found)
                                                      : std::nullopt);
}

void ReadingPipeline::updatePower(const std::optional<DeviceReading>& power) {
    if (!power || !power->hasProfile || !power->powerWatts) {
        return;
    }
    const ReadingKey key{power->deviceType, power->deviceNumber, power->version};
    if (lastPower_ == key) {
        return;
    }
    lastPower_ = key;
    int watts = calibratedPower(*power);
    workoutData().power.append({static_cast<double>(watts)});
    if (watts == 0) {
        if (powerHolds_ < kHoldPowerReadings) {
            // a possible misread: the last value for a few readings
            watts = lastNonZeroPower_;
            ++powerHolds_;
        }
    } else {
        powerHolds_ = 0;
        lastNonZeroPower_ = watts;
    }
    emit powerShown(watts);
    save(appState().savedPowerVals, watts);
}

void ReadingPipeline::updateHeartRate() {
    const std::optional<DeviceReading> heart = bridge_->selectedDevice(Role::Hrm);
    if (!heart || !heart->hasProfile || !heart->heartRate) {
        return;
    }
    const ReadingKey key{heart->deviceType, heart->deviceNumber, heart->version};
    if (lastHr_ == key) {
        return;
    }
    lastHr_ = key;
    int bpm = *heart->heartRate;
    workoutData().heartRate.append({static_cast<double>(bpm)});
    if (bpm == 0) {
        if (hrHolds_ < kHoldHrReadings) {
            bpm = lastNonZeroHr_;
            ++hrHolds_;
        }
    } else {
        hrHolds_ = 0;
        lastNonZeroHr_ = bpm;
    }
    emit heartRateShown(bpm);
    save(appState().savedHRVals, bpm);
}

void ReadingPipeline::updateCadence(const std::optional<DeviceReading>& power) {
    // a dedicated sensor (combined or discrete) first, falling back to the power meter's own cadence while it's
    // stale, lost or stalled
    const AppState& state = appState();
    std::optional<DeviceReading> sensor;
    if (state.antCadence) {
        sensor = bridge_->selectedDevice(Role::Cadence);
    } else if (state.antSpeedCadence) {
        sensor = bridge_->selectedDevice(Role::SpeedCadence);
    }
    const bool sensorHealthy = healthy(sensor);
    if (!sensorHealthy) {
        // a recovered sensor's first reading mustn't be diffed against its ticks from before the outage
        cadenceTicks_.reset();
    }

    std::optional<double> cadence;
    bool fallback = false;
    if (sensorHealthy) {
        const bool combined = sensor->deviceType == kDeviceTypeSpeedCadence;
        const bool hasReading = combined ? sensor->cadenceEventTicks.has_value() : sensor->eventTicks.has_value();
        const ReadingKey key{sensor->deviceType, sensor->deviceNumber, sensor->version};
        if (hasReading && lastCadenceSource_ != key) {
            lastCadenceSource_ = key;
            // raw counters for the synopsis' replay, every new page whether or not it has a new pedal stroke (so a
            // combined sensor's speed while coasting is kept, and no gap is long enough for its event time to wrap)
            if (combined) {
                workoutData().speedCadence.append(
                    {static_cast<double>(*sensor->speedRevolutionCount), static_cast<double>(*sensor->speedEventTicks),
                     static_cast<double>(*sensor->cadenceRevolutionCount),
                     static_cast<double>(*sensor->cadenceEventTicks)});
            } else {
                workoutData().cadence.append(
                    {static_cast<double>(*sensor->revolutionCount), static_cast<double>(*sensor->eventTicks)});
            }
            cadence = cadenceFromTicks(*sensor);
        }
    } else if (powerHealthy_ && powerMeterCadence_ && power) {
        const ReadingKey key{power->deviceType, power->deviceNumber, power->version};
        if (lastCadenceFallback_ != key) {
            lastCadenceFallback_ = key;
            cadence = *powerMeterCadence_;
            fallback = true;
            // (no raw counters to record for the replay here)
            workoutData().fallbackCadence.append({static_cast<double>(*powerMeterCadence_)});
        }
    }
    if (!cadence) {
        return;
    }
    double rpm = *cadence;
    const QString source = fallback ? QStringLiteral("fallback") : QStringLiteral("sensor");
    if (source != cadenceSource_) {
        cadenceSource_ = source;
        lastNonZeroCadence_ = 0;
        cadenceHolds_ = 0;
    }
    if (rpm > 200) {
        return;
    }
    if (rpm < 10) {
        if (cadenceHolds_ < kHoldCadenceReadings) {
            rpm = lastNonZeroCadence_;
            ++cadenceHolds_;
        }
    } else {
        lastNonZeroCadence_ = rpm;
        cadenceHolds_ = 0;
    }
    emit cadenceShown(rpm, fallback ? 0 : 1);
    save(appState().savedCadenceVals, static_cast<int>(rpm));
}

void ReadingPipeline::updateSpeed(const std::optional<DeviceReading>& power) {
    // a dedicated sensor first, falling back to a speed estimated from power while it's stale, lost or stalled
    // (only with a fixed resistance power source, see estimateSpeedFromPower)
    const AppState& state = appState();
    std::optional<DeviceReading> sensor;
    if (state.antSpeed) {
        sensor = bridge_->selectedDevice(Role::Speed);
    } else if (state.antSpeedCadence) {
        sensor = bridge_->selectedDevice(Role::SpeedCadence);
    }
    const bool sensorHealthy = healthy(sensor);
    if (!sensorHealthy) {
        speedTicks_.reset();
    }

    std::optional<double> speed;
    bool fallback = false;
    if (sensorHealthy) {
        const bool combined = sensor->deviceType == kDeviceTypeSpeedCadence;
        const bool hasReading = combined ? sensor->speedEventTicks.has_value() : sensor->eventTicks.has_value();
        const ReadingKey key{sensor->deviceType, sensor->deviceNumber, sensor->version};
        if (hasReading && lastSpeedSource_ != key) {
            lastSpeedSource_ = key;
            if (!combined) {
                // raw counters for the synopsis' replay (a combined sensor's are kept with its cadence)
                workoutData().speed.append(
                    {static_cast<double>(*sensor->revolutionCount), static_cast<double>(*sensor->eventTicks)});
            }
            speed = speedFromTicks(*sensor);
            if (speed && power && powerFixedResistance_) {
                // a real, simultaneous power and speed reading teaches the fallback (never an estimate)
                recordSpeedCalibration(calibratedPower(*power), *speed);
            }
        }
    } else if (powerHealthy_ && powerFixedResistance_ && power) {
        const ReadingKey key{power->deviceType, power->deviceNumber, power->version};
        if (lastSpeedFallback_ != key) {
            lastSpeedFallback_ = key;
            if (const std::optional<double> estimated = estimateSpeedFromPower(calibratedPower(*power))) {
                speed = estimated;
                fallback = true;
            }
        }
    }
    if (!speed) {
        return;
    }
    double mph = *speed;
    const QString source = fallback ? QStringLiteral("fallback") : QStringLiteral("sensor");
    if (source != speedSource_) {
        speedSource_ = source;
        lastNonZeroSpeed_ = 0;
        speedHolds_ = 0;
    }
    if (mph > 60) {
        return;
    }
    if (mph < 5) {
        if (speedHolds_ < kHoldSpeedReadings) {
            mph = lastNonZeroSpeed_;
            ++speedHolds_;
        }
    } else {
        lastNonZeroSpeed_ = mph;
        speedHolds_ = 0;
    }
    emit speedShown(mph);
    if (!fallback) {
        // distance from real wheel revolutions only: an estimated speed keeps the ride from looking frozen during a
        // dropout, but adds no made up distance (0.000621371 miles a meter)
        emit distanceShown(state.wheelCircumference * state.totalRevolutions * 0.000621371);
    }
}

std::optional<double> ReadingPipeline::cadenceFromTicks(const DeviceReading& device) {
    const bool combined = device.deviceType == kDeviceTypeSpeedCadence;
    const int ticks = combined ? *device.cadenceEventTicks : *device.eventTicks;
    const int revolutions = combined ? *device.cadenceRevolutionCount : *device.revolutionCount;
    // values a combined sensor's cadence event time is known to glitch to (seen on real hardware)
    if (combined && (ticks == 513 || ticks == 2305)) {
        return std::nullopt;
    }
    if (!cadenceTicks_ || cadenceTicks_->deviceType != device.deviceType ||
        cadenceTicks_->deviceNumber != device.deviceNumber) {
        // a new source: this reading is the baseline, not diffed against another device's counters
        cadenceTicks_ = Ticks{device.deviceType, device.deviceNumber, ticks, revolutions};
        return std::nullopt;
    }
    const int dtTicks = (ticks - cadenceTicks_->ticks) & 0xFFFF;
    const int dRevolutions = (revolutions - cadenceTicks_->revolutions) & 0xFFFF;
    cadenceTicks_->ticks = ticks;
    cadenceTicks_->revolutions = revolutions;
    if (dtTicks == 0) {
        return std::nullopt;
    }
    // revolutions a minute: 61440 = 1024 ticks a second x 60
    const double rpm = (61440.0 * dRevolutions) / dtTicks;
    if (rpm > 200) {
        return std::nullopt;
    }
    return rpm;
}

std::optional<double> ReadingPipeline::speedFromTicks(const DeviceReading& device) {
    const bool combined = device.deviceType == kDeviceTypeSpeedCadence;
    const int ticks = combined ? *device.speedEventTicks : *device.eventTicks;
    const int revolutions = combined ? *device.speedRevolutionCount : *device.revolutionCount;
    if (!speedTicks_ || speedTicks_->deviceType != device.deviceType ||
        speedTicks_->deviceNumber != device.deviceNumber) {
        speedTicks_ = Ticks{device.deviceType, device.deviceNumber, ticks, revolutions};
        return std::nullopt;
    }
    const int dtTicks = (ticks - speedTicks_->ticks) & 0xFFFF;
    const int dRevolutions = (revolutions - speedTicks_->revolutions) & 0xFFFF;
    speedTicks_->ticks = ticks;
    speedTicks_->revolutions = revolutions;
    if (dtTicks == 0) {
        return std::nullopt;
    }
    // MPH: event times in 1/1024 s, x 2.23694 MPH per m/s
    AppState& state = appState();
    const double mph = (state.wheelCircumference * dRevolutions * 2290.62656) / dtTicks;
    if (mph > 60) {
        return std::nullopt;
    }
    // the one place real wheel distance is counted (never from the fallback estimate)
    state.totalRevolutions += dRevolutions;
    return mph;
}

void ReadingPipeline::recordSpeedCalibration(double watts, double mph) {
    const double now = clock_();
    speedCalibration_.append({now, watts, mph});
    const double cutoff = now - kSpeedCalibrationMaxAgeSeconds;
    while (!speedCalibration_.isEmpty() && speedCalibration_.first().time < cutoff) {
        speedCalibration_.removeFirst();
    }
    while (speedCalibration_.size() > kSpeedCalibrationMaxSamples) {
        speedCalibration_.removeFirst();
    }
}

std::optional<double> ReadingPipeline::estimateSpeedFromPower(double watts) const {
    if (speedCalibration_.size() < kSpeedCalibrationMinSamples) {
        return std::nullopt;
    }
    QList<std::pair<double, double>> points;
    for (const SpeedSample& sample : speedCalibration_) {
        points.append({sample.watts, sample.mph});
    }
    std::sort(points.begin(), points.end());
    // below the lowest power ridden, slope down to a stopped wheel at 0 W rather than holding that reading's speed
    // (a rider who stops pedaling would otherwise be shown riding at it)
    if (watts <= 0) {
        return 0.0;
    }
    if (watts < points.first().first) {
        return points.first().second * watts / points.first().first;
    }
    if (watts == points.first().first) {
        return points.first().second;
    }
    if (watts >= points.last().first) {
        return points.last().second;
    }
    for (qsizetype i = 1; i < points.size(); ++i) {
        const auto [p0, s0] = points[i - 1];
        const auto [p1, s1] = points[i];
        if (p0 <= watts && watts <= p1) {
            if (p1 == p0) {
                return s0;
            }
            return s0 + (watts - p0) / (p1 - p0) * (s1 - s0);
        }
    }
    return points.last().second;
}

}  // namespace awo::ant
