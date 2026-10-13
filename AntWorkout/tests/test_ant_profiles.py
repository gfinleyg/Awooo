"""The ANT+ profiles, battery pages and ANTDevice: decoding what ant_native hands over"""

import filecmp
import os

import pytest

from conftest import APP_DIR, REPO_DIR
from ant import battery
from ant.device import ANTDevice
from ant.profiles import (FitnessEquipmentProfile, HeartRateProfile, PowerMeterProfile, SpeedCadenceProfile,
                          PROFILE_CLASSES, target_power_payload)
from ant.profiles.fitness_equipment import basic_resistance_payload
import fakes


def test_vendored_ant_package_matches_antnative():
    """AntWorkout runs its own copy of the ant package (copied over by the AntNative build): it should be the same"""
    source = os.path.join(REPO_DIR, "AntNative", "python", "ant")
    vendored = os.path.join(APP_DIR, "AntNative", "python", "ant")
    differences = []
    for folder, _, files in os.walk(source):
        if "__pycache__" in folder:
            continue
        for name in files:
            if name.endswith(".py"):
                relative = os.path.relpath(os.path.join(folder, name), source)
                other = os.path.join(vendored, relative)
                if not os.path.exists(other) or not filecmp.cmp(os.path.join(folder, name), other, shallow=False):
                    differences.append(relative)
    assert differences == []


def test_profile_lookup():
    handlers = {t: [cls.__name__ for cls in PROFILE_CLASSES if cls.can_handle(t)] for t in (11, 17, 120, 121, 122, 123, 99)}
    assert handlers == {11: ["PowerMeterProfile"], 17: ["FitnessEquipmentProfile"], 120: ["HeartRateProfile"],
                        121: ["SpeedCadenceProfile"], 122: ["SpeedCadenceProfile"], 123: ["SpeedCadenceProfile"],
                        99: []}


###Power meter
def test_power_meter():
    profile = PowerMeterProfile()
    profile.on_message(fakes.power_page(1, 250, cadence=92))
    assert profile.latest.instantaneous_power_watts == 250
    assert profile.latest.power_meter_cadence_rpm == 92
    assert profile.readings_received == 1 and profile.pages_received == 1
    #the same page again is ignored
    profile.on_message(fakes.power_page(1, 250, cadence=92))
    assert profile.pages_received == 1
    #no cadence from this meter
    profile.on_message(fakes.power_page(1, 260, cadence=0xFF))
    assert profile.latest.power_meter_cadence_rpm is None


def test_power_meter_other_pages_count_but_dont_change_the_reading():
    profile = PowerMeterProfile()
    profile.on_message(fakes.power_page(1, 250))
    reading = profile.latest
    profile.on_message(fakes.message(11, 1, [0x50, 0xFF, 0xFF, 1, 2, 3, 4, 5], page=0x50))
    assert profile.latest is reading
    assert profile.pages_received == 2 and profile.readings_received == 1
    assert profile.page_counts == {0x10: 1, 0x50: 1}


@pytest.mark.parametrize("message", [
    {"payload": b"\x10\x00"},                        #too short
    {"payload": bytes(8)},                           #no decoded page
    {},
])
def test_power_meter_ignores_unusable_messages(message):
    profile = PowerMeterProfile()
    profile.on_message(message)
    assert profile.pages_received == 0 and profile.latest is None


###Heart rate
def test_heart_rate_unwraps_event_time():
    profile = HeartRateProfile()
    profile.on_message(fakes.hr_page(2, 140, 10, 65000))
    assert profile.latest.computed_heart_rate == 140
    assert profile.latest.event_time_unwrapped == pytest.approx(65000/1024)
    #the 16 bit event time rolls over: 65000 -> 400 is 936 ticks later
    profile.on_message(fakes.hr_page(2, 141, 11, 400))
    assert profile.latest.event_time_unwrapped == pytest.approx((65000 + 936)/1024)
    assert profile.readings_received == 2


def test_heart_rate_page_without_a_reading():
    profile = HeartRateProfile()
    profile.on_message(fakes.message(120, 2, [0x07, 80, 0, 0x2F, 0, 0, 0, 0], page=7))
    assert profile.pages_received == 1 and profile.readings_received == 0 and profile.latest is None


