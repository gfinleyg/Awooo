"""Every ANT+ page of the six sensor types, decoded (ant.pages / ant_native.decode_page / ant_decode_page), the
control pages built for sending, and the profiles keeping each page they receive. Expected values come from the
Fitness Equipment (Rev 5.0) and Bike Speed and Cadence (Rev 2.1) profiles' page tables; the heart rate pages 5 and
6, power pages and common pages from the layouts ant_pages.cpp documents."""
import ctypes
import random

import pytest

import ant_native
import antnative_ctypes as c
from ant import pages
from ant.device import ANTDevice
from ant.profiles import FitnessEquipmentProfile, HeartRateProfile, PowerMeterProfile, SpeedCadenceProfile

HR, POWER, FE, COMBINED, CADENCE, SPEED = 120, 11, 17, 121, 122, 123


def page(device_type, payload):
    decoded = pages.decode_page(device_type, bytes(payload))
    assert decoded is not None
    return decoded


def fields(decoded):
    return {k: v for k, v in decoded.items() if k not in ("page", "page_name")}


###Fitness equipment (FE Rev 5.0 section 8)

def test_fe_general_data():
    # trainer, 10 s, 200 m, 5 m/s, 140 bpm from an ANT+ HRM, distance enabled, IN_USE
    d = page(FE, [0x10, 25, 40, 200, 0x88, 0x13, 140, 0x35])
    assert d["page_name"] == "general_fe_data"
    assert fields(d) == {"equipment_type": 25, "elapsed_time_s": 10.0, "distance_m": 200, "speed_mps": 5.0,
                         "heart_rate": 140, "heart_rate_source": 1, "virtual_speed": False, "fe_state": 3,
                         "lap_toggle": False}


def test_fe_general_data_without_optional_fields():
    d = page(FE, [0x10, 22, 0, 9, 0xFF, 0xFF, 0xFF, 0x28 | 0x80])
    assert "distance_m" not in d and "speed_mps" not in d and "heart_rate" not in d
    assert d["virtual_speed"] and d["fe_state"] == 2 and d["lap_toggle"]


def test_fe_trainer_data():
    # cadence 90, 10000 W accumulated, 250 W, user configuration required, speed too low for the target
    d = page(FE, [0x19, 7, 90, 0x10, 0x27, 250, 0x40, 0x31])
    assert fields(d) == {"update_event_count": 7, "instantaneous_cadence": 90, "accumulated_power": 10000,
                         "instantaneous_power": 250, "trainer_status": 4,
                         "bicycle_power_calibration_required": False, "resistance_calibration_required": False,
                         "user_configuration_required": True, "target_power_limits": 1, "fe_state": 3,
                         "lap_toggle": False}


def test_fe_trainer_invalid_power():
    d = page(FE, [0x19, 7, 0xFF, 0, 0, 0xFF, 0x0F, 0x20])
    assert "instantaneous_power" not in d and "accumulated_power" not in d and "instantaneous_cadence" not in d


def test_fe_settings_and_metabolic():
    d = page(FE, [0x11, 0xFF, 0xFF, 210, 0xA2, 0xFE, 100, 0x30])
    assert d["cycle_length_m"] == pytest.approx(2.10) and d["incline_percent"] == pytest.approx(-3.5)
    assert d["resistance_level_percent"] == 50.0
    d = page(FE, [0x12, 0xFF, 0x52, 0x03, 0x70, 0x17, 77, 0x31])
    assert d["mets"] == pytest.approx(8.5) and d["caloric_burn_rate_kcal_per_hr"] == pytest.approx(600.0)
    assert d["calories"] == 77


@pytest.mark.parametrize("payload, expected", [
    ([0x13, 0xFF, 0xFF, 0xFF, 160, 12, 30, 0x33],
     {"cadence": 160, "negative_vertical_distance_m": -1.2, "positive_vertical_distance_m": 3.0}),
    ([0x14, 0xFF, 25, 140, 60, 0xC8, 0x00, 0x33],
     {"positive_vertical_distance_m": 2.5, "stride_count": 140, "cadence": 60, "instantaneous_power": 200}),
    ([0x16, 0xFF, 0xFF, 33, 28, 0xB4, 0x00, 0x31], {"stroke_count": 33, "cadence": 28, "instantaneous_power": 180}),
    ([0x17, 0xFF, 0xFF, 12, 40, 0x96, 0x00, 0x31], {"stride_cycles": 12, "cadence": 40, "instantaneous_power": 150}),
    ([0x18, 0xFF, 0xFF, 50, 70, 0xFF, 0xFF, 0x30], {"cadence": 70}),
])
def test_fe_equipment_pages(payload, expected):
    d = page(FE, payload)
    for name, value in expected.items():
        assert d[name] == pytest.approx(value), name
    if payload[0] == 0x18:
        assert "stride_count" not in d and "instantaneous_power" not in d


