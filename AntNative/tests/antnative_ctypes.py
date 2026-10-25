"""ctypes bindings for antnative.dll's C API (include/antnative.h), as the tests use it. The structs here must match
the header's field for field."""
import ctypes
import os

from conftest import BUILD_DIR

ANT_API_VERSION = 1

ANT_OK = 0
ANT_FAILED = 1
ANT_ERROR = -1
ANT_ERROR_INVALID_ARGUMENT = -2
ANT_ERROR_NOT_OPEN = -3

ANT_MESSAGE_DATA_MAX = 41
ANT_USB_STRING_MAX = 256

FIELD_CHANNEL = 1 << 0
FIELD_PAYLOAD = 1 << 1
FIELD_EXTENDED = 1 << 2
FIELD_RSSI = 1 << 3
FIELD_DEVICE_ID = 1 << 4
FIELD_RESPONSE_EVENT = 1 << 5
FIELD_ERROR_CODE = 1 << 6
FIELD_PAGE = 1 << 7
FIELD_HEART_RATE = 1 << 8
FIELD_HEART_RATE_PREVIOUS = 1 << 9
FIELD_POWER = 1 << 10
FIELD_POWER_CADENCE = 1 << 11
FIELD_SPEED_CADENCE = 1 << 12
FIELD_SPEED_OR_CADENCE = 1 << 13

u8, u16, u32, u64 = ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint32, ctypes.c_uint64


class AntUsbDevice(ctypes.Structure):
    _fields_ = [
        ("index", u32), ("vid", u32), ("pid", u32), ("serial_number", u32), ("device_type", u32),
        ("has_product_description", ctypes.c_int), ("has_serial_string", ctypes.c_int),
        ("product_description", ctypes.c_char * ANT_USB_STRING_MAX),
        ("serial_string", ctypes.c_char * ANT_USB_STRING_MAX),
    ]


class AntMessage(ctypes.Structure):
    _fields_ = [
        ("fields", u64),
        ("message_id", u8), ("is_error", u8), ("error_code", u8), ("channel", u8),
        ("message_name", ctypes.c_char_p),
        ("data_size", u8), ("data", u8 * ANT_MESSAGE_DATA_MAX),
        ("payload_size", u8), ("payload", u8 * 8),
        ("extended_bitfield", u8),
        ("extended_data_size", u8), ("extended_data", u8 * ANT_MESSAGE_DATA_MAX),
        ("rssi_data_size", u8), ("rssi_data", u8 * ANT_MESSAGE_DATA_MAX),
        ("device_number", u16), ("device_type", u8), ("transmission_type", u8),
        ("response_to_message_id", u8), ("event_code", u8), ("event_name", ctypes.c_char_p),
        ("page", u8), ("page_change_toggle", u8),
        ("heart_beat_event_time", ctypes.c_double), ("heart_beat_event_ticks", u16),
        ("heart_beat_count", u8), ("computed_heart_rate", u8),
        ("previous_heart_beat_event_time", ctypes.c_double), ("rr_interval", ctypes.c_double),
        ("instantaneous_power_watts", u16), ("power_meter_cadence_rpm", u8),
        ("cadence_event_time", ctypes.c_double), ("cadence_event_ticks", u16), ("cadence_revolution_count", u16),
        ("speed_event_time", ctypes.c_double), ("speed_event_ticks", u16), ("speed_revolution_count", u16),
        ("event_time", ctypes.c_double), ("event_ticks", u16), ("revolution_count", u16),
    ]


class AntChannelId(ctypes.Structure):
    _fields_ = [("device_number", u16), ("device_type", u8), ("transmission_type", u8)]


