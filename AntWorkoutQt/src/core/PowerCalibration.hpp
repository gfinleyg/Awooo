#pragma once

// Saved per power meter calibration (PowerCalibration.py), written by Calibrate Power and applied to every power
// reading. Kept under "powerCalibration" in savedSettingsANT.json, keyed "<device type> <device number>":
//   offset_watts     the meter's average reading at rest; subtracted from every reading
//   scale            reference watts / measured watts from the optional accuracy check; readings are multiplied by it
//   ant_zero_offset  the raw offset the meter reported for its last ANT+ zero offset calibration (for the record:
//                    the meter applies that one itself)
//   updated          when they were saved

#include <QHash>
#include <QString>

#include <optional>

namespace awo::calibration {

// accepted range for the accuracy check's scale: outside it is far more likely a bad reference ride than the meter
constexpr double kMinScale = 0.75;
constexpr double kMaxScale = 1.25;

struct Calibration {
    double offsetWatts = 0;
    double scale = 1;
    std::optional<int> antZeroOffset;
    QString updated;
};

QString key(int deviceType, int deviceNumber);
QHash<QString, Calibration> loadAll();
std::optional<Calibration> get(int deviceType, int deviceNumber);
// false if the settings file couldn't be written
bool save(int deviceType, int deviceNumber, double offsetWatts = 0, double scale = 1,
          std::optional<int> antZeroOffset = std::nullopt);
bool clear(int deviceType, int deviceNumber);
// calibrated watts for a raw reading; 0 (coasting) stays 0
int apply(int watts, const std::optional<Calibration>& calibration);
// a one line summary
QString describe(const std::optional<Calibration>& calibration);

}  // namespace awo::calibration