def test_fe_trainer_torque_and_calibration():
    d = page(FE, [0x1A, 3, 200, 0x00, 0x08, 0x40, 0x01, 0x30])
    assert (d["wheel_ticks"], d["wheel_period"], d["accumulated_torque"]) == (200, 2048, 320)
    d = page(FE, [0x02, 0x80, 0x60, 0x5A, 0x8D, 0x20, 0xFF, 0xFF])
    assert d["spin_down_calibration_pending"] and not d["zero_offset_calibration_pending"]
    assert (d["temperature_condition"], d["speed_condition"]) == (2, 1)
    assert d["temperature_c"] == 20.0 and d["target_speed_mps"] == pytest.approx(8.333)
    assert "target_spin_down_time_ms" not in d
    d = page(FE, [0x01, 0xC0, 0, 0xFF, 0x34, 0x12, 0xD0, 0x07])
    assert d["zero_offset_calibration_success"] and d["spin_down_calibration_success"]
    assert (d["zero_offset"], d["spin_down_time_ms"]) == (0x1234, 2000) and "temperature_c" not in d


def test_fe_capabilities_and_command_status():
    d = page(FE, [0x36, 0xFF, 0xFF, 0xFF, 0xFF, 0xE8, 0x03, 0b111])
    assert d["maximum_resistance_n"] == 1000
    assert d["supports_basic_resistance"] and d["supports_target_power"] and d["supports_simulation"]
    # the trainer confirming target power 250 W
    d = page(FE, [0x47, 0x31, 5, 0, 0xFF, 0xFF, 0xE8, 0x03])
    assert (d["last_command_id"], d["sequence_number"], d["command_status"]) == (0x31, 5, 0)
    assert d["target_power_w"] == 250.0


###Control pages a display sends: built, then decoded back

def test_track_resistance_round_trip():
    payload = pages.track_resistance(5.0, 0.004)
    assert payload == bytes([0x33, 0xFF, 0xFF, 0xFF, 0xFF, 0x14, 0x50, 80])
    d = page(FE, payload)
    assert d["grade_percent"] == pytest.approx(5.0) and d["rolling_resistance_coefficient"] == pytest.approx(0.004)
    # defaults left for the trainer: flat, asphalt
    assert pages.track_resistance() == bytes([0x33, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF])
    # -200..200% clamped
    assert page(FE, pages.track_resistance(-250))["grade_percent"] == pytest.approx(-200.0)


def test_wind_resistance_round_trip():
    d = page(FE, pages.wind_resistance(0.51, -10, 0.9))
    assert d["wind_resistance_coefficient"] == pytest.approx(0.51)
    assert d["wind_speed_kmh"] == -10 and d["drafting_factor"] == pytest.approx(0.9)


def test_user_configuration_round_trip():
    payload = pages.user_configuration(75.5, 9.0, 0.70, 1.5, 3)
    d = page(FE, payload)
    assert d["user_weight_kg"] == pytest.approx(75.5) and d["bicycle_weight_kg"] == pytest.approx(9.0)
    assert d["wheel_diameter_m"] == pytest.approx(0.70) and d["gear_ratio"] == pytest.approx(1.5)
    assert d["wheel_diameter_offset_mm"] == 3
    assert fields(page(FE, pages.user_configuration())) == {}


def test_request_page():
    assert pages.request_page(0x36, times=2) == bytes([0x46, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x36, 0x01])
    assert pages.request_page(0x47, acknowledged=True)[5] == 0x81
    assert pages.power_manual_zero() == bytes([0x01, 0xAA] + [0xFF] * 6)


###Bike speed and cadence (BSC Rev 2.1 sections 5.3 and 6.3)

