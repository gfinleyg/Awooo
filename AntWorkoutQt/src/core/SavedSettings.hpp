#pragma once

// The one settings file kept between runs, savedSettingsANT.json, in the working folder (beside the exe), shared with
// the Python app (SavedSettings.py):
//
//   "wheelCircumference": 2.096       meters
//   "ftp": 250                        watts
//   "maxHr": 185                      BPM
//   "maxWatts": 1200                  top of the free mode FE-C watts slider
//   "powerDriveCutoff": 300           workout mode FE-C trainer is released above this requested power
//   "musicVolume": 100                Musical Cadence Mode's volume, 0-100%
//   "themeFile", "themePartFiles", "randomTheme", "randomThemeLast"    the theme (see the UI's Theme)
//   "weightUnit", "powerWeightUnit", "distanceUnit", "riderWeightKg"  see Units
//   "gaugeFps"                        see Performance
//   "powerCalibration": {"11 12345": {...}}                           see PowerCalibration
//
// Any key can be missing (nothing saved yet). The older savedStatsANT.txt ("wheelC,ftp,maxHr") and
// powerCalibrationANT.txt are folded in and removed the first time it's loaded without a settings file.

#include <QJsonObject>
#include <QString>

namespace awo::settings {

extern const QString kSettingsFile;
extern const QString kLegacyStatsFile;
extern const QString kLegacyCalibrationFile;

// The whole settings object; empty when nothing is saved or the file is unreadable
QJsonObject load();
// Merges changes into the saved settings (other keys are kept); false if the file couldn't be written
bool update(const QJsonObject& changes);

}  // namespace awo::settings
