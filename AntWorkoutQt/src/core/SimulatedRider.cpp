#include "SimulatedRider.hpp"

#include "AppState.hpp"

#include <algorithm>
#include <cmath>

namespace awo {

SimulatedRider::SimulatedRider(std::uint64_t seed) : rng_(seed) {}

RiderReading SimulatedRider::sample(double seconds, double requested, int ftp, bool cadenceTarget) {
    const double base = requested > 0 && !cadenceTarget ? requested : 140;
    const double power = std::max(0.0, base + rng_.gauss(0, 8) + 6 * std::sin(seconds / 7));
    const double targetHr = 95 + 85 * std::min(power / std::max(ftp, 1), 1.1);
    hr_ += (targetHr - hr_) * 0.04;
    const double cadence =
        (cadenceTarget && requested > 0 ? requested : 88) + 4 * std::sin(seconds / 23) + rng_.gauss(0, 1.5);
    const double speed = 3.2 * std::pow(power, 0.33);
    RiderReading reading;
    reading.power = static_cast<int>(power);
    reading.heartRate = static_cast<int>(hr_ + rng_.gauss(0, 0.6));
    reading.cadence = cadence;
    reading.speedMph = speed;
    return reading;
}

void saveSimulatedReading(double seconds, const RiderReading& reading) {
    AppState& state = appState();
    const double window = state.graphSecondsShown - state.graphBreakpoint;
    const std::pair<SavedValues*, double> values[] = {
        {&state.savedPowerVals, static_cast<double>(reading.power)},
        {&state.savedHRVals, static_cast<double>(reading.heartRate)},
        {&state.savedCadenceVals, static_cast<double>(static_cast<int>(reading.cadence))}};
    for (const auto& [saved, value] : values) {
        saved->times.append(seconds);
        saved->values.append(value);
        while (saved->times.back() - saved->times.front() > window) {
            saved->times.removeFirst();
            saved->values.removeFirst();
        }
    }
}

}  // namespace awo
