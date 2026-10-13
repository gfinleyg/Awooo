"""AntWorkoutBridge (ClassAntDevices.py) on a fake stick: discovery, connecting, the trainer's target power,
power meter calibration, battery warnings and starting up without a stick"""

import os
import time
from types import SimpleNamespace

import pytest

import ant.device as device_module
import ant.manager as manager_module
import ant.recovery as recovery_module
from ant.manager import ANTManager, MODE_CHANNELS, MODE_SCAN
from ant.profiles.fitness_equipment import basic_resistance_payload, target_power_payload
import ClassAntDevices as CAD
import ClassAntLogger
import fakes
from WorkoutHelpers import WriteSelectedDeviceLines

POWER_ID, TRAINER_ID, HRM_ID = (11, 100), (17, 300), (120, 200)


def log_text():
    return ClassAntLogger.read_log()


@pytest.fixture
def clock(monkeypatch):
    clock = fakes.FakeClock()
    for module in (device_module, manager_module, recovery_module):
        monkeypatch.setattr(module, "monotonic", clock)
    monkeypatch.setattr(manager_module, "sleep", clock.advance)
    monkeypatch.setattr(CAD, "time", SimpleNamespace(monotonic=clock))
    return clock


@pytest.fixture
def native(monkeypatch):
    native = fakes.FakeNative()
    real = ANTManager
    monkeypatch.setattr(CAD, "ANTManager", lambda on_log=None: real(native, on_log=on_log))
    return native


class Signals:
    def __init__(self, bridge):
        self.seen = []
        for name in ("deviceDiscovered", "devicesConnected", "devicesDisconnected", "dataUpdated", "statusMessage",
                     "errorOccurred", "calibrationUpdate", "batteryChanged"):
            getattr(bridge, name).connect(lambda *args, name=name: self.seen.append((name,) + args))

    def named(self, name):
        return [s[1:] for s in self.seen if s[0] == name]


@pytest.fixture
def bridge(native, clock):
    bridge = CAD.AntWorkoutBridge()
    bridge.running = True
    bridge._startup_cleanup()
    assert bridge._initialize_with_retries() is None
    bridge._initialized = True
    bridge._scan_started = clock.now
    bridge.signals = Signals(bridge)
    return bridge


def hear(native, *messages):
    for m in messages:
        native.deliver(m)


def power(watts=200, channel=0):
    return fakes.power_page(100, watts, channel=channel, transmission_type=5, event_count=fakes.unique_payload_byte())


def trainer(watts=200, channel=0):
    return fakes.trainer_page(300, watts, channel=channel, transmission_type=5)


def hr(channel=0):
    return fakes.hr_page(200, 140, fakes.unique_payload_byte(), 1000, channel=channel, transmission_type=1)


def connect(bridge, selected):
    bridge.request_connect(selected)
    bridge._process_pending_request()


###Selection
def test_read_selected_devices():
    WriteSelectedDeviceLines(["11 100", "120 200", "123 7", "17 300"])
    #(a trainer and a power meter share the power slot: the later one wins)
    assert CAD.read_selected_devices() == {"power": (17, 300), "hrm": (120, 200), "speed": (123, 7)}


###Starting up
def test_initialize_opens_and_scans(bridge, native):
    assert native.names()[:2] == ["open", "reset"]
    assert bridge.ant.mode == MODE_SCAN and native.callback is not None
    assert "Opening ANT USB stick" in log_text()


def test_initialize_retries_then_reports(native, clock, monkeypatch):
    monkeypatch.setattr(CAD, "INIT_RETRY_DELAYS_S", (0.001,)*3)
    native.fail.add("open")
    bridge = CAD.AntWorkoutBridge()
    bridge.running = True
    error = bridge._initialize_with_retries()
    assert error == "Failed to open ANT USB stick"
    assert native.names().count("open") == CAD.INIT_ATTEMPTS
    assert "failed after 4 attempts" in log_text()


