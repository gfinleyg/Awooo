#pragma once

// A made-up rider (PreviewRide.py's Rider) for riding the workout screen without an ANT stick: LayoutViewer's
// screens, the theme picker's preview and the tests. It follows the power asked for (140 W with none) with some noise
// and a slow swing, its heart rate rising toward what that power takes at its FTP, a cadence of about 88 RPM (or the
// cadence asked for, in Musical Cadence Mode) and the speed that power makes. Its noise is Python's random from the
// same seed, so a ride matches the Python app's.

#include "PyRandom.hpp"

namespace awo {

struct RiderReading {
    int power = 0;
    int heartRate = 0;
    double cadence = 0;
    double speedMph = 0;
};

class SimulatedRider {
public:
    explicit SimulatedRider(std::uint64_t seed = 7);

    // seconds into the ride, the power (or in Musical Cadence Mode the cadence, cadenceTarget) asked for; ftp: the
    // rider's (0: not set)
    RiderReading sample(double seconds, double requested, int ftp, bool cadenceTarget = false);

private:
    py::Random rng_;
    double hr_ = 95.0;
};

}  // namespace awo
