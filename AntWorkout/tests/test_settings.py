"""SavedSettings, Units, Performance and PowerCalibration: everything kept in savedSettingsANT.json"""

import json
import os

import pytest

import Performance
import PowerCalibration
import SavedSettings
import Units


def read_settings():
    with open(SavedSettings.SETTINGS_FILE) as f:
        return json.load(f)


###SavedSettings
def test_load_with_nothing_saved():
    assert SavedSettings.load() == {}


def test_update_merges_and_keeps_other_keys():
    assert SavedSettings.update(ftp=250, maxHr=185)
    assert SavedSettings.update(ftp=260)
    assert SavedSettings.load() == {"ftp": 260, "maxHr": 185}
    assert not os.path.exists(SavedSettings.SETTINGS_FILE + ".tmp")


@pytest.mark.parametrize("contents", ["not json", "[1, 2]", ""])
def test_unreadable_or_wrong_shape_file_loads_empty(contents):
    with open(SavedSettings.SETTINGS_FILE, "w") as f:
        f.write(contents)
    assert SavedSettings.load() == {}


def test_update_reports_a_failed_write(monkeypatch):
    def failing_replace(src, dst):
        raise OSError("disk full")
    monkeypatch.setattr(os, "replace", failing_replace)
    assert SavedSettings.update(ftp=1) is False


def test_legacy_files_are_migrated_and_removed():
    with open(SavedSettings.LEGACY_STATS_FILE, "w") as f:
        f.write("2.096,250,185\n")
    with open(SavedSettings.LEGACY_CALIBRATION_FILE, "w") as f:
        json.dump({"11 5": {"offset_watts": 3.0, "scale": 1.0}}, f)
    assert SavedSettings.load() == {"wheelCircumference": 2.096, "ftp": 250, "maxHr": 185,
                                    "powerCalibration": {"11 5": {"offset_watts": 3.0, "scale": 1.0}}}
    assert not os.path.exists(SavedSettings.LEGACY_STATS_FILE)
    assert not os.path.exists(SavedSettings.LEGACY_CALIBRATION_FILE)


def test_bad_legacy_stats_are_skipped():
    with open(SavedSettings.LEGACY_STATS_FILE, "w") as f:
        f.write("abc,250,185")
    #nothing usable: no settings file written, the legacy file left alone
    assert SavedSettings.load() == {}
    assert os.path.exists(SavedSettings.LEGACY_STATS_FILE)
    assert not os.path.exists(SavedSettings.SETTINGS_FILE)


def test_legacy_files_ignored_once_settings_exist():
    SavedSettings.update(ftp=300)
    with open(SavedSettings.LEGACY_STATS_FILE, "w") as f:
        f.write("2.096,250,185")
    assert SavedSettings.load() == {"ftp": 300}


###Units
def test_units_defaults_and_saved_values_load():
    Units.load()
    assert (Units.current.weight, Units.current.powerWeight, Units.current.distance) == ("lbs", "W/kg", "mi")
    SavedSettings.update(weightUnit="kg", powerWeightUnit="W/lb", distanceUnit="km", riderWeightKg=70)
    Units.load()
    assert (Units.current.weight, Units.current.powerWeight, Units.current.distance) == ("kg", "W/lb", "km")
    assert Units.current.riderWeightKg == 70.0


@pytest.mark.parametrize("saved", [
    {"weightUnit": "stone", "distanceUnit": "furlongs", "powerWeightUnit": "hp"},
    {"riderWeightKg": 5}, {"riderWeightKg": 500}, {"riderWeightKg": "70"},
])
def test_units_ignore_unknown_saved_values(saved):
    SavedSettings.update(**saved)
    Units.load()
    assert (Units.current.weight, Units.current.powerWeight, Units.current.distance) == ("lbs", "W/kg", "mi")
    assert Units.current.riderWeightKg == 0.0


def test_set_units_saves_and_ignores_unknown():
    Units.set_units(weight="kg", distance="km", powerWeight="bogus")
    assert read_settings() == {"weightUnit": "kg", "powerWeightUnit": "W/kg", "distanceUnit": "km"}


def test_weight_conversions():
    assert Units.weight_text(70) == "154.3 lbs"
    assert Units.format_weight(Units.weight_to_kg(165)) == "165"
    low, high = Units.weight_range()
    assert low == pytest.approx(20*Units.LBS_PER_KG) and high == pytest.approx(300*Units.LBS_PER_KG)
    Units.set_units(weight="kg")
    assert Units.weight_text(70) == "70 kg"
    assert Units.weight_text(70.26) == "70.3 kg"
    assert Units.weight_to_kg(70) == 70