def test_initialize_recovers_on_a_later_attempt(native, clock, monkeypatch):
    monkeypatch.setattr(CAD, "INIT_RETRY_DELAYS_S", (0.001,)*3)
    native.fail_once.add("reset")
    bridge = CAD.AntWorkoutBridge()
    bridge.running = True
    assert bridge._initialize_with_retries() is None
    assert native.names().count("open") == 2
    assert "ANT initialization failed: ANT scan configuration failed" in log_text()


def test_initialize_stops_when_asked(native, clock):
    native.fail.add("open")
    bridge = CAD.AntWorkoutBridge()
    bridge.running = False
    bridge._initialize_with_retries()
    assert native.names().count("open") == 0


def test_waiting_for_a_new_stick(native, clock, monkeypatch):
    monkeypatch.setattr(CAD, "STICK_POLL_S", 0.001)
    bridge = CAD.AntWorkoutBridge()
    bridge.running = True
    sticks = iter([[], [], [{"vid": 1, "pid": 2, "serial_number": 3}]])
    native.list_devices = lambda: next(sticks)
    assert bridge._wait_for_new_stick() is True
    bridge.running = False
    assert bridge._wait_for_new_stick() is False


def test_stick_ids_survive_a_listing_error(native, clock):
    bridge = CAD.AntWorkoutBridge()
    def broken():
        raise OSError
    native.list_devices = broken
    assert bridge._stick_ids() == frozenset()


def test_startup_cleanup_removes_old_files(native, clock):
    for name in CAD.STALE_FILES[:3]:
        open(name, "w").close()
    CAD.AntWorkoutBridge()._startup_cleanup()
    assert not any(os.path.exists(name) for name in CAD.STALE_FILES)


def test_take_error_once(bridge):
    bridge._error = "no stick"
    assert bridge.take_error() == "no stick" and bridge.take_error() is None


###Discovery
def test_discovery(bridge, native, clock):
    assert bridge.scan_settle_remaining() == CAD.SCAN_SETTLE_S
    hear(native, power(), power(), hr())
    assert bridge.found_devices() == ["11 100", "120 200"]
    assert bridge.signals.named("deviceDiscovered") == [(11, 100), (120, 200)]
    assert bridge.signals.named("dataUpdated") == [("power",), ("power",), ("hrm",)]
    clock.advance(CAD.SCAN_SETTLE_S + 1)
    assert bridge.scan_settle_remaining() == 0
    assert "Discovered device type=11 number=100" in log_text()


def test_unchanged_pages_dont_signal_new_data(bridge, native):
    page = power()
    hear(native, page, page)
    assert bridge.signals.named("dataUpdated") == [("power",)]


def test_channel_events(bridge, native):
    notified = []
    bridge.recovery.notify_channel_event = lambda name, channel: notified.append((name, channel))
    hear(native, {"channel": 0, "event_name": "EVENT_RX_FAIL"},
         {"channel": 1, "event_name": "EVENT_CHANNEL_CLOSED"},
         {"channel": 2, "event_name": "UNKNOWN", "event_code": 0x99, "response_to_message_id": 0x4B})
    assert notified == [("EVENT_RX_FAIL", 0), ("EVENT_CHANNEL_CLOSED", 1), ("UNKNOWN", 2)]
    text = log_text()
    #routine events aren't logged
    assert "EVENT_RX_FAIL" not in text
    assert "EVENT_CHANNEL_CLOSED (channel=1)" in text
    assert "UNKNOWN (channel=2, code=0x99, response_to=0x4B)" in text


###Connecting
def test_connect_opens_dedicated_channels(bridge, native):
    hear(native, power(), hr())
    connect(bridge, {"power": POWER_ID, "hrm": HRM_ID})
    assert bridge.is_ready() and bridge.is_connect_requested()
    assert bridge.selected_roles() == {"power", "hrm"}
    assert bridge.ant.mode == MODE_CHANNELS
    #the transmission types heard while scanning; channels in role order (hrm, power)
    assert bridge.ant.channel_devices() == {0: (120, 200, 1), 1: (11, 100, 5)}
    assert bridge.recovery._watched_devices == {POWER_ID, HRM_ID}
    assert bridge.signals.named("devicesConnected") == [()]
    assert bridge.selected_device("power").device_number == 100
    #looked up once
    assert bridge.selected_device("power") is bridge.selected_device("power")
    assert bridge.selected_device("speed") is None


