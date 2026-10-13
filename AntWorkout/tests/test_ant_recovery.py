"""RecoveryManager on a fake stick and a fake clock: device states, frozen readings and the escalation steps"""

import pytest

import ant.device as device_module
import ant.manager as manager_module
import ant.recovery as recovery_module
from ant.manager import ANTManager, MODE_CHANNELS
from ant.recovery import RecoveryManager
import fakes

POWER = (11, 100, 5)
HRM = (120, 200, 1)


@pytest.fixture
def clock(monkeypatch):
    clock = fakes.FakeClock()
    for module in (device_module, manager_module, recovery_module):
        monkeypatch.setattr(module, "monotonic", clock)
    monkeypatch.setattr(manager_module, "sleep", clock.advance)
    return clock


@pytest.fixture
def native():
    return fakes.FakeNative()


@pytest.fixture
def ant(native, clock):
    ant = ANTManager(native)
    ant.open()
    ant.configure_scan(bytes(8), channel=0)
    ant.start()
    return ant


class Rig:
    def __init__(self, ant, native, clock, **kwargs):
        self.ant, self.native, self.clock = ant, native, clock
        self.log = []
        options = dict(stale_timeout=1.5, lost_timeout=3, channel_retry_limit=2, channel_retry_cooldown=5,
                       device_reset_timeout=7, device_reset_cooldown=7, channel_reopen_wait=5,
                       channel_stick_reset_cooldown=30, channel_open_retry_interval=10, value_frozen_timeout=6,
                       hr_frozen_timeout=10, on_event=self.log.append)
        options.update(kwargs)
        self.recovery = RecoveryManager(ant, **options)
        self.watts = 200

    def power(self, watts=None, channel=0):
        if watts is not None:
            self.watts = watts
        self.native.deliver(fakes.power_page(100, self.watts, channel=channel, transmission_type=5,
                                             event_count=fakes.unique_payload_byte()))

    def same_power(self, channel=0):
        self.native.deliver(fakes.power_page(100, self.watts, channel=channel, transmission_type=5, event_count=0))

    def hr(self, channel=1):
        self.native.deliver(fakes.hr_page(200, 150, fakes.unique_payload_byte(), 1000, channel=channel,
                                          transmission_type=1))

    def device(self, device_type):
        return next(d for d in self.ant.get_devices() if d.device_type == device_type)

    def step(self, seconds):
        self.clock.advance(seconds)
        self.recovery.check()

    def logged(self, text):
        return [line for line in self.log if text in line]


@pytest.fixture
def rig(ant, native, clock):
    return Rig(ant, native, clock)


###Device states
def test_active_stale_lost_recovered(rig):
    rig.recovery.disable_escalation()
    rig.power()
    device = rig.device(11)
    rig.step(1)
    assert device.state == "ACTIVE"
    rig.step(1)
    assert device.state == "STALE" and rig.logged("DEVICE_STALE")
    rig.step(1.5)
    assert device.state == "LOST" and rig.logged("DEVICE_LOST")
    rig.step(10)
    assert len(rig.logged("DEVICE_LOST")) == 1     #logged once
    rig.power()
    rig.step(0.1)
    assert device.state == "ACTIVE" and rig.logged("DEVICE_RECOVERED")
    assert rig.recovery.report()["stale"] == 1 and rig.recovery.report()["lost"] == 1
    assert rig.recovery.report()["recoveries"] == 1


def test_unwatched_devices_are_left_alone(rig):
    rig.recovery.set_watched_devices(set())
    rig.power()
    rig.step(10)
    assert rig.device(11).state == "ACTIVE" and rig.log == []
    rig.recovery.set_watched_devices({(11, 100)})
    rig.step(0.1)
    assert rig.device(11).state == "LOST"


