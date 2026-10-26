#include "WorkoutData.hpp"

#include <chrono>

namespace awo {

double secondsNow() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() / 1e6;
}

void SensorHistory::append(std::initializer_list<double> values) {
    appendAt(secondsNow(), values);
}

void SensorHistory::appendAt(double time, std::initializer_list<double> values) {
    times_.append(time);
    auto value = values.begin();
    for (QList<int>& column : columns_) {
        column.append(value != values.end() ? static_cast<int>(*value++) : 0);
    }
}

void SensorHistory::clear() {
    times_.clear();
    for (QList<int>& column : columns_) {
        column.clear();
    }
}

void WorkoutData::clear() {
    for (SensorHistory* history : {&power, &heartRate, &speedCadence, &cadence, &speed, &fallbackCadence}) {
        history->clear();
    }
}

WorkoutData& workoutData() {
    static WorkoutData data;
    return data;
}

}  // namespace awo
