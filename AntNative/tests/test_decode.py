"""The message decode, through the Python module (ant_native._decode_message, what the message pump's callback
gets) and through antnative.dll (ant_decode_message, what ant_poll returns): known messages decode to the
expected fields, and both layers agree on everything"""
import ctypes
import random

import pytest

import ant_native
import antnative_ctypes as c

KEY_FIELDS = ("message_id", "channel", "payload", "device_number", "device_type", "transmission_type")


def extended(payload, device_number, device_type, channel=0, message_id=0x4E, transmission_type=1):
    """A broadcast with flagged extended data carrying the device ID, as the scan channel receives them"""
    data = bytes([channel]) + bytes(payload) + bytes([0x80, device_number & 0xFF, device_number >> 8,
                                                      device_type, transmission_type])
    return message_id, data


def decode(message_id, data, channel=None):
    return ant_native._decode_message(message_id, data, channel)


@pytest.fixture(scope="module")
def lib():
    return c.load()


def decode_c(lib, message_id, data, channel=None):
    message = c.AntMessage()
    rc = lib.ant_decode_message(message_id, data, len(data), -1 if channel is None else channel,
                                ctypes.byref(message))
    assert rc == c.ANT_OK, lib.ant_last_error()
    return c.as_dict(message)


### Known messages

def test_heart_rate_page_4():
    # previous beat at 1000 ticks, this one at 2024: one second apart
    m = decode(*extended([0x84, 0, 1000 & 0xFF, 1000 >> 8, 2024 & 0xFF, 2024 >> 8, 17, 60], 0x1234, 120, channel=2))
    assert m["channel"] == 2
    assert (m["device_number"], m["device_type"], m["transmission_type"]) == (0x1234, 120, 1)
    assert (m["page"], m["page_change_toggle"]) == (4, True)
    assert m["computed_heart_rate"] == 60
    assert m["heart_beat_count"] == 17
    assert m["heart_beat_event_ticks"] == 2024
    assert m["heart_beat_event_time"] == pytest.approx(2024 / 1024)
    assert m["rr_interval"] == pytest.approx(1.0)
    assert m["previous_heart_beat_event_time"] == pytest.approx(1000 / 1024)


def test_heart_rate_rr_interval_across_rollover():
    m = decode(*extended([0x04, 0, 0x00, 0xFF, 0x00, 0x03, 1, 70], 1, 120))
    # 0xFF00 -> 0x0300 is 0x400 ticks
    assert m["rr_interval"] == pytest.approx(1.0)


def test_heart_rate_background_page_has_the_beat_too():
    # every HRM data page (0-7) carries the beat in bytes 4-7
    m = decode(*extended([0x07, 90, 0, 0x30, 0x00, 0x04, 9, 75], 1, 120))
    assert m["page"] == 7
    assert (m["computed_heart_rate"], m["heart_beat_count"], m["heart_beat_event_ticks"]) == (75, 9, 1024)
    assert m["page_data"]["battery_level_percent"] == 90


def test_heart_rate_common_page_has_no_beat():
    m = decode(*extended([0x50, 0xFF, 0xFF, 1, 0x0F, 0x00, 0x02, 0x00], 1, 120))
    assert m["page"] == 0x50
    assert "computed_heart_rate" not in m
    assert m["page_data"]["manufacturer_id"] == 15


def test_power_only_page():
    m = decode(*extended([0x10, 5, 0xB2, 90, 0, 0, 250, 0], 77, 11))
    assert m["page"] == 0x10
    assert m["instantaneous_power_watts"] == 250
    assert m["power_meter_cadence_rpm"] == 90


def test_power_meter_without_cadence():
    m = decode(*extended([0x10, 5, 0xFF, 0xFF, 0, 0, 0x2C, 0x01], 77, 11))
    assert m["instantaneous_power_watts"] == 300
    assert "power_meter_cadence_rpm" not in m


def test_power_other_page_has_no_power():
    m = decode(*extended([0x12, 0, 0, 0, 0, 0, 0, 0], 77, 11))
    assert m["page"] == 0x12
    assert "instantaneous_power_watts" not in m