###Frozen readings
def test_frozen_power_reading_is_flagged_and_cleared(rig):
    rig.recovery.disable_escalation()
    rig.power(250)
    device = rig.device(11)
    for _ in range(7):
        rig.same_power()      #still transmitting, nothing new
        rig.step(1)
    assert device.state == "ACTIVE" and device.data_stalled
    assert rig.logged("DEVICE_DATA_STALLED")
    rig.power(260)
    assert not device.data_stalled


def test_frozen_reading_recovery_is_logged(rig):
    rig.recovery.disable_escalation()
    rig.power(250)
    for _ in range(7):
        rig.same_power()
        rig.step(1)
    rig.power(260)
    #cleared at once (the gauges' fallbacks see it straight away), logged on the next check, once
    assert not rig.device(11).data_stalled and not rig.logged("DEVICE_DATA_RECOVERED")
    rig.step(0.1)
    rig.step(0.1)
    assert len(rig.logged("DEVICE_DATA_RECOVERED")) == 1 and rig.recovery.report()["stalled_recovered"] == 1


def test_frozen_zero_watts_is_just_coasting(rig):
    rig.recovery.disable_escalation()
    rig.power(0)
    for _ in range(10):
        rig.same_power()
        rig.step(1)
    assert not rig.device(11).data_stalled


def test_frozen_heart_rate_waits_longer(rig):
    rig.recovery.disable_escalation()
    rig.native.deliver(fakes.hr_page(200, 150, 1, 1000, transmission_type=1))
    device = rig.device(120)
    for second in range(11):
        rig.native.deliver(fakes.hr_page(200, 150, 1, 1000, transmission_type=1))
        rig.step(1)
        assert device.data_stalled == (second >= 9)


def test_a_lost_device_isnt_also_stalled(rig):
    rig.recovery.disable_escalation()
    rig.power(250)
    device = rig.device(11)
    device.mark_stalled()
    rig.step(4)
    assert device.state == "LOST" and not device.data_stalled


###Escalation on the scan channel
def test_scan_channel_retries_then_resets(rig):
    rig.power()
    rig.step(3.1)                             #lost: first retry
    assert rig.recovery.report()["channel_retries"] == 1
    assert rig.native.names().count("open_rx_scan_mode") == 2
    rig.step(1)                               #cooldown
    assert rig.recovery.report()["channel_retries"] == 1
    rig.step(5)                               #second retry
    assert rig.recovery.report()["channel_retries"] == 2
    rig.step(5)                               #retries used up, lost for over 7s: reset
    assert rig.recovery.report()["device_resets"] == 1
    assert rig.native.names()[-3:] == ["set_rx_extended_messages_enabled", "open_rx_scan_mode",
                                       "start_message_pump"]
    assert "stop_message_pump" in rig.native.names() and "reset" in rig.native.names()
    #the pump goes back to the manager
    rig.power()
    assert rig.device(11).packet_count == 2


def test_failed_channel_retry_is_counted(rig):
    rig.power()
    rig.native.fail.add("open_rx_scan_mode")
    rig.step(3.1)
    assert rig.recovery.report()["channel_retry_failures"] == 1
    assert rig.logged("channel retry failed")


def test_failed_reset_is_counted(rig):
    rig.recovery.channel_retry_limit = 0
    rig.power()
    rig.native.fail.add("reset")
    rig.step(3.1)
    rig.step(7)
    assert rig.recovery.report()["device_reset_failures"] == 1
    assert rig.logged("device-level reset FAILED")


def test_channel_closed_event_resets_right_away(rig):
    rig.power()
    rig.clock.advance(manager_module.RECONFIGURE_EVENT_GRACE_S)
    rig.recovery.notify_channel_event("EVENT_CHANNEL_CLOSED", 0)
    rig.step(0.1)
    assert rig.recovery.report()["device_resets"] == 1


def test_our_own_channel_close_is_not_a_reason_to_reset(rig):
    rig.power()
    rig.ant.reopen_scan_channel()       #starts the grace for channel 0
    rig.recovery.notify_channel_event("EVENT_CHANNEL_CLOSED", 0)
    rig.step(0.1)
    assert rig.recovery.report()["device_resets"] == 0


