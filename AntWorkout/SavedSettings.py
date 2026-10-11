"""
SavedSettings.py

The one settings file AntWorkout keeps between runs, savedSettingsANT.json:

  {
    "wheelCircumference": 2.096,      - meters
    "ftp": 250,                       - watts
    "maxHr": 185,                     - BPM
    "maxWatts": 1200,                 - top of the free mode FE-C Watts slider
    "powerDriveCutoff": 300,          - workout mode FE-C trainer is released above this requested power
    "musicVolume": 100,               - Musical Cadence Mode's Volume slider, 0-100%
    "themeFile": "C:/.../theme.json", - font/color theme from the settings menu (see AppTheme.py), null for the default theme
    "themePartFiles": {               - colors/gradients files loaded on top of it from the settings menu
      "colors": "C:/.../colors.json", "ftpColors": "C:/.../gradients.json"
    },
    "powerCalibration": {             - per power meter, see PowerCalibration.py
      "11 12345": {"offset_watts": ..., "scale": ..., ...}
    }
  }

Any key can be missing (nothing saved yet). Replaces the older
savedStatsANT.txt ("wheelC,ftp,maxHr") and powerCalibrationANT.txt (JSON
calibration only) - both are folded into this file and removed the first
time it's loaded without an existing savedSettingsANT.json.
"""

import json
import os

SETTINGS_FILE = "savedSettingsANT.json"
LEGACY_STATS_FILE = "savedStatsANT.txt"
LEGACY_CALIBRATION_FILE = "powerCalibrationANT.txt"


def load():
    """Whole settings dict; {} when nothing is saved or the file is unreadable"""
    if not os.path.exists(SETTINGS_FILE):
        _migrate_legacy_files()
    try:
        with open(SETTINGS_FILE, "r") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def update(**changes):
    """Merge changes into the saved settings (other keys are kept); False if the file couldn't be written"""
    data = load()
    data.update(changes)
    return _write(data)


def _write(data):
    #written to a temp file and swapped in, so a crash mid-write can't leave a half written settings file
    tempFile = SETTINGS_FILE + ".tmp"
    try:
        with open(tempFile, "w") as f:
            json.dump(data, f, indent=2)
        os.replace(tempFile, SETTINGS_FILE)
        return True
    except OSError:
        return False


def _migrate_legacy_files():
    data = {}
    try:
        with open(LEGACY_STATS_FILE) as f:
            stats = f.read().strip().split(",")
        if len(stats) == 3:
            try:
                data["wheelCircumference"] = float(stats[0])
                data["ftp"] = int(stats[1])
                data["maxHr"] = int(stats[2])
            except ValueError:
                pass
    except OSError:
        pass
    try:
        with open(LEGACY_CALIBRATION_FILE) as f:
            calibration = json.load(f)
        if isinstance(calibration, dict) and calibration:
            data["powerCalibration"] = calibration
    except (OSError, ValueError):
        pass
    if data and _write(data):
        for legacyFile in (LEGACY_STATS_FILE, LEGACY_CALIBRATION_FILE):
            try:
                os.remove(legacyFile)
            except OSError:
                pass