@pytest.mark.parametrize("device_type", [SPEED, CADENCE])
def test_bsc_background_pages(device_type):
    event = [0x00, 0x04, 0x10, 0x00]       # bytes 4-7, the same on every page
    assert page(device_type, [0x81, 0x10, 0x27, 0x00] + event)["cumulative_operating_time_s"] == 20000
    d = page(device_type, [0x02, 0x20, 0x34, 0x12] + event)
    assert (d["page"], d["manufacturer_id"], d["serial_number_upper"]) == (2, 32, 0x1234)
    d = page(device_type, [0x83, 2, 7, 11] + event)
    assert (d["hardware_version"], d["software_version"], d["model_number"]) == (2, 7, 11)
    d = page(device_type, [0x04, 0xFF, 128, 0x23] + event)
    assert d["battery_voltage"] == pytest.approx(3.5) and d["battery_status"] == 2
    assert page(device_type, [0x05, 0x01, 0xFF, 0xFF] + event)["stopped"] is True
    assert page(device_type, [0x85, 0x00, 0xFF, 0xFF] + event)["stopped"] is False


def test_combined_sensor_has_no_pages():
    assert pages.decode_page(COMBINED, bytes(8)) is None


###Heart rate

def test_heart_rate_pages():
    beat = [0x00, 0x04, 9, 70]
    d = page(HR, [0x85, 120, 150, 110] + beat)
    assert (d["interval_average_heart_rate"], d["interval_maximum_heart_rate"],
            d["session_average_heart_rate"]) == (120, 150, 110)
    d = page(HR, [0x06, 0xFF, 0x07, 0x01] + beat)
    assert (d["features_supported"], d["features_enabled"]) == (7, 1)
    assert d["extended_running_features"] and not d["extended_cycling_features"]
    d = page(HR, [0x07, 55, 0, 0x23] + beat)
    assert (d["battery_level_percent"], d["battery_status"]) == (55, 2) and d["battery_voltage"] == 3.0
    d = page(HR, [0x04, 0xAB, 0x00, 0x02] + beat)
    assert d["previous_heart_beat_event_time"] == 0.5 and d["manufacturer_specific"] == 0xAB
    assert page(HR, [0x82, 1, 0x34, 0x12] + beat)["manufacturer_id"] == 1


###Bicycle power

def test_power_only_balance_and_accumulated_power():
    d = page(POWER, [0x10, 3, 0x80 | 52, 90, 0x20, 0x4E, 0xFA, 0x00])
    assert fields(d) == {"event_count": 3, "pedal_power_percent": 52, "pedal_power_right": True,
                         "instantaneous_cadence": 90, "accumulated_power": 20000, "instantaneous_power": 250}
    d = page(POWER, [0x10, 3, 0xFF, 0xFF, 0, 0, 0, 0])
    assert "pedal_power_percent" not in d and "instantaneous_cadence" not in d


def test_power_torque_pages():
    d = page(POWER, [0x12, 9, 40, 92, 0x00, 0x08, 0x40, 0x01])
    assert (d["crank_ticks"], d["instantaneous_cadence"], d["crank_period"], d["accumulated_torque"]) == \
        (40, 92, 2048, 320)
    d = page(POWER, [0x11, 9, 15, 0xFF, 0x00, 0x04, 0x20, 0x00])
    assert (d["wheel_ticks"], d["wheel_period"], d["accumulated_torque"]) == (15, 1024, 32)
    assert "instantaneous_cadence" not in d


def test_torque_effectiveness_and_pedal_smoothness():
    d = page(POWER, [0x13, 9, 150, 140, 50, 0xFE, 0xFF, 0xFF])
    assert (d["left_torque_effectiveness"], d["right_torque_effectiveness"]) == (75.0, 70.0)
    assert d["combined_pedal_smoothness"] == 25.0 and "left_pedal_smoothness" not in d
    d = page(POWER, [0x13, 9, 0xFF, 0xFF, 40, 44, 0xFF, 0xFF])
    assert (d["left_pedal_smoothness"], d["right_pedal_smoothness"]) == (20.0, 22.0)
    assert "left_torque_effectiveness" not in d


def test_crank_torque_frequency_is_big_endian():
    d = page(POWER, [0x20, 4, 0x01, 0x2C, 0x07, 0xD0, 0x00, 0x64])
    assert (d["slope"], d["timestamp"], d["torque_ticks_stamp"]) == (30.0, 2000, 100)


def test_power_calibration_response():
    d = page(POWER, [0x01, 0xAC, 0x01, 0xFF, 0xFF, 0xFF, 0x9C, 0xFF])
    assert d["calibration_success"] and d["auto_zero_status"] == 1 and d["calibration_data"] == -100
    d = page(POWER, [0x01, 0xAF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00])
    assert not d["calibration_success"] and "auto_zero_status" not in d