def test_power_to_weight():
    assert Units.power_to_weight(300) is None
    assert Units.power_to_weight_text(300) == ""
    Units.set_rider_weight_kg(75)
    assert read_settings()["riderWeightKg"] == 75
    assert Units.power_to_weight(300) == pytest.approx(4.0)
    assert Units.power_to_weight_text(300) == " (4.00 W/kg)"
    assert Units.power_to_weight(None) is None
    Units.set_units(powerWeight="W/lb")
    assert Units.power_to_weight(300) == pytest.approx(300/(75*Units.LBS_PER_KG))
    assert Units.power_to_weight_text(300, prefix="", suffix="").endswith("W/lb")


def test_speed_and_distance():
    assert (Units.speed(10), Units.distance(10), Units.speed_unit(), Units.distance_unit(),
            Units.distance_unit(False)) == (10, 10, "MPH", "Miles", "mi")
    Units.set_units(distance="km")
    assert Units.speed(10) == pytest.approx(16.09344)
    assert Units.distance(1) == pytest.approx(1.609344)
    assert (Units.speed_unit(), Units.distance_unit(), Units.distance_unit(False)) == ("KM/H", "Km", "km")


class Watcher:
    def __init__(self):
        self.calls = 0

    def changed(self):
        self.calls += 1


class DeadWatcher:
    def changed(self):
        raise RuntimeError("wrapped C/C++ object has been deleted")


@pytest.mark.parametrize("module, change", [
    (Units, lambda: Units.set_units(distance="km")),
    (Performance, lambda: Performance.set_gauge_fps(30)),
])
def test_watchers_called_and_dropped(module, change):
    watcher, dead, gone = Watcher(), DeadWatcher(), Watcher()
    for obj in (watcher, dead, gone):
        module.watch(obj, "changed")
    del gone, obj
    change()
    assert watcher.calls == 1
    #the deleted one and the one whose Qt side is gone are dropped
    assert len(module._watchers) == 1


###Performance
def test_gauge_fps_default_saved_and_invalid():
    Performance._gaugeFps = None
    assert Performance.gauge_fps() == Performance.DEFAULT_GAUGE_FPS
    Performance._gaugeFps = None
    SavedSettings.update(gaugeFps=17)
    assert Performance.gauge_fps() == Performance.DEFAULT_GAUGE_FPS
    Performance._gaugeFps = None
    SavedSettings.update(gaugeFps=20)
    assert Performance.gauge_fps() == 20


def test_set_gauge_fps():
    Performance._gaugeFps = None
    watcher = Watcher()
    Performance.watch(watcher, "changed")
    Performance.set_gauge_fps(25)  #not a choice
    Performance.set_gauge_fps(Performance.DEFAULT_GAUGE_FPS)  #no change
    assert watcher.calls == 0
    Performance.set_gauge_fps(0)
    assert watcher.calls == 1 and Performance.gauge_fps() == 0 and read_settings()["gaugeFps"] == 0


def test_gauge_fps_text():
    assert Performance.gauge_fps_text(0).startswith("Off")
    assert Performance.gauge_fps_text(60) == "60 fps - smoothest"
    assert Performance.gauge_fps_text(30) == "30 fps"


###PowerCalibration
def test_calibration_save_get_clear():
    assert PowerCalibration.get(11, 5) is None
    assert PowerCalibration.save(11, 5, offset_watts=4.26, scale=1.023456, ant_zero_offset=-12)
    PowerCalibration.save(11, 6, offset_watts=1)
    saved = PowerCalibration.get(11, 5)
    assert saved["offset_watts"] == 4.3 and saved["scale"] == 1.0235 and saved["ant_zero_offset"] == -12
    assert "T" in saved["updated"]
    assert PowerCalibration.clear(11, 5)
    assert PowerCalibration.get(11, 5) is None and PowerCalibration.get(11, 6) is not None
    #clearing what isn't there is fine
    assert PowerCalibration.clear(11, 5)


def test_calibration_ignores_a_bad_saved_value():
    SavedSettings.update(powerCalibration=["not", "a", "dict"])
    assert PowerCalibration.load_all() == {}


@pytest.mark.parametrize("watts, calibration, result", [
    (200, None, 200),
    (200, {}, 200),
    (0, {"offset_watts": 5, "scale": 1.1}, 0),         #coasting stays 0
    (200, {"offset_watts": 5, "scale": 1.0}, 195),
    (200, {"offset_watts": 0, "scale": 1.1}, 220),
    (200, {"offset_watts": 5, "scale": 1.1}, 215),      #(200 - 5) x 1.1
    (3, {"offset_watts": 5, "scale": 1.0}, 0),          #never negative
])
def test_calibration_apply(watts, calibration, result):
    assert PowerCalibration.apply(watts, calibration) == result


def test_calibration_describe():
    assert PowerCalibration.describe(None) == "No calibration saved"
    text = PowerCalibration.describe({"offset_watts": 4.5, "scale": 1.0, "updated": "2026-10-12T10:00:00"})
    assert text == "Offset 4.5 W, scale x1.000 (saved 2026-10-12 10:00:00)"
