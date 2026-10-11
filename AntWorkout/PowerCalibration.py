"""
PowerCalibration.py

Saved per-power-meter calibration, written by CalibratePowerWindow and
applied to every power reading by AntDevicesPoller.

Stored under "powerCalibration" in savedSettingsANT.json (see
SavedSettings.py), keyed by "<device_type> <device_number>" so each meter
keeps its own settings:

  offset_watts   - the meter's average reading at rest; subtracted from
                   every reading
  scale          - reference watts / measured watts from the optional
                   accuracy check; readings are multiplied by this
  ant_zero_offset - the raw offset the meter itself reported for its last
                   successful ANT+ manual zero offset calibration (for
                   the record only - the meter applies that one itself)
  updated        - when these settings were saved
"""

import datetime

import SavedSettings

#Accepted range for the accuracy check's scale factor; anything outside this
#is far more likely a bad reference/ride than a real meter error
MIN_SCALE = 0.75
MAX_SCALE = 1.25


def _key(device_type, device_number):
    return f"{device_type} {device_number}"


def load_all():
    data = SavedSettings.load().get("powerCalibration")
    return data if isinstance(data, dict) else {}


def get(device_type, device_number):
    return load_all().get(_key(device_type, device_number))


def save(device_type, device_number, offset_watts=0.0, scale=1.0, ant_zero_offset=None):
    """False if the settings file couldn't be written"""
    data = load_all()
    data[_key(device_type, device_number)] = {
        "offset_watts": round(float(offset_watts), 1),
        "scale": round(float(scale), 4),
        "ant_zero_offset": ant_zero_offset,
        "updated": datetime.datetime.now().isoformat(timespec="seconds"),
    }
    return SavedSettings.update(powerCalibration=data)


def clear(device_type, device_number):
    """False if the settings file couldn't be written"""
    data = load_all()
    if data.pop(_key(device_type, device_number), None) is not None:
        return SavedSettings.update(powerCalibration=data)
    return True


def apply(watts, calibration):
    """Calibrated watts for a raw reading; 0 (coasting) stays 0"""
    if not calibration or watts <= 0:
        return watts
    corrected = (watts - calibration.get("offset_watts", 0.0)) * calibration.get("scale", 1.0)
    return max(0, int(round(corrected)))


def describe(calibration):
    """One-line human readable summary of saved settings"""
    if not calibration:
        return "No calibration saved"
    text = f"Offset {calibration.get('offset_watts', 0):g} W, scale x{calibration.get('scale', 1.0):.3f}"
    if calibration.get("updated"):
        text += f" (saved {calibration['updated'].replace('T', ' ')})"
    return text