def test_combined_speed_cadence():
    m = decode(*extended([0x00, 0x04, 5, 0, 0x00, 0x08, 10, 0], 9, 121))
    assert "page" not in m
    assert (m["cadence_event_ticks"], m["cadence_revolution_count"]) == (1024, 5)
    assert (m["speed_event_ticks"], m["speed_revolution_count"]) == (2048, 10)
    assert m["cadence_event_time"] == pytest.approx(1.0)
    assert m["speed_event_time"] == pytest.approx(2.0)


@pytest.mark.parametrize("device_type", [122, 123])
def test_single_speed_or_cadence_page_0(device_type):
    m = decode(*extended([0x80, 0, 0, 0, 0x00, 0x02, 0x10, 0x01], 9, device_type))
    assert (m["page"], m["page_change_toggle"]) == (0, True)
    assert (m["event_ticks"], m["revolution_count"]) == (512, 0x110)
    assert m["event_time"] == pytest.approx(0.5)


def test_fitness_equipment_is_left_to_the_python_profile():
    m = decode(*extended([0x19, 1, 85, 0, 0, 0xC8, 0x00, 0], 3, 17))
    assert m["payload"] == bytes([0x19, 1, 85, 0, 0, 0xC8, 0x00, 0])
    assert "page" not in m


def test_dedicated_channel_message_without_extended_data():
    m = decode(0x4E, bytes([1, 0x84, 0, 0, 0, 0, 0, 0, 60]), channel=1)
    assert m["has_extended_data"] is False
    assert m["payload"] == bytes([0x84, 0, 0, 0, 0, 0, 0, 60])
    assert "page" not in m  # no device type to decode by


def test_response_event():
    m = decode(0x40, bytes([2, 1, 0x02]))
    assert m["message_name"] == "response_event"
    assert (m["channel"], m["response_to_message_id"], m["event_code"]) == (2, 1, 2)
    assert m["event_name"] == "EVENT_RX_FAIL"


def test_channel_id_response():
    m = decode(0x51, bytes([3, 0x34, 0x12, 120, 1]))
    assert (m["channel"], m["device_number"], m["device_type"], m["transmission_type"]) == (3, 0x1234, 120, 1)


def test_error_message():
    m = ant_native._decode_message(0xAE, bytes([0x02]), None, True, 0xAE)
    assert m["is_error"] is True
    assert m["error_code"] == 0xAE


### The C API decodes the same

def random_messages(count, seed=1234):
    rng = random.Random(seed)
    ids = [0x4E, 0x4F, 0x50, 0x5D, 0x5E, 0x5F, 0x40, 0x51, 0x4D, 0x6F]
    flags = [0x80, 0xC0, 0xE0, 0x20, 0x40, 0x00, 0x01, 0xFF]
    types = [120, 11, 121, 122, 123, 17, 0]
    pages = [0, 1, 4, 7, 0x10, 0x84, 0x90, 0xFF]
    for _ in range(count):
        size = rng.randrange(c.ANT_MESSAGE_DATA_MAX + 1)
        data = bytearray(rng.randrange(256) for _ in range(size))
        if size > 1:
            data[1] = rng.choice(pages)
        if size > 9:
            data[9] = rng.choice(flags)
        if size > 12:
            data[12] = rng.choice(types)
        channel = None if rng.random() < 0.5 else rng.randrange(8)
        yield rng.choice(ids) if rng.random() < 0.9 else rng.randrange(256), bytes(data), channel


def test_c_api_matches_python_module(lib):
    for message_id, data, channel in random_messages(5000):
        assert decode_c(lib, message_id, data, channel) == decode(message_id, data, channel), (message_id, data)


def test_c_api_known_message(lib):
    m = decode_c(lib, *extended([0x10, 5, 0xB2, 90, 0, 0, 250, 0], 77, 11))
    assert (m["device_number"], m["instantaneous_power_watts"], m["power_meter_cadence_rpm"]) == (77, 250, 90)


def test_c_api_rejects_oversized_message(lib):
    message = c.AntMessage()
    data = bytes(c.ANT_MESSAGE_DATA_MAX + 1)
    assert lib.ant_decode_message(0x4E, data, len(data), -1, ctypes.byref(message)) == c.ANT_ERROR_INVALID_ARGUMENT
    assert b"longer" in lib.ant_last_error()
