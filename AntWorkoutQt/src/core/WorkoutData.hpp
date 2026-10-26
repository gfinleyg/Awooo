#pragma once

// The workout's full sensor history (WorkoutData.py), recorded as each reading arrives and replayed by the synopsis
// once the workout ends. Timestamps are seconds since the epoch, the same clock as AppState's startTimestamp.

#include <QList>

#include <initializer_list>

namespace awo {

class SensorHistory {
public:
    explicit SensorHistory(int columns = 1) : columns_(columns) {}

    // a reading now (each value cut to a whole number, as the Python app stored them)
    void append(std::initializer_list<double> values);
    // a reading at a time (seconds since the epoch)
    void appendAt(double time, std::initializer_list<double> values);

    const QList<double>& times() const { return times_; }
    const QList<int>& column(int index = 0) const { return columns_[index]; }
    int columnCount() const { return static_cast<int>(columns_.size()); }
    qsizetype size() const { return times_.size(); }
    bool isEmpty() const { return times_.isEmpty(); }
    void clear();

private:
    QList<double> times_;
    QList<QList<int>> columns_;
};

// Seconds since the epoch, to the microsecond (Python's time.time())
double secondsNow();

struct WorkoutData {
    SensorHistory power{1};            // calibrated watts
    SensorHistory heartRate{1};        // BPM
    // combined (121) sensor raw counters: cumulative speed revolutions, speed event time, cumulative cadence
    // revolutions, cadence event time (event times in 1/1024 s)
    SensorHistory speedCadence{4};
    // discrete cadence (122) / speed (123) sensor raw counters: cumulative revolutions, event time (1/1024 s)
    SensorHistory cadence{2};
    SensorHistory speed{2};
    // RPM from the power meter's own cadence field, whenever the cadence gauge fell back to it (no dedicated cadence
    // sensor, or it went stale/lost/stalled)
    SensorHistory fallbackCadence{1};

    void clear();
};

// The ride being ridden's
WorkoutData& workoutData();

}  // namespace awo
