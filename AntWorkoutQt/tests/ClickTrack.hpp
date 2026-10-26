#pragma once

// Click tracks for the music tests, made exactly as make_golden.py's golden_click_track makes them (from Python's
// random), so the golden analyses are of the same files byte for byte.

#include <QByteArray>
#include <QString>

#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "PyRandom.hpp"
#include "TestSupport.hpp"

namespace awotest {

// make_golden.py's golden_click_track: a 16 bit mono wav of a click every beat (a decaying noise burst) over a little
// noise, from Python's random
inline void writeClickTrack(const QString& path, double bpm, double seconds, int seed,
                     std::optional<std::pair<double, double>> silence = {},
                     std::optional<std::pair<double, double>> change = {}, int rate = 22050) {
    awo::py::Random rng(static_cast<std::uint64_t>(seed));
    const int n = static_cast<int>(seconds * rate);
    std::vector<double> samples(n, 0.0);
    const int length = static_cast<int>(0.03 * rate);
    std::vector<double> burst(length);
    for (int i = 0; i < length; ++i) {
        const double g = rng.gauss(0, 1);
        burst[i] = g * std::exp(static_cast<double>(-8 * i) / (length - 1));
    }
    double t = 0.0;
    double beat = 60.0 / bpm;
    while (t < seconds) {
        if (change && t >= change->first) {
            beat = 60.0 / change->second;
        }
        const int start = static_cast<int>(t * rate);
        for (int i = 0; i < std::min(length, n - start); ++i) {
            samples[start + i] += 0.6 * burst[i];
        }
        t += beat;
    }
    for (int i = 0; i < n; ++i) {
        samples[i] += 0.002 * rng.gauss(0, 1);
    }
    if (silence) {
        for (int i = static_cast<int>(silence->first * rate); i < static_cast<int>(silence->second * rate); ++i) {
            samples[i] = 0.0;
        }
    }
    QByteArray data;
    const auto u32 = [&data](quint32 v) { data.append(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&data](quint16 v) { data.append(reinterpret_cast<const char*>(&v), 2); };
    data.append("RIFF");
    u32(36 + n * 2);
    data.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    data.append("data");
    u32(n * 2);
    for (const double s : samples) {
        // (Python's int(): toward zero)
        u16(static_cast<quint16>(static_cast<qint16>(static_cast<int>(std::max(-1.0, std::min(1.0, s)) * 32767))));
    }
    awotest::writeFile(path, data);
}

}  // namespace awotest
