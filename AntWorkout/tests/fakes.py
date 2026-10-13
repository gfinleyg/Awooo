"""
Stand-ins for the ANT USB stick, for driving ANTManager, RecoveryManager and AntWorkoutBridge without hardware.

FakeNative has every ant_native.ANTNative method the Python side calls. Each call is recorded (calls), every
configuration call succeeds unless its name is in fail (or fail_once), and messages are pushed into the message pump
callback with deliver(). The message dicts are shaped like ant_native's: the decoded fields its profile_decode fills
in, plus the raw payload.
"""

import itertools


class FakeNative:
    def __init__(self, sticks=None):
        self.calls = []
        self.fail = set()
        self.fail_once = set()
        self.opened = False
        self.callback = None
        self.sticks = sticks if sticks is not None else [
            {"index": 0, "vid": 0x0FCF, "pid": 0x1008, "serial_number": 1, "product_description": "Fake stick"}]
        self.send_results = []
        self.default_send_result = 1

    def _call(self, name, *args):
        self.calls.append((name, args))
        if name in self.fail_once:
            self.fail_once.discard(name)
            return False
        return name not in self.fail

    def names(self):
        return [name for name, _ in self.calls]

    def calls_to(self, name):
        return [args for called, args in self.calls if called == name]

    ###the stick
    def list_devices(self):
        return list(self.sticks)

    def open(self, baudrate, device_number):
        self.opened = self._call("open", baudrate, device_number)
        return self.opened

    def close(self):
        self._call("close")
        self.opened = False

    def is_open(self):
        return self.opened

    def reset(self, response_timeout_ms=1000):
        return self._call("reset")

    ###message pump
    def start_message_pump(self, callback):
        self._call("start_message_pump")
        self.callback = callback

    def stop_message_pump(self):
        self._call("stop_message_pump")
        self.callback = None

    def deliver(self, message):
        assert self.callback is not None, "message pump not started"
        self.callback(message)

    ###configuration (each returns success)
    def set_network_key(self, network_number, key):
        return self._call("set_network_key", network_number, key)

    def assign_channel(self, channel, channel_type, network_number):
        return self._call("assign_channel", channel, channel_type, network_number)

    def unassign_channel(self, channel):
        return self._call("unassign_channel", channel)

    def set_channel_id(self, channel, device_number, device_type, transmission_type):
        return self._call("set_channel_id", channel, device_number, device_type, transmission_type)

    def set_channel_period(self, channel, period):
        return self._call("set_channel_period", channel, period)

    def set_channel_rf_frequency(self, channel, frequency):
        return self._call("set_channel_rf_frequency", channel, frequency)

    def set_search_timeout(self, channel, timeout):
        return self._call("set_search_timeout", channel, timeout)

    def set_low_priority_search_timeout(self, channel, timeout):
        return self._call("set_low_priority_search_timeout", channel, timeout)

    def set_rx_extended_messages_enabled(self, enabled):
        return self._call("set_rx_extended_messages_enabled", enabled)

    def open_rx_scan_mode(self):
        return self._call("open_rx_scan_mode")

    def open_channel(self, channel):
        return self._call("open_channel", channel)

    def close_channel(self, channel):
        return self._call("close_channel", channel)

    def send_acknowledged(self, channel, payload, timeout_ms=500):
        self.calls.append(("send_acknowledged", (channel, bytes(payload), timeout_ms)))
        return self.send_results.pop(0) if self.send_results else self.default_send_result


class FakeClock:
    """monotonic() stand-in: time only moves when told to"""
    def __init__(self, start=1000.0):
        self.now = start

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


###----------------------------------------------------------------------------------------------
### Messages as ant_native hands them over (extended device ID on, as on the scan channel)
###----------------------------------------------------------------------------------------------
def message(device_type, device_number, payload, channel=0, transmission_type=1, **decoded):
    m = {"channel": channel, "has_device_id": True, "device_number": device_number, "device_type": device_type,
         "transmission_type": transmission_type, "payload": bytes(payload)}
    m.update(decoded)
    return m


def power_page(device_number, watts, cadence=90, event_count=0, **kwargs):
    """Standard Power-Only page (0x10)"""
    payload = [0x10, event_count & 0xFF, 0xFF, cadence, 0, 0, watts & 0xFF, (watts >> 8) & 0xFF]
    return message(11, device_number, payload, page=0x10, instantaneous_power_watts=watts, **kwargs)


def hr_page(device_number, bpm, beat_count, event_ticks, page=4, **kwargs):
    payload = [page, 0xFF, 0, 0, event_ticks & 0xFF, (event_ticks >> 8) & 0xFF, beat_count & 0xFF, bpm]
    return message(120, device_number, payload, page=page, heart_beat_event_time=event_ticks/1024.0,
                   heart_beat_event_ticks=event_ticks, heart_beat_count=beat_count, computed_heart_rate=bpm, **kwargs)


def bsc_page(device_type, device_number, event_ticks, revolutions, page=0, decoded=True, **kwargs):
    """Speed-only (123) / cadence-only (122) page: event time and cumulative revolutions in bytes 4-7"""
    payload = [page, 0xFF, 0xFF, 0xFF, event_ticks & 0xFF, (event_ticks >> 8) & 0xFF,
               revolutions & 0xFF, (revolutions >> 8) & 0xFF]
    fields = {"page": page}
    if decoded:
        fields.update(event_ticks=event_ticks, revolution_count=revolutions)
    fields.update(kwargs)
    return message(device_type, device_number, payload, **fields)


def combined_page(device_number, cadence_ticks, cadence_revs, speed_ticks, speed_revs, **kwargs):
    """Combined speed/cadence (121), page-less"""
    payload = [cadence_ticks & 0xFF, (cadence_ticks >> 8) & 0xFF, cadence_revs & 0xFF, (cadence_revs >> 8) & 0xFF,
               speed_ticks & 0xFF, (speed_ticks >> 8) & 0xFF, speed_revs & 0xFF, (speed_revs >> 8) & 0xFF]
    return message(121, device_number, payload, cadence_event_ticks=cadence_ticks,
                   cadence_event_time=cadence_ticks/1024.0, cadence_revolution_count=cadence_revs,
                   speed_event_ticks=speed_ticks, speed_event_time=speed_ticks/1024.0,
                   speed_revolution_count=speed_revs, **kwargs)


def trainer_page(device_number, watts, cadence=85, status=0, **kwargs):
    """FE-C Specific Trainer Data (0x19); decoded on the Python side"""
    payload = [0x19, 0, cadence, 0, 0, watts & 0xFF, ((watts >> 8) & 0x0F) | (status << 4), 0]
    return message(17, device_number, payload, **kwargs)


_counter = itertools.count()


def unique_payload_byte():
    """Something to vary in otherwise identical pages (profiles drop exact repeats)"""
    return next(_counter) & 0xFF