###Speed/cadence
def test_cadence_sensor():
    profile = SpeedCadenceProfile(122)
    profile.on_message(fakes.bsc_page(122, 3, 1024, 100))
    assert profile.latest.cadence_rpm is None   #nothing to compare to yet
    profile.on_message(fakes.bsc_page(122, 3, 2048, 101))
    assert profile.latest.cadence_rpm == pytest.approx(60)
    assert profile.latest.speed_mps is None


def test_speed_sensor_rolls_over():
    profile = SpeedCadenceProfile(123, wheel_circumference_m=2.0)
    profile.on_message(fakes.bsc_page(123, 4, 65024, 65534))
    profile.on_message(fakes.bsc_page(123, 4, 512, 2))     #1024 ticks, 4 revolutions later
    assert profile.latest.speed_mps == pytest.approx(8.0)
    assert profile.latest.speed_kph == pytest.approx(28.8)
    assert profile.latest.event_time_unwrapped == pytest.approx((65024 + 1024)/1024)


def test_speed_sensor_reads_other_pages_from_the_payload():
    profile = SpeedCadenceProfile(123, wheel_circumference_m=2.0)
    profile.on_message(fakes.bsc_page(123, 4, 1024, 10, page=4, decoded=False))
    profile.on_message(fakes.bsc_page(123, 4, 2048, 12, page=2, decoded=False))
    assert profile.latest.speed_mps == pytest.approx(4.0)


def test_speed_sensor_same_event_time_has_no_speed():
    profile = SpeedCadenceProfile(123)
    profile.on_message(fakes.bsc_page(123, 4, 1024, 10, page=0))
    profile.on_message(fakes.bsc_page(123, 4, 1024, 10, page=1))   #a different page, same event
    assert profile.latest.speed_mps is None


def test_combined_sensor():
    profile = SpeedCadenceProfile(121, wheel_circumference_m=2.1)
    profile.on_message(fakes.combined_page(5, 1000, 50, 2000, 300))
    profile.on_message(fakes.combined_page(5, 1000 + 1024, 51, 2000 + 512, 302))
    latest = profile.latest
    assert latest.cadence_rpm == pytest.approx(60)
    assert latest.speed_mps == pytest.approx(2*2.1/0.5)
    assert latest.cadence_revolution_count == 51 and latest.speed_revolution_count == 302
    #repeats ignored
    profile.on_message(fakes.combined_page(5, 1000 + 1024, 51, 2000 + 512, 302))
    assert profile.pages_received == 2


###Trainer (FE-C)
def test_trainer_data():
    profile = FitnessEquipmentProfile()
    profile.on_message(fakes.trainer_page(6, 1234, cadence=88, status=0b0101))
    latest = profile.latest
    assert latest.instantaneous_power_watts == 1234
    assert latest.power_meter_cadence_rpm == 88
    assert latest.trainer_status == 0b0101
    #invalid power is skipped
    profile.on_message(fakes.trainer_page(6, 0xFFF))
    assert profile.latest is latest
    profile.on_message(fakes.trainer_page(6, 100, cadence=0xFF))
    assert profile.latest.power_meter_cadence_rpm is None
    assert profile.readings_received == 2


def test_trainer_capabilities():
    profile = FitnessEquipmentProfile()
    profile.on_message(fakes.message(17, 6, [0x36, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x27, 0b011]))
    caps = profile.capabilities
    assert caps.maximum_resistance_newtons == 10000
    assert caps.supports_basic_resistance and caps.supports_target_power and not caps.supports_simulation
    profile.on_message(fakes.message(17, 6, [0x36, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0b100]))
    assert profile.capabilities.maximum_resistance_newtons is None and profile.capabilities.supports_simulation


def test_trainer_page_toggle_bit():
    profile = FitnessEquipmentProfile()
    profile.on_message(fakes.message(17, 6, [0x80 | 0x10] + [0]*7))
    assert profile.last_page == 0x10 and profile.last_raw.page_change_toggle


@pytest.mark.parametrize("watts, quarter_watts", [(0, 0), (250, 1000), (250.1, 1000), (4000, 16000),
                                                  (5000, 16000), (-5, 0)])
def test_target_power_payload(watts, quarter_watts):
    payload = target_power_payload(watts)
    assert len(payload) == 8 and payload[0] == 0x31 and payload[1:6] == b"\xff"*5
    assert payload[6] | (payload[7] << 8) == quarter_watts