###Common pages

@pytest.mark.parametrize("device_type", [POWER, FE, HR, SPEED])
def test_common_pages(device_type):
    d = page(device_type, [0x50, 0xFF, 0xFF, 3, 0x01, 0x00, 0xE8, 0x03])
    assert (d["page_name"], d["hardware_revision"], d["manufacturer_id"], d["model_number"]) == \
        ("manufacturer_information", 3, 1, 1000)
    d = page(device_type, [0x51, 0xFF, 0xFF, 42, 0x78, 0x56, 0x34, 0x12])
    assert (d["software_revision_main"], d["serial_number"]) == (42, 0x12345678)
    assert "software_revision_supplemental" not in d
    d = page(device_type, [0x52, 0xFF, 0x12, 0x10, 0x27, 0x00, 64, 0xB3])
    assert (d["number_of_batteries"], d["battery_id"], d["cumulative_operating_time_s"]) == (2, 1, 20000)
    assert (d["battery_voltage"], d["battery_status"]) == (3.25, 3)


def test_unknown_page():
    d = page(POWER, [0xE2, 1, 2, 3, 4, 5, 6, 7])
    assert d == {"page": 0xE2, "page_name": "unknown"}


###The profiles keep every page

def extended(payload, device_number, device_type):
    return 0x4E, bytes([0]) + bytes(payload) + bytes([0x80, device_number & 0xFF, device_number >> 8, device_type, 1])


def received(device_type, *payloads):
    return [ant_native._decode_message(*extended(p, 7, device_type)) for p in payloads]


def test_messages_carry_their_page():
    message = received(FE, [0x10, 25, 40, 200, 0x88, 0x13, 140, 0x35])[0]
    assert message["page_data"]["speed_mps"] == 5.0


def test_profile_keeps_each_page():
    profile = FitnessEquipmentProfile()
    for message in received(FE, [0x10, 25, 40, 200, 0x88, 0x13, 140, 0x35], [0x19, 7, 90, 0, 0, 250, 0, 0x30],
                            [0x50, 0xFF, 0xFF, 3, 0x01, 0x00, 0xE8, 0x03]):
        profile.on_message(message)
    assert set(profile.pages) == {0x10, 0x19, 0x50}
    assert profile.pages[0x10]["distance_m"] == 200 and profile.pages[0x50]["model_number"] == 1000
    assert profile.latest.instantaneous_power_watts == 250


def test_device_pages():
    device = ANTDevice(7, POWER, 1, 0, PowerMeterProfile())
    for message in received(POWER, [0x13, 1, 150, 140, 50, 0xFE, 0xFF, 0xFF], [0x10, 1, 0x80 | 50, 90, 0, 0, 200, 0]):
        device.update(message)
    assert device.pages[0x13]["combined_pedal_smoothness"] == 25.0
    assert device.pages[0x10]["pedal_power_right"] is True and device.latest.instantaneous_power_watts == 200


def test_heart_rate_and_speed_profiles_keep_pages():
    hr = HeartRateProfile()
    for message in received(HR, [0x07, 80, 0, 0x23, 0, 4, 1, 60], [0x02, 1, 0x34, 0x12, 0, 8, 2, 61]):
        hr.on_message(message)
    assert hr.pages[7]["battery_level_percent"] == 80 and hr.pages[2]["serial_number_upper"] == 0x1234
    assert hr.latest.computed_heart_rate == 61
    speed = SpeedCadenceProfile(SPEED)
    speed.on_message(received(SPEED, [0x05, 0x01, 0xFF, 0xFF, 0, 4, 10, 0])[0])
    assert speed.pages[5]["stopped"] is True


###Held rates (what the C API's sensor snapshots show)

def combined(cadence_ticks, cadence_revs, speed_ticks, speed_revs):
    payload = [cadence_ticks & 0xFF, cadence_ticks >> 8, cadence_revs & 0xFF, cadence_revs >> 8,
               speed_ticks & 0xFF, speed_ticks >> 8, speed_revs & 0xFF, speed_revs >> 8]
    return received(COMBINED, payload)[0]