def test_connect_to_a_device_not_heard_yet(bridge, native):
    connect(bridge, {"hrm": HRM_ID})
    assert bridge.ant.channel_devices() == {0: (120, 200, 0)}
    assert bridge.selected_device("hrm") is None
    hear(native, hr(channel=0))
    assert bridge.selected_device("hrm").device_number == 200


def test_connect_stays_on_scan_if_channels_fail(bridge, native):
    native.fail_once.add("open_channel")
    connect(bridge, {"power": POWER_ID})
    assert bridge.is_ready() and bridge.ant.mode == MODE_SCAN
    assert "Opening dedicated device channels failed" in log_text()


def test_disconnect_goes_back_to_scanning(bridge, native, clock):
    hear(native, power())
    connect(bridge, {"power": POWER_ID})
    bridge.set_trainer_target_power(100)
    bridge.request_disconnect()
    assert not bridge.is_ready() and not bridge.is_connect_requested()
    assert bridge.found_devices() == [] and bridge.scan_settle_remaining() == CAD.SCAN_SETTLE_S
    bridge._process_pending_request()
    assert bridge.ant.mode == MODE_SCAN and bridge.selected_roles() == set()
    assert bridge._trainer_target is None and bridge.recovery._watched_devices == set()
    assert bridge.signals.named("devicesDisconnected") == [()]
    assert bridge.scan_settle_remaining() == CAD.SCAN_SETTLE_S
    clock.advance(1)
    assert bridge.scan_settle_remaining() == CAD.SCAN_SETTLE_S - 1


def test_connect_then_disconnect_before_processing(bridge):
    bridge.request_connect({"power": POWER_ID})
    bridge.request_disconnect()
    bridge._process_pending_request()
    #nothing was connected, nothing to disconnect
    assert bridge.selected_roles() == set() and not bridge.is_ready()
    assert bridge.signals.named("devicesConnected") == [] and bridge.signals.named("devicesDisconnected") == []


def test_clear_connection_flags(bridge):
    connect(bridge, {"power": POWER_ID})
    bridge.clear_connection_flags()
    assert not bridge.is_ready() and not bridge.is_connect_requested()
    #still connected
    assert bridge.selected_roles() == {"power"}


###The trainer
@pytest.fixture
def with_trainer(bridge, native):
    hear(native, trainer())
    connect(bridge, {"power": TRAINER_ID})
    native.calls.clear()
    return bridge


def sends(native):
    return [payload for channel, payload, _ in native.calls_to("send_acknowledged")]


def test_trainer_selected(bridge):
    assert not bridge.trainer_selected()
    connect(bridge, {"power": POWER_ID})
    assert not bridge.trainer_selected()
    connect(bridge, {"power": TRAINER_ID})
    assert bridge.trainer_selected()


def test_target_power_is_sent_once_the_trainer_is_heard(with_trainer, native, clock):
    bridge = with_trainer
    bridge.set_trainer_target_power(250.4)
    bridge._service_trainer_target()
    assert sends(native) == []                       #not heard since
    hear(native, trainer(201))
    assert bridge._wake_event.is_set()
    bridge._service_trainer_target()
    assert sends(native) == [target_power_payload(250)]
    #the same target isn't sent again until the resend interval
    hear(native, trainer(202))
    bridge._service_trainer_target()
    assert len(sends(native)) == 1
    clock.advance(CAD.TRAINER_TARGET_RESEND_S)
    hear(native, trainer(203))
    bridge._service_trainer_target()
    assert len(sends(native)) == 2
    #a new target goes right away
    bridge.set_trainer_target_power(300)
    hear(native, trainer(204))
    bridge._service_trainer_target()
    assert sends(native)[-1] == target_power_payload(300)


def test_target_power_is_clamped(with_trainer):
    with_trainer.set_trainer_target_power(99999)
    assert with_trainer._trainer_target["watts"] == 4000
    with_trainer.set_trainer_target_power(-20)
    assert with_trainer._trainer_target["watts"] == 0
    with_trainer.set_trainer_target_power(None)
    assert with_trainer._trainer_target is None