@pytest.mark.parametrize("percent, half_percent", [(0, 0), (50, 100), (100, 200), (150, 200), (-1, 0)])
def test_basic_resistance_payload(percent, half_percent):
    payload = basic_resistance_payload(percent)
    assert len(payload) == 8 and payload[0] == 0x30 and payload[7] == half_percent


###Battery
def descriptive(status, coarse=3):
    return (status << 4) | coarse


def test_common_battery_page():
    status = battery.decode_battery(11, [0x52, 0xFF, 0x10, 0, 0, 0, 128, descriptive(battery.BATTERY_STATUS_LOW)])
    assert status.status_name == "Low" and status.is_low and status.severity == 2
    assert status.voltage == pytest.approx(3.5) and status.battery_id == 1


def test_hrm_battery_page_with_toggle_and_level():
    status = battery.decode_battery(120, [0x80 | 0x07, 55, 0, descriptive(battery.BATTERY_STATUS_GOOD)])
    assert status is None   #too short
    status = battery.decode_battery(120, [0x80 | 0x07, 55, 0, descriptive(battery.BATTERY_STATUS_GOOD), 0, 0, 0, 0])
    assert status.level_percent == 55 and not status.is_low and status.severity == 1


def test_battery_level_only():
    #an unknown status with a low level counts as low
    status = battery.decode_battery(120, [0x07, 10, 0, descriptive(0, 0x0F), 0, 0, 0, 0])
    assert status.voltage is None and status.is_low and status.status_name == "Unknown"
    status = battery.decode_battery(120, [0x07, 0xFF, 0, descriptive(0), 0, 0, 0, 0])
    assert status.level_percent is None and not status.is_low and status.severity == 0


def test_bsc_battery_page():
    status = battery.decode_battery(123, [0x84, 0, 0, descriptive(battery.BATTERY_STATUS_CRITICAL), 0, 0, 0, 0])
    assert status.severity == 3
    #not a battery page for a power meter
    assert battery.decode_battery(11, [0x04, 0, 0, 0x50, 0, 0, 0, 0]) is None
    #a combined sensor can't report one
    assert battery.decode_battery(121, [0x52] + [0]*7) is None
    assert battery.decode_battery(11, None) is None


###ANTDevice
def test_device_update_and_battery():
    device = ANTDevice(1, 11, 5, 0, PowerMeterProfile())
    assert device.update(fakes.power_page(1, 200)) is True
    assert device.update(fakes.power_page(1, 200)) is False   #nothing new
    assert device.latest.instantaneous_power_watts == 200 and device.last_page == 0x10
    assert device.packet_count == 2
    device.update(fakes.message(11, 1, [0x52, 0xFF, 0x00, 0, 0, 0, 0, descriptive(battery.BATTERY_STATUS_OK)],
                                page=0x52))
    device.update(fakes.message(11, 1, [0x52, 0xFF, 0x10, 0, 0, 0, 0, descriptive(battery.BATTERY_STATUS_CRITICAL)],
                                page=0x52))
    #the weakest of the meter's two batteries
    assert device.battery.status == battery.BATTERY_STATUS_CRITICAL
    assert set(device.batteries) == {0, 1}
    assert device.identity == (1, 11, 5)


def test_device_without_profile_always_has_new_data():
    device = ANTDevice(1, 99, 5, 0)
    assert device.update({"payload": bytes(8)}) is True
    assert device.latest is None and device.last_page is None and device.battery is None


def test_device_states_and_stall():
    device = ANTDevice(1, 11, 5, 0, PowerMeterProfile())
    device.mark_state("LOST")
    assert device.state == "LOST"
    device.mark_recovered()
    assert device.state == "ACTIVE" and device.recovery_count == 1
    device.mark_stalled()
    since = device.stalled_since
    device.mark_stalled()
    assert device.data_stalled and device.stalled_since == since
    #new data clears it, noting it moved again (for RecoveryManager to log)
    assert not device.data_resumed
    device.update(fakes.power_page(1, 300))
    assert not device.data_stalled and device.data_resumed
    device.mark_error()
    status = device.status
    assert status["errors"] == 1 and status["state"] == "ACTIVE" and status["packets"] == 1
