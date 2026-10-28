#pragma once

// Workout text and small helpers shared by the screens (WorkoutHelpers.py)

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

#include "WorkoutTypes.hpp"

namespace awo {

// "1h 2m 3s" without the parts that are zero ("1m", "1h 30s" is "1h 0m 30s"); a time past its end shows as 0s
QString minimizedDuration(double seconds);

// The riding positions' names in the block text, by Position; seated says nothing
extern const QStringList kPositionNames;

// A Block or Interval entry's text at a difficulty, in the target unit ("Watts", shown as "W", or "RPM" in Musical
// Cadence Mode, shown as "rpm"):
//   free ride  "Free 5m" (seated), "Stand 1m"
//   steady     "200W 5m" (seated), "90rpm 5m"
//   ramp       "↑RPM 100-200W 1m 30s"
//   interval   "4x", then a line for each of its blocks
QStringList blockText(const WorkoutEntry& entry, int difficulty, const QString& unit);
QString blockText(const Block& block, int difficulty, const QString& unit);

// The workout's blocks as the workout lists show them, one line each (intervals' blocks indented under their
// header), at the written difficulty; songStarts puts Musical Cadence Mode's song headers before each song's first block
QString workoutText(const QList<WorkoutEntry>& entries, const QString& unit,
                    const QMap<int, QString>& songStarts = {});

// A block label's target(s) as (low, high), high 0 for a single target, from its first "<n>W" or "<n>-<n>W" word (or
// "<n>rpm"); empty for text without one (free ride, headers)
std::optional<std::pair<int, int>> targetsFromText(const QString& text);

// Musical Cadence Mode's song lines in the workout lists ("<length> : <Artist> - <Title>"); no block's text has " : "
bool isSongHeader(const QString& text, bool cadenceMode);
extern const QString kSongHeaderCss;

template <typename T>
T maxOrZero(const QList<T>& values) {
    T best{};
    bool first = true;
    for (const T& value : values) {
        if (first || value > best) {
            best = value;
            first = false;
        }
    }
    return best;
}

// The largest value and the index of its first occurrence; (0, 0) for none
template <typename T>
std::pair<T, qsizetype> maxOrZeroAndIndex(const QList<T>& values) {
    if (values.isEmpty()) {
        return {T{}, 0};
    }
    qsizetype index = 0;
    for (qsizetype i = 1; i < values.size(); ++i) {
        if (values[i] > values[index]) {
            index = i;
        }
    }
    return {values[index], index};
}

// Hold-last-value smoothing, the same live and in the synopsis' replay: a 0 reading (cadence under 10 RPM, speed
// under 5 MPH) is taken as a possible misread and shows the last good value instead, up to this many in a row
constexpr int kHoldPowerReadings = 3;
constexpr int kHoldHrReadings = 10;
constexpr int kHoldCadenceReadings = 30;
constexpr int kHoldSpeedReadings = 3;

// Sensor dropouts on the graphs: a gap up to kDropoutZeroSeconds is drawn as a straight line; a longer one drops to
// zero kDropoutEdgeSeconds (about one ANT+ message period) after the last reading until the sensor is heard again
constexpr double kDropoutZeroSeconds = 5.0;
constexpr double kDropoutEdgeSeconds = 0.25;

// A plot-only copy of a sensor series with long gaps filled with zeros. endTime is now for a live graph (or the
// end of the ride): a sensor silent that long at the end also shows zero
std::pair<QList<double>, QList<double>> withDropoutsAsZero(const QList<double>& times, const QList<double>& values,
                                                           std::optional<double> endTime = std::nullopt);

// The devices picked for the workout, one "<device type> <device number>" each, kept for the life of the app
// (cleared by Disconnect and Rescan): only well formed, known, unique entries are ever stored
namespace selectedDevices {
extern const QStringList kSelectableTypes;
QStringList read();
void write(const QStringList& lines);
void clear();
}  // namespace selectedDevices

}  // namespace awo
