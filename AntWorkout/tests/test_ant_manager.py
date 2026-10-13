"""ANTManager on a fake stick: opening, scan and dedicated channel setup, falling back, and routing messages"""

import pytest

import ant.manager as manager_module
from ant.exceptions import ANTConfigurationError, ANTNotOpenError
from ant.manager import ANTManager, MODE_CHANNELS, MODE_SCAN
import fakes

KEY = bytes(range(8))
POWER = (11, 100, 5)
HRM = (120, 200, 1)
TRAINER = (17, 300, 5)


@pytest.fixture
def clock(monkeypatch):
    clock = fakes.FakeClock()
    monkeypatch.setattr(manager_module, "monotonic", clock)
    monkeypatch.setattr(manager_module, "sleep", clock.advance)
    return clock


@pytest.fixture
def native():
    return fakes.FakeNative()


@pytest.fixture
def scanning(native, clock):
    ant = ANTManager(native)
    assert ant.open()
    ant.configure_scan(KEY, channel=0)
    native.calls.clear()
    return ant


###Opening the stick
def test_open_picks_the_first_stick_that_opens(native):
    native.sticks = [{"index": 0, "product_description": "busy"}, {"index": 1, "product_description": "free"}]
    native.fail_once.add("open")
    logs = []
    ant = ANTManager(native, on_log=logs.append)
    assert ant.open()
    assert ant.device_number == 1
    assert native.calls_to("open") == [(57600, 0), (57600, 1)]
    assert any("Selected ANT device 1" in line for line in logs)


def test_open_fails_without_a_stick(native):
    native.sticks = []
    assert ANTManager(native).open() is False


def test_open_a_given_stick(native):
    ant = ANTManager(native)
    assert ant.open(device_number=3) and ant.device_number == 3


def test_a_failing_log_callback_is_ignored(native):
    def broken(_):
        raise ValueError
    assert ANTManager(native, on_log=broken).open()


###Scan channel
def test_configure_scan_needs_an_open_stick(native):
    with pytest.raises(ANTNotOpenError):
        ANTManager(native).configure_scan(KEY)


def test_configure_scan(native, clock):
    ant = ANTManager(native)
    ant.open()
    ant.configure_scan(KEY, channel=0)
    assert native.names()[1:] == ["reset", "set_network_key", "assign_channel", "set_channel_id",
                                  "set_channel_rf_frequency", "set_rx_extended_messages_enabled", "open_rx_scan_mode"]
    assert native.calls_to("set_network_key") == [(0, KEY)]
    assert native.calls_to("assign_channel") == [(0, 1, 0)]
    assert ant.mode == MODE_SCAN and ant._scan_config["rf_frequency"] == 57


def test_configure_scan_failure(native, clock):
    ant = ANTManager(native)
    ant.open()
    native.fail.add("open_rx_scan_mode")
    with pytest.raises(ANTConfigurationError):
        ant.configure_scan(KEY)
    assert ant.mode is None and ant._scan_config is None


###Dedicated channels
def test_dedicated_channels_need_the_scan_setup(native):
    with pytest.raises(ANTConfigurationError):
        ANTManager(native).open_device_channels([POWER])


def test_open_device_channels(scanning, native, clock):
    scanning.open_device_channels([POWER, HRM])
    assert scanning.mode == MODE_CHANNELS
    assert scanning.channel_devices() == {0: POWER, 1: HRM}
    assert scanning.channel_for(120, 200) == 1 and scanning.channel_for(120, 999) is None
    #the scan channel is closed first
    assert native.calls[:2] == [("close_channel", (0,)), ("unassign_channel", (0,))]
    assert native.calls_to("set_channel_id") == [(0, 100, 11, 5), (1, 200, 120, 1)]
    assert native.calls_to("set_channel_period") == [(0, 8182), (1, 8070)]
    assert native.calls_to("set_low_priority_search_timeout") == [(0, 0xFF), (1, 0xFF)]
    assert native.calls_to("set_search_timeout") == [(0, 0), (1, 0)]
    assert native.calls_to("open_channel") == [(0,), (1,)]
    assert not scanning.wants_device_channels()


