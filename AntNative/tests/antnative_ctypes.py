"""ctypes bindings for antnative.dll's C API (include/antnative.h), as the tests use it. The structs here must match
the header's field for field."""
import ctypes
import os

from conftest import BUILD_DIR

ANT_API_VERSION = 2

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
FIELD_PAGE_DATA = 1 << 14

ANT_PAGE_FIELDS_MAX = 16
VALUE_INT, VALUE_DOUBLE, VALUE_BOOL = 0, 1, 2

READING_HEART_RATE = 1 << 0
READING_RR_INTERVAL = 1 << 1
READING_POWER = 1 << 2
READING_CADENCE = 1 << 3
READING_SPEED = 1 << 4
READING_TRAINER = 1 << 5
READING_CAPABILITIES = 1 << 6
READING_BATTERY = 1 << 7

u8, u16, u32, u64 = ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint32, ctypes.c_uint64


class AntPageField(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("type", ctypes.c_int), ("value", ctypes.c_double),
                ("int_value", ctypes.c_int64)]


class AntPage(ctypes.Structure):
    _fields_ = [("page", ctypes.c_int), ("name", ctypes.c_char_p), ("field_count", ctypes.c_int),
                ("fields", AntPageField * ANT_PAGE_FIELDS_MAX)]


class AntSensorId(ctypes.Structure):
    _fields_ = [("device_type", u8), ("device_number", u16), ("transmission_type", u8)]


class AntSensor(ctypes.Structure):
    _fields_ = [
        ("device_number", u16), ("device_type", u8), ("transmission_type", u8),
        ("channel", ctypes.c_int), ("state", ctypes.c_int), ("seconds_since_heard", ctypes.c_double),
        ("packet_count", u64), ("data_stalled", ctypes.c_int), ("fields", u32),
        ("heart_rate", ctypes.c_int), ("heart_beat_count", ctypes.c_int), ("rr_interval", ctypes.c_double),
        ("power_watts", ctypes.c_int), ("cadence_rpm", ctypes.c_double), ("speed_mps", ctypes.c_double),
        ("trainer_status", ctypes.c_int), ("supports_basic_resistance", ctypes.c_int),
        ("supports_target_power", ctypes.c_int), ("supports_simulation", ctypes.c_int),
        ("maximum_resistance_n", ctypes.c_int), ("battery_status", ctypes.c_int),
        ("battery_level_percent", ctypes.c_int), ("battery_voltage", ctypes.c_double),
    ]


class AntRecoveryOptions(ctypes.Structure):
    _fields_ = [("stale_timeout", ctypes.c_double), ("lost_timeout", ctypes.c_double),
                ("interval", ctypes.c_double), ("channel_retry_limit", ctypes.c_int),
                ("channel_retry_cooldown", ctypes.c_double), ("device_reset_timeout", ctypes.c_double),
                ("device_reset_cooldown", ctypes.c_double), ("channel_reopen_wait", ctypes.c_double),
                ("channel_stick_reset_cooldown", ctypes.c_double), ("channel_open_retry_interval", ctypes.c_double),
                ("hr_frozen_timeout", ctypes.c_double), ("value_frozen_timeout", ctypes.c_double)]


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
        ("page_data", AntPage),
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

    lib.ant_decode_page.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(AntPage)]
    lib.ant_page_get_field.argtypes = [ctypes.POINTER(AntPage), ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]
    out8 = ctypes.c_uint8 * 8
    lib.ant_page_basic_resistance.argtypes = [ctypes.c_double, out8]
    lib.ant_page_target_power.argtypes = [ctypes.c_double, out8]
    lib.ant_page_wind_resistance.argtypes = [ctypes.c_double] * 3 + [out8]
    lib.ant_page_track_resistance.argtypes = [ctypes.c_double] * 2 + [out8]
    lib.ant_page_user_configuration.argtypes = [ctypes.c_double] * 4 + [out8]
    lib.ant_page_request.argtypes = [u8, u8, ctypes.c_int, out8]
    lib.ant_page_power_manual_zero.argtypes = [out8]
    for name in ("ant_page_basic_resistance", "ant_page_target_power", "ant_page_wind_resistance",
                 "ant_page_track_resistance", "ant_page_user_configuration", "ant_page_request",
                 "ant_page_power_manual_zero"):
        getattr(lib, name).restype = None

    lib.ant_manager_create.restype = ctypes.c_void_p
    lib.ant_manager_destroy.argtypes = [ctypes.c_void_p]
    lib.ant_manager_open.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.ant_manager_close.argtypes = [ctypes.c_void_p]
    lib.ant_manager_configure_scan.argtypes = [ctypes.c_void_p, ctypes.c_char_p, u8, u8, u8]
    lib.ant_manager_start.argtypes = [ctypes.c_void_p]
    lib.ant_manager_stop.argtypes = [ctypes.c_void_p]
    lib.ant_manager_open_device_channels.argtypes = [ctypes.c_void_p, ctypes.POINTER(AntSensorId), ctypes.c_int]
    lib.ant_manager_return_to_scan.argtypes = [ctypes.c_void_p]
    lib.ant_manager_channel_for.argtypes = [ctypes.c_void_p, u8, u16]
    lib.ant_manager_send.argtypes = [ctypes.c_void_p, u8, u16, ctypes.c_char_p, u32, ctypes.POINTER(ctypes.c_int)]
    lib.ant_manager_sensors.argtypes = [ctypes.c_void_p, ctypes.POINTER(AntSensor), ctypes.c_int]
    lib.ant_manager_sensor_page.argtypes = [ctypes.c_void_p, u8, u16, ctypes.c_int, ctypes.POINTER(AntPage)]
    lib.ant_manager_set_wheel_circumference.argtypes = [ctypes.c_void_p, u16, ctypes.c_double]
    lib.ant_manager_poll.argtypes = [ctypes.c_void_p, ctypes.POINTER(AntMessage), ctypes.c_int]
    lib.ant_recovery_default_options.argtypes = [ctypes.POINTER(AntRecoveryOptions)]
    lib.ant_recovery_default_options.restype = None
    lib.ant_manager_start_recovery.argtypes = [ctypes.c_void_p, ctypes.POINTER(AntRecoveryOptions)]
    lib.ant_manager_stop_recovery.argtypes = [ctypes.c_void_p]
    lib.ant_manager_watch.argtypes = [ctypes.c_void_p, ctypes.POINTER(AntSensorId), ctypes.c_int]
    lib.ant_manager_next_log.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    return lib


def page_dict(page):
    """An AntPage as the Python module's page dict ("page", "page_name" and the fields)"""
    d = {"page": page.page, "page_name": page.name.decode()}
    for field in page.fields[:page.field_count]:
        name = field.name.decode()
        if field.type == VALUE_INT:
            d[name] = field.int_value
        elif field.type == VALUE_BOOL:
            d[name] = bool(field.int_value)
        else:
            d[name] = field.value
    return d


def build(lib, name, *args):
    """A page builder's 8 bytes"""
    out = (ctypes.c_uint8 * 8)()
    getattr(lib, name)(*args, out)
    return bytes(out)


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
    if m.fields & FIELD_PAGE_DATA:
        d["page_data"] = page_dict(m.page_data)
    return d