def test_release_sends_basic_resistance(with_trainer, native):
    with_trainer.release_trainer_power()
    hear(native, trainer(210))
    with_trainer._service_trainer_target()
    assert sends(native) == [basic_resistance_payload(0)]


def test_failed_sends_are_retried_sooner_and_logged_once(with_trainer, native, clock):
    bridge = with_trainer
    native.default_send_result = "EVENT_TRANSFER_TX_FAILED"
    bridge.set_trainer_target_power(200)
    for _ in range(3):
        hear(native, trainer())
        bridge._service_trainer_target()
        clock.advance(CAD.TRAINER_RETRY_S)
    assert len(sends(native)) == 3
    assert log_text().count("Target power 200W send failed") == 1
    #after the fast retries, back to the normal interval
    for _ in range(3):
        hear(native, trainer())
        bridge._service_trainer_target()
        clock.advance(CAD.TRAINER_RETRY_S)
    assert len(sends(native)) == 4
    native.default_send_result = CAD.ANTFRAMER_PASS
    clock.advance(CAD.TRAINER_TARGET_RESEND_S)
    hear(native, trainer())
    bridge._service_trainer_target()
    assert bridge._trainer_target["failures"] == 0


def test_power_meter_never_gets_a_target(bridge, native):
    hear(native, power())
    connect(bridge, {"power": POWER_ID})
    bridge.set_trainer_target_power(200)
    hear(native, power())
    bridge._service_trainer_target()
    assert sends(native) == []


def test_send_without_a_dedicated_channel(bridge):
    assert bridge._send_to_device(17, 300, b"x"*8).startswith("error")


@pytest.mark.parametrize("target, now, due", [
    ({"watts": 200, "attempted_watts": None, "last_sent": 0}, 0, True),
    ({"watts": 200, "attempted_watts": 200, "last_sent": 10}, 11, False),
    ({"watts": 200, "attempted_watts": 200, "last_sent": 10}, 12, True),
    ({"watts": 200, "attempted_watts": 200, "last_sent": 10, "failures": 1}, 10.5, True),
    ({"watts": 200, "attempted_watts": 200, "last_sent": 10, "failures": 4}, 10.5, False),
    ({"watts": 210, "attempted_watts": 200, "last_sent": 10}, 10, True),
])
def test_trainer_send_due(target, now, due):
    assert CAD.AntWorkoutBridge._trainer_send_due(target, now) is due


###Power meter calibration
@pytest.fixture
def with_meter(bridge, native):
    hear(native, power())
    connect(bridge, {"power": POWER_ID})
    native.calls.clear()
    return bridge


def test_calibration_request_and_success(with_meter, native):
    bridge = with_meter
    bridge.request_power_calibration(100)
    bridge._service_calibration()
    assert sends(native) == []          #waits to hear the meter
    hear(native, power())
    bridge._service_calibration()
    assert sends(native) == [CAD.CALIBRATION_REQUEST_PAYLOAD]
    assert bridge.signals.named("calibrationUpdate") == [(100, "sending", None, 1)]
    response = fakes.message(11, 100, [0x01, 0xAC, 0, 0, 0, 0, 0xF6, 0xFF], channel=0, transmission_type=5, page=1)
    hear(native, response)
    assert bridge.signals.named("calibrationUpdate")[-1] == (100, "success", -10, 1)
    assert bridge._calibration is None


def test_calibration_failed_answer(with_meter, native):
    with_meter.request_power_calibration(100)
    hear(native, fakes.message(11, 100, [0x01, 0xAF, 0, 0, 0, 0, 5, 0], channel=0, transmission_type=5, page=1))
    assert with_meter.signals.named("calibrationUpdate")[-1] == (100, "failed", 5, 0)


def test_calibration_times_out(with_meter, native, clock):
    with_meter.request_power_calibration(100)
    clock.advance(CAD.CALIBRATION_TIMEOUT_S + 1)
    with_meter._service_calibration()
    assert with_meter.signals.named("calibrationUpdate") == [(100, "timeout", None, 0)]


