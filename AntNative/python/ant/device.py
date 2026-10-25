"""ANTDevice: one ANT+ sensor - what's been heard from it, its profile, and the state RecoveryManager keeps on it.

A thin layer over the native Sensor (src/core/ant_manager.cpp), which does the work and is shared with the native
manager and watchdog; time comes from this module's monotonic(), which the tests replace with a fake clock.
"""

from time import monotonic

from . import battery as _battery
from .transport.native import ant_native as _core


def _clock():
    return monotonic()


def _field(name, doc=None):
    return property(lambda self: getattr(self._core, name),
                    lambda self, value: setattr(self._core, name, value), doc=doc)


class ANTDevice:

    def __init__(self, device_number, device_type, transmission_type, channel, profile=None):
        core = _core.Sensor(device_number, device_type, transmission_type, channel,
                            None if profile is None else profile._core, _clock)
        self._attach(core, profile)

    @classmethod
    def _wrap(cls, core, profile):
        """A device over a sensor the native manager created"""
        device = cls.__new__(cls)
        device._attach(core, profile)
        return device

    def _attach(self, core, profile):
        self._core = core
        self.profile = profile
        self.last_message = None

    device_number = property(lambda self: self._core.device_number)
    device_type = property(lambda self: self._core.device_type)
    transmission_type = property(lambda self: self._core.transmission_type)
    channel = _field("channel")

    #
    # Communication tracking
    #
    last_seen = _field("last_seen")
    packet_count = _field("packet_count")
    last_new_data_seen = _field(
        "last_new_data_seen",
        "When a message last actually changed the profile's decoded payload - unlike last_seen, which updates on "
        "every packet. The gap between the two tells a frozen reading from a device gone silent (see "
        "RecoveryManager._check_data_freshness).")

    #
    # Recovery state
    #
    state = _field("state")
    error_count = _field("error_count")
    recovery_attempts = _field("recovery_attempts")
    recovery_count = _field("recovery_count")
    last_state_change = _field("last_state_change")
    lost_time = _field("lost_time")
    stale_time = _field("stale_time")
    data_stalled = _field(
        "data_stalled",
        "The decoded value stopped changing although packets still arrive. Separate from `state`, which follows "
        "packet silence only: a device can be ACTIVE and data_stalled at once.")
    stalled_since = _field("stalled_since")
    data_resumed = _field(
        "data_resumed",
        "Set when new data clears data_stalled, until RecoveryManager has logged that the reading moved again.")

    @property
    def identity(self):
        return (self.device_number, self.device_type, self.transmission_type)

    def update(self, message):
        self.last_message = message
        return self._core.update(message)

    def mark_error(self):
        self._core.mark_error()

    def mark_state(self, state):
        self._core.mark_state(state)

    def mark_recovered(self):
        self._core.mark_recovered()

    def mark_stalled(self):
        self._core.mark_stalled()

    def clear_stalled(self):
        self._core.clear_stalled()

    @property
    def age(self):
        return monotonic() - self.last_seen

    @property
    def data_age(self):
        return monotonic() - self.last_new_data_seen

    @property
    def latest(self):
        return self.profile.latest if self.profile else None

    @property
    def batteries(self):
        """Latest battery status per battery id (a dual-sided power meter reports each side separately)"""
        return {battery_id: _battery.battery_status(fields) for battery_id, fields in self._core.batteries().items()}

    @property
    def battery(self):
        """The weakest battery"""
        return _battery.battery_status(self._core.battery())

    @property
    def last_page(self):
        return self.profile.last_page if self.profile else None

    @property
    def pages(self):
        """{page number: that page's fields, decoded} - see ant.pages"""
        return self.profile.pages if self.profile else {}

    @property
    def status(self):
        return {
            "id": self.device_number,
            "type": self.device_type,
            "state": self.state,
            "age": round(self.age, 3),
            "packets": self.packet_count,
            "recoveries": self.recovery_count,
            "errors": self.error_count,
            "data_stalled": self.data_stalled,
            "data_age": round(self.data_age, 3),
        }