def test_held_rates_cover_repeated_events_then_drop_when_stopped():
    profile = SpeedCadenceProfile(COMBINED, wheel_circumference_m=2.0)
    assert profile.held_cadence_rpm is None
    profile.on_message(combined(0, 0, 0, 0))
    profile.on_message(combined(1024, 1, 512, 2))
    assert profile.held_cadence_rpm == pytest.approx(60) and profile.held_speed_mps == pytest.approx(8.0)
    # the same events again: latest has no rates, the held ones stay
    profile.on_message(combined(1024, 1, 512, 2))
    assert profile.latest.cadence_rpm == pytest.approx(60)    # (an exact repeat is ignored by latest)
    for _ in range(11):
        profile.on_message(combined(1024, 1, 512, 2))
    assert profile.held_cadence_rpm == pytest.approx(60)
    # over 12 messages without a new event: stopped
    profile.on_message(combined(1024, 1, 512, 2))
    assert profile.held_cadence_rpm == 0 and profile.held_speed_mps == 0
    # pedalling again
    profile.on_message(combined(2048, 2, 1024, 4))
    assert profile.held_cadence_rpm == pytest.approx(60)


def test_held_cadence_of_a_cadence_sensor():
    profile = SpeedCadenceProfile(CADENCE)
    profile.on_message(received(CADENCE, [0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x04, 10, 0])[0])
    profile.on_message(received(CADENCE, [0x80, 0xFF, 0xFF, 0xFF, 0x00, 0x08, 11, 0])[0])
    # a background page repeating the event: no new cadence, held
    profile.on_message(received(CADENCE, [0x02, 1, 0, 0, 0x00, 0x08, 11, 0])[0])
    assert profile.latest.cadence_rpm is None and profile.held_cadence_rpm == pytest.approx(60)


###The C API decodes and builds the same

@pytest.fixture(scope="module")
def lib():
    return c.load()


def test_c_api_decode_page_matches(lib):
    rng = random.Random(7)
    for device_type in (HR, POWER, FE, CADENCE, SPEED):
        known = {HR: range(8), POWER: [0x01, 0x10, 0x11, 0x12, 0x13, 0x20], CADENCE: range(6), SPEED: range(6),
                 FE: [0x01, 0x02, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x30, 0x31, 0x32,
                      0x33, 0x36, 0x37]}[device_type]
        for _ in range(400):
            payload = bytearray(rng.randrange(256) for _ in range(8))
            payload[0] = rng.choice(list(known) + [0x46, 0x47, 0x50, 0x51, 0x52, rng.randrange(256)])
            out = c.AntPage()
            assert lib.ant_decode_page(device_type, bytes(payload), ctypes.byref(out)) == c.ANT_OK
            assert c.page_dict(out) == ant_native.decode_page(device_type, bytes(payload)), (device_type, payload)
    out = c.AntPage()
    assert lib.ant_decode_page(COMBINED, bytes(8), ctypes.byref(out)) == c.ANT_FAILED


def test_c_api_page_field(lib):
    out = c.AntPage()
    lib.ant_decode_page(FE, bytes([0x10, 25, 40, 200, 0x88, 0x13, 140, 0x35]), ctypes.byref(out))
    value = ctypes.c_double()
    assert lib.ant_page_get_field(ctypes.byref(out), b"speed_mps", ctypes.byref(value)) == c.ANT_OK
    assert value.value == 5.0
    assert lib.ant_page_get_field(ctypes.byref(out), b"nope", ctypes.byref(value)) == c.ANT_FAILED


def test_c_api_builders_match(lib):
    nan = float("nan")
    assert c.build(lib, "ant_page_target_power", 250.1) == pages.target_power(250.1)
    assert c.build(lib, "ant_page_basic_resistance", 37.5) == pages.basic_resistance(37.5)
    assert c.build(lib, "ant_page_track_resistance", 5.0, 0.004) == pages.track_resistance(5.0, 0.004)
    assert c.build(lib, "ant_page_track_resistance", nan, nan) == pages.track_resistance()
    assert c.build(lib, "ant_page_wind_resistance", 0.51, -10, nan) == pages.wind_resistance(0.51, -10)
    assert c.build(lib, "ant_page_user_configuration", 75.5, 9.0, 0.7, nan) == \
        pages.user_configuration(75.5, 9.0, 0.7)
    assert c.build(lib, "ant_page_request", 0x36, 2, 0) == pages.request_page(0x36, 2)
    assert c.build(lib, "ant_page_power_manual_zero") == pages.power_manual_zero()