def test_calibration_cancel_and_other_devices(with_meter, native):
    with_meter.request_power_calibration(999)
    hear(native, power())
    assert not with_meter._calibration["heard"]
    with_meter.cancel_power_calibration()
    with_meter._service_calibration()
    assert sends(native) == []


###Batteries
def battery_page(device_type, device_number, status, level=None, channel=0, transmission_type=5):
    if device_type == 120:
        payload = [0x07, 0xFF if level is None else level, 0, (status << 4) | 3, 0, 0, 0, 0]
    else:
        payload = [0x52, 0xFF, 0xFF, 0, 0, 0, 0, (status << 4) | 3]
    return fakes.message(device_type, device_number, payload, channel=channel, transmission_type=transmission_type,
                         page=payload[0])


def test_low_battery_warnings(bridge, native):
    hear(native, power(), hr(), trainer())
    assert bridge.low_battery_warnings() == []
    hear(native, battery_page(11, 100, 4), battery_page(120, 200, 5, level=8, transmission_type=1),
         battery_page(17, 300, 4))
    #nothing selected yet
    assert bridge.low_battery_warnings() == []
    assert bridge.signals.named("batteryChanged") == []
    connect(bridge, {"power": POWER_ID, "hrm": HRM_ID})
    assert bridge.low_battery_warnings() == ["Heart rate monitor battery critical (8%)", "Power meter battery low"]
    connect(bridge, {"power": TRAINER_ID})
    assert bridge.low_battery_warnings() == ["Trainer battery low"]


def test_battery_change_signals_once(bridge, native):
    hear(native, power())
    connect(bridge, {"power": POWER_ID})
    bridge.signals.seen.clear()
    page = battery_page(11, 100, 4)
    hear(native, page)
    hear(native, battery_page(11, 100, 4, channel=0))
    assert bridge.signals.named("batteryChanged") == [()]
    assert "battery Low 3.00V" in log_text()


###The whole thread
def test_run_thread(native, monkeypatch):
    """run() on its own thread: initializes, connects when asked, drives the trainer and shuts down"""
    bridge = CAD.AntWorkoutBridge()
    bridge.start()
    try:
        deadline = time.time() + 5
        while not bridge.is_initialized() and time.time() < deadline:
            time.sleep(0.01)
        assert bridge.is_initialized()
        native.deliver(trainer())
        bridge.request_connect({"power": TRAINER_ID})
        while not bridge.is_ready() and time.time() < deadline:
            time.sleep(0.01)
        assert bridge.is_ready() and bridge.ant.mode == MODE_CHANNELS
        bridge.set_trainer_target_power(180)
        while not native.calls_to("send_acknowledged") and time.time() < deadline:
            native.deliver(trainer(fakes.unique_payload_byte()))
            time.sleep(0.02)
        assert native.calls_to("send_acknowledged")[0][1] == target_power_payload(180)
    finally:
        bridge.stop()
        assert bridge.wait(5000)
    assert native.names()[-2:] == ["stop_message_pump", "close"]


def test_run_thread_without_a_stick(native, monkeypatch):
    monkeypatch.setattr(CAD, "INIT_RETRY_DELAYS_S", (0.001,)*3)
    monkeypatch.setattr(CAD, "STICK_POLL_S", 0.01)
    native.sticks = []
    bridge = CAD.AntWorkoutBridge()
    bridge.start()
    try:
        deadline = time.time() + 5
        while bridge.waiting_for_stick() is None and time.time() < deadline:
            time.sleep(0.01)
        assert "Plug in an ANT USB stick" in bridge.waiting_for_stick()
        assert bridge.take_error() == "Failed to open ANT USB stick"
        #plugged in
        native.sticks = [{"index": 0, "vid": 1, "pid": 2, "serial_number": 3}]
        while not bridge.is_initialized() and time.time() < deadline:
            time.sleep(0.01)
        assert bridge.is_initialized() and bridge.waiting_for_stick() is None
    finally:
        bridge.stop()
        assert bridge.wait(5000)
