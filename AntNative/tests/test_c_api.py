"""antnative.dll's C API without a stick: it loads, reports its version, and refuses calls on a closed device
with ANT_ERROR_NOT_OPEN instead of crashing"""
import ctypes

import pytest

import ant_native
import antnative_ctypes as c


@pytest.fixture(scope="module")
def lib():
    return c.load()


@pytest.fixture
def device(lib):
    handle = lib.ant_create()
    assert handle
    yield handle
    lib.ant_destroy(handle)


def test_api_version(lib):
    assert lib.ant_api_version() == c.ANT_API_VERSION


def test_list_devices_matches_python_module(lib):
    count = lib.ant_list_devices(None, 0)
    assert count == len(ant_native.list_devices())
    devices = (c.AntUsbDevice * max(count, 1))()
    assert lib.ant_list_devices(devices, count) == count
    for i in range(count):
        assert devices[i].index == i


def test_list_devices_bad_arguments(lib):
    assert lib.ant_list_devices(None, 4) == c.ANT_ERROR_INVALID_ARGUMENT


def test_closed_device(lib, device):
    assert lib.ant_is_open(device) == 0
    assert lib.ant_reset(device, 100) == c.ANT_ERROR_NOT_OPEN
    assert lib.ant_last_error() == b"ANT device is not open"
    assert lib.ant_set_network_key(device, 0, bytes(8), 100) == c.ANT_ERROR_NOT_OPEN
    channel_id = c.AntChannelId()
    assert lib.ant_get_channel_id(device, 0, 100, ctypes.byref(channel_id)) == c.ANT_ERROR_NOT_OPEN
    transfer = ctypes.c_int(-1)
    assert lib.ant_send_acknowledged(device, 0, bytes(8), 100, ctypes.byref(transfer)) == c.ANT_ERROR_NOT_OPEN
    assert lib.ant_start_messages(device) == c.ANT_ERROR_NOT_OPEN


def test_poll_closed_device_is_empty(lib, device):
    messages = (c.AntMessage * 4)()
    assert lib.ant_poll(device, messages, 4) == 0
    assert lib.ant_dropped_messages(device) == 0
    lib.ant_stop_messages(device)  # nothing running: a no-op


def test_null_device(lib):
    assert lib.ant_open(None, 57600, 0) == c.ANT_ERROR_INVALID_ARGUMENT
    assert lib.ant_reset(None, 100) == c.ANT_ERROR_INVALID_ARGUMENT
    assert lib.ant_is_open(None) == 0
    lib.ant_close(None)
    lib.ant_destroy(None)


###The manager (ant_manager), never opening a stick

@pytest.fixture
def manager(lib):
    handle = lib.ant_manager_create()
    assert handle
    yield handle
    lib.ant_manager_destroy(handle)


def test_manager_before_opening(lib, manager):
    sensors = (c.AntSensor * 4)()
    assert lib.ant_manager_sensors(manager, sensors, 4) == 0
    assert lib.ant_manager_configure_scan(manager, bytes(8), 0, 0, 57) == c.ANT_ERROR_NOT_OPEN
    assert lib.ant_manager_channel_for(manager, 11, 100) == -1
    transfer = ctypes.c_int(-1)
    assert lib.ant_manager_send(manager, 17, 300, bytes(8), 500, ctypes.byref(transfer)) == c.ANT_FAILED
    assert b"dedicated channel" in lib.ant_last_error()
    page = c.AntPage()
    assert lib.ant_manager_sensor_page(manager, 11, 100, 0x10, ctypes.byref(page)) == c.ANT_FAILED
    assert lib.ant_manager_set_wheel_circumference(manager, 5, 2.1) == c.ANT_FAILED
    messages = (c.AntMessage * 2)()
    assert lib.ant_manager_poll(manager, messages, 2) == 0
    # nothing to switch back from
    assert lib.ant_manager_return_to_scan(manager) == c.ANT_OK
    ids = (c.AntSensorId * 1)(c.AntSensorId(11, 100, 0))
    assert lib.ant_manager_open_device_channels(manager, ids, 1) == c.ANT_FAILED
    assert b"configure_scan" in lib.ant_last_error()


def test_manager_watchdog_and_log(lib, manager):
    ids = (c.AntSensorId * 1)(c.AntSensorId(11, 100, 0))
    assert lib.ant_manager_watch(manager, ids, 1) == c.ANT_ERROR
    options = c.AntRecoveryOptions()
    lib.ant_recovery_default_options(ctypes.byref(options))
    assert (options.stale_timeout, options.lost_timeout, options.channel_retry_limit) == (1.5, 3.0, 2)
    options.interval = 0.05
    assert lib.ant_manager_start_recovery(manager, ctypes.byref(options)) == c.ANT_OK
    assert lib.ant_manager_watch(manager, ids, 1) == c.ANT_OK
    assert lib.ant_manager_watch(manager, None, -1) == c.ANT_OK
    lib.ant_manager_stop_recovery(manager)
    line = ctypes.create_string_buffer(256)
    assert lib.ant_manager_next_log(manager, line, 256) == 0 and line.value == b""


def test_manager_null(lib):
    assert lib.ant_manager_open(None, -1) == c.ANT_ERROR_INVALID_ARGUMENT
    assert lib.ant_manager_sensors(None, None, 0) == c.ANT_ERROR_INVALID_ARGUMENT
    lib.ant_manager_close(None)
    lib.ant_manager_stop_recovery(None)
    lib.ant_manager_destroy(None)


def test_python_module_closed_device():
    native = ant_native.ANTNative()
    assert native.is_open() is False
    with pytest.raises(RuntimeError, match="not open"):
        native.reset()
    with pytest.raises(RuntimeError, match="not open"):
        native.start_message_pump(lambda message: None)
    native.stop_message_pump()
    assert native.request_message(0, 0x51) is False
    with pytest.raises(RuntimeError, match="0..255"):
        native.request_message(256, 0x51)
    native.close()