def test_open_device_channels_waits_for_the_scan_channel_to_settle(scanning, clock):
    start = clock.now
    scanning.open_device_channels([POWER])
    assert clock.now - start == pytest.approx(manager_module.SCAN_MIN_OPEN_S)
    #not again once it's been open long enough
    scanning.return_to_scan()
    clock.advance(1)
    start = clock.now
    scanning.open_device_channels([POWER])
    assert clock.now == start


def test_stick_without_low_priority_search(scanning, native):
    native.fail.add("set_low_priority_search_timeout")
    scanning.open_device_channels([POWER])
    assert native.calls_to("set_search_timeout") == [(0, 0xFF)]


def test_unknown_device_type_gets_the_default_period(scanning, native):
    scanning.open_device_channels([(99, 1, 0)])
    assert native.calls_to("set_channel_period") == [(0, manager_module.DEFAULT_CHANNEL_PERIOD)]


def test_failed_channel_falls_back_to_scan(scanning, native):
    native.fail_once.add("open_channel")
    with pytest.raises(ANTConfigurationError):
        scanning.open_device_channels([POWER, HRM])
    assert scanning.mode == MODE_SCAN and scanning.channel_devices() == {}
    assert native.names()[-1] == "open_rx_scan_mode"
    assert scanning.wants_device_channels()
    #retried later
    assert scanning.retry_device_channels() is True
    assert scanning.mode == MODE_CHANNELS and scanning.channel_devices() == {0: POWER, 1: HRM}
    assert not scanning.wants_device_channels()


def test_failed_fall_back_flags_a_reset(scanning, native):
    native.fail.update({"open_channel", "open_rx_scan_mode"})
    with pytest.raises(ANTConfigurationError, match="needs a reset"):
        scanning.open_device_channels([POWER])
    assert scanning.needs_reset and scanning.mode == MODE_SCAN and scanning.wants_device_channels()
    #a reset clears it
    native.fail.clear()
    scanning.reapply_configuration()
    assert not scanning.needs_reset and scanning.mode == MODE_CHANNELS


def test_return_to_scan(scanning, native):
    scanning.open_device_channels([POWER, HRM])
    native.calls.clear()
    scanning.return_to_scan()
    assert scanning.mode == MODE_SCAN and scanning.channel_devices() == {}
    assert sorted(native.calls_to("close_channel")) == [(0,), (1,)]
    assert native.names()[-1] == "open_rx_scan_mode"
    #nothing wanted any more
    assert not scanning.wants_device_channels() and scanning.retry_device_channels() is False
    #already scanning: nothing to do
    native.calls.clear()
    scanning.return_to_scan()
    assert native.calls == []


def test_return_to_scan_failure(scanning, native):
    scanning.open_device_channels([POWER])
    native.fail.add("open_rx_scan_mode")
    with pytest.raises(ANTConfigurationError):
        scanning.return_to_scan()
    assert scanning.needs_reset


def test_reopen_channel(scanning, native):
    scanning.reopen_channel(0)      #not in channel mode: nothing
    assert native.calls == []
    scanning.open_device_channels([POWER, HRM])
    native.calls.clear()
    scanning.reopen_channel(1)
    assert native.calls[:2] == [("close_channel", (1,)), ("unassign_channel", (1,))]
    assert native.calls_to("set_channel_id") == [(1, 200, 120, 1)]
    scanning.reopen_channel(7)      #no such channel
    assert len(native.calls_to("set_channel_id")) == 1


def test_reopen_scan_channel(scanning, native):
    scanning.reopen_scan_channel()
    assert native.names() == ["close_channel", "unassign_channel", "assign_channel", "set_channel_id",
                              "set_channel_rf_frequency", "set_rx_extended_messages_enabled", "open_rx_scan_mode"]
    scanning.open_device_channels([POWER])
    native.calls.clear()
    scanning.reopen_scan_channel()   #in channel mode: nothing
    assert native.calls == []