def load():
    lib = ctypes.CDLL(os.path.join(BUILD_DIR, "antnative.dll"))
    lib.ant_api_version.restype = ctypes.c_int
    lib.ant_last_error.restype = ctypes.c_char_p
    lib.ant_list_devices.argtypes = [ctypes.POINTER(AntUsbDevice), ctypes.c_int]
    lib.ant_create.restype = ctypes.c_void_p
    lib.ant_destroy.argtypes = [ctypes.c_void_p]
    lib.ant_open.argtypes = [ctypes.c_void_p, u32, u8]
    lib.ant_close.argtypes = [ctypes.c_void_p]
    lib.ant_is_open.argtypes = [ctypes.c_void_p]
    lib.ant_reset.argtypes = [ctypes.c_void_p, u32]
    lib.ant_set_network_key.argtypes = [ctypes.c_void_p, u8, ctypes.c_char_p, u32]
    lib.ant_get_channel_id.argtypes = [ctypes.c_void_p, u8, u32, ctypes.POINTER(AntChannelId)]
    lib.ant_send_acknowledged.argtypes = [ctypes.c_void_p, u8, ctypes.c_char_p, u32, ctypes.POINTER(ctypes.c_int)]
    lib.ant_start_messages.argtypes = [ctypes.c_void_p]
    lib.ant_stop_messages.argtypes = [ctypes.c_void_p]
    lib.ant_poll.argtypes = [ctypes.c_void_p, ctypes.POINTER(AntMessage), ctypes.c_int]
    lib.ant_dropped_messages.argtypes = [ctypes.c_void_p]
    lib.ant_dropped_messages.restype = u64
    lib.ant_decode_message.argtypes = [u8, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.POINTER(AntMessage)]
    return lib


def as_dict(m):
    """An AntMessage as the Python module's message dict, so the two can be compared"""
    d = {
        "message_id": m.message_id,
        "data": bytes(m.data[:m.data_size]),
        "message_size": m.data_size,
        "message_name": m.message_name.decode(),
        "is_error": bool(m.is_error),
        "has_extended_data": bool(m.fields & FIELD_EXTENDED),
    }
    if m.fields & FIELD_ERROR_CODE:
        d["error_code"] = m.error_code
    if m.fields & FIELD_CHANNEL:
        d["channel"] = m.channel
    if m.fields & FIELD_PAYLOAD:
        d["payload"] = bytes(m.payload[:m.payload_size])
    if m.fields & FIELD_EXTENDED:
        bits = m.extended_bitfield
        d.update(extended_bitfield=bits, has_device_id=bool(bits & 0x80), has_rssi=bool(bits & 0x40),
                 has_timestamp=bool(bits & 0x20), has_extension=bool(bits & 0x01),
                 extended_data=bytes(m.extended_data[:m.extended_data_size]),
                 has_timestamp_field=bool(bits & 0x20))
    if m.fields & FIELD_RSSI:
        d["rssi_data"] = bytes(m.rssi_data[:m.rssi_data_size])
    if m.fields & FIELD_DEVICE_ID:
        d.update(device_number=m.device_number, device_type=m.device_type, transmission_type=m.transmission_type)
    if m.fields & FIELD_RESPONSE_EVENT:
        d.update(response_to_message_id=m.response_to_message_id, event_code=m.event_code,
                 event_name=m.event_name.decode())
    if m.fields & FIELD_PAGE:
        d.update(page=m.page, page_change_toggle=bool(m.page_change_toggle))
    if m.fields & FIELD_HEART_RATE:
        d.update(heart_beat_event_time=m.heart_beat_event_time, heart_beat_event_ticks=m.heart_beat_event_ticks,
                 heart_beat_count=m.heart_beat_count, computed_heart_rate=m.computed_heart_rate)
    if m.fields & FIELD_HEART_RATE_PREVIOUS:
        d.update(previous_heart_beat_event_time=m.previous_heart_beat_event_time, rr_interval=m.rr_interval)
    if m.fields & FIELD_POWER:
        d["instantaneous_power_watts"] = m.instantaneous_power_watts
    if m.fields & FIELD_POWER_CADENCE:
        d["power_meter_cadence_rpm"] = m.power_meter_cadence_rpm
    if m.fields & FIELD_SPEED_CADENCE:
        d.update(cadence_event_time=m.cadence_event_time, cadence_event_ticks=m.cadence_event_ticks,
                 cadence_revolution_count=m.cadence_revolution_count, speed_event_time=m.speed_event_time,
                 speed_event_ticks=m.speed_event_ticks, speed_revolution_count=m.speed_revolution_count)
    if m.fields & FIELD_SPEED_OR_CADENCE:
        d.update(event_time=m.event_time, event_ticks=m.event_ticks, revolution_count=m.revolution_count)
    return d