def test_urgent_retry_event(rig):
    rig.power()
    rig.recovery.notify_channel_event("EVENT_RX_SEARCH_TIMEOUT", 0)
    rig.step(0.1)
    assert rig.recovery.report()["channel_retries"] == 1
    assert rig.logged("reason=urgent event")


def test_disabled_escalation_does_nothing(rig):
    rig.recovery.disable_escalation()
    rig.power()
    rig.step(30)
    assert rig.recovery.report()["channel_retries"] == 0 and rig.recovery.report()["device_resets"] == 0


###Escalation on dedicated channels
@pytest.fixture
def channels(rig):
    rig.ant.open_device_channels([POWER, HRM])
    rig.recovery.set_watched_devices({(11, 100), (120, 200)})
    rig.clock.advance(manager_module.RECONFIGURE_EVENT_GRACE_S)
    rig.power(channel=0)
    rig.hr(channel=1)
    rig.native.calls.clear()
    return rig


def test_lost_device_gets_its_channel_reopened_once(channels):
    rig = channels
    for _ in range(4):
        rig.hr()
        rig.step(1)
    #the power meter is lost, the heart rate monitor is fine
    assert rig.device(11).state == "LOST"
    assert rig.native.calls_to("close_channel") == [(0,)]
    assert rig.logged("RECOVERY reopening channel 0")
    #still silent after the reopen, but the HRM is heard: the stick works, no reset
    for _ in range(6):
        rig.hr()
        rig.step(1)
    assert rig.recovery.report()["device_resets"] == 0
    assert rig.logged("not resetting the stick")
    assert rig.native.calls_to("close_channel") == [(0,)]


def test_every_device_lost_resets_the_stick_once(channels):
    rig = channels
    rig.step(3.1)
    assert sorted(rig.native.calls_to("close_channel")) == [(0,), (1,)]
    rig.step(5)
    assert rig.recovery.report()["device_resets"] == 1
    assert rig.ant.mode == MODE_CHANNELS
    rig.step(60)
    assert rig.recovery.report()["device_resets"] == 1


def test_device_back_ends_the_episode(channels):
    rig = channels
    for _ in range(4):
        rig.hr()
        rig.step(1)
    assert rig.recovery._channel_reopens
    rig.power(channel=0)
    rig.hr()
    rig.step(0.1)
    assert rig.device(11).state == "ACTIVE" and rig.recovery._channel_reopens == {}


def test_closed_channel_is_reopened_right_away(channels):
    rig = channels
    rig.recovery.notify_channel_event("EVENT_CHANNEL_CLOSED", 1)
    rig.power(channel=0)
    rig.hr()
    rig.step(0.1)
    assert rig.native.calls_to("close_channel") == [(1,)]


def test_stick_refusing_channel_changes_is_reset(rig):
    rig.native.fail.update({"open_channel", "open_rx_scan_mode"})
    with pytest.raises(Exception):
        rig.ant.open_device_channels([POWER])
    assert rig.ant.needs_reset
    rig.native.fail.clear()
    rig.clock.advance(10)
    rig.recovery.check()
    assert rig.recovery.report()["device_resets"] == 1
    assert rig.ant.mode == MODE_CHANNELS and not rig.ant.needs_reset


def test_channels_that_failed_to_open_are_retried(rig):
    rig.native.fail_once.add("open_channel")
    with pytest.raises(Exception):
        rig.ant.open_device_channels([POWER])
    rig.recovery.check()
    assert rig.ant.mode == MODE_CHANNELS
    assert rig.recovery.report()["channel_open_retries"] == 1 and rig.logged("dedicated channels opened")


def test_failing_event_callback_is_ignored(ant, native, clock):
    def broken(_):
        raise RuntimeError
    recovery = RecoveryManager(ant, on_event=broken)
    native.deliver(fakes.power_page(100, 200))
    clock.advance(10)
    recovery.disable_escalation()
    recovery.check()
    assert ant.get_devices()[0].state == "LOST"