def test_reapply_configuration_keeps_the_dedicated_channels(scanning, native):
    scanning.open_device_channels([POWER, HRM])
    native.calls.clear()
    scanning.reapply_configuration()
    assert native.names()[0] == "reset"
    assert scanning.mode == MODE_CHANNELS and scanning.channel_devices() == {0: POWER, 1: HRM}


def test_reapply_configuration_stays_on_scan_if_the_channels_fail(scanning, native):
    logs = []
    scanning.on_log = logs.append
    scanning.open_device_channels([POWER])
    native.fail_once.add("open_channel")
    scanning.reapply_configuration()    #doesn't raise
    assert scanning.mode == MODE_SCAN and scanning.wants_device_channels()
    assert any("staying on the scan channel" in line for line in logs)


###Our own channel closes
def test_events_suppressed(scanning, clock):
    assert not scanning.events_suppressed(0)
    scanning.open_device_channels([POWER, HRM])
    assert scanning.events_suppressed(0) and scanning.events_suppressed(None)
    clock.advance(manager_module.RECONFIGURE_EVENT_GRACE_S + 0.01)
    assert not scanning.events_suppressed(0)
    scanning.reopen_channel(1)
    #just that channel
    assert scanning.events_suppressed(1) and not scanning.events_suppressed(0)
    assert scanning.events_suppressed(None)
    assert scanning.events_suppressed(1, now=clock.now + 10) is False


###Messages
def test_messages_make_devices_with_their_profile(scanning, native):
    received = []
    scanning.start(lambda m, d, changed: received.append((d, changed)))
    native.deliver(fakes.power_page(100, 250, transmission_type=5))
    native.deliver(fakes.power_page(100, 250, transmission_type=5))
    native.deliver(fakes.bsc_page(123, 7, 1024, 1))
    native.deliver(fakes.message(99, 8, bytes(8)))
    devices = {d.device_type: d for d in scanning.get_devices()}
    assert set(devices) == {11, 123, 99}
    assert type(devices[11].profile).__name__ == "PowerMeterProfile"
    assert devices[123].profile.device_type == 123
    assert devices[99].profile is None
    assert [changed for _, changed in received] == [True, False, True, True]
    assert received[0][0] is received[1][0]


def test_channel_events_reach_the_callback_without_a_device(scanning, native):
    received = []
    scanning.start(lambda m, d, changed: received.append((m, d, changed)))
    event = {"channel": 0, "event_name": "EVENT_RX_FAIL"}
    native.deliver(event)
    assert received == [(event, None, False)]


def test_incomplete_device_id_is_not_a_device(scanning, native):
    received = []
    scanning.start(lambda m, d, changed: received.append(d))
    message = fakes.power_page(100, 250)
    message["device_number"] = None
    native.deliver(message)
    assert received == [None] and scanning.get_devices() == []


def test_dedicated_channel_messages_without_extended_id(scanning, native):
    scanning.open_device_channels([POWER, (120, 200, 0)])
    received = []
    scanning.start(lambda m, d, changed: received.append(d))
    message = fakes.power_page(100, 250, channel=0)
    del message["has_device_id"]
    native.deliver(message)
    assert received[0].identity == (100, 11, 5)
    #a channel whose transmission type isn't known (0, match any) can't name its device
    message = fakes.hr_page(200, 150, 1, 1000, channel=1)
    del message["has_device_id"]
    native.deliver(message)
    assert received[1] is None


def test_devices_heard_on_the_scan_channel_move_to_their_own(scanning, native):
    scanning.start()
    native.deliver(fakes.power_page(100, 250, channel=0, transmission_type=5))
    native.deliver(fakes.power_page(100, 260, channel=2, transmission_type=5))
    assert scanning.get_devices()[0].channel == 2


def test_dropped_devices_go_silent(scanning, native):
    received = []
    scanning.start(lambda m, d, changed: received.append(d))
    scanning.drop_devices.add(100)
    native.deliver(fakes.power_page(100, 250))
    assert received == []
    device = scanning.get_devices()[0]
    assert device.packet_count == 0


def test_stop_and_close(scanning, native):
    scanning.start(lambda *a: None)
    scanning.close()
    assert native.names()[-2:] == ["stop_message_pump", "close"]
    assert scanning._callback is None and not native.opened
