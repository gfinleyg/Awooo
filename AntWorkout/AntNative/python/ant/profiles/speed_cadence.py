from dataclasses import dataclass

from .base import ANTProfile, core_module


@dataclass(frozen=True)
class SpeedCadencePage:
    page: int
    raw_page: int
    page_change_toggle: bool
    payload: bytes


@dataclass(frozen=True)
class SpeedCadenceData:
    """Decoded reading from a single-function speed (123) or cadence
    (122) sensor - standard ANT+ BSC page 0."""
    page: int
    event_time: float
    revolution_count: int
    event_ticks: int = 0
    speed_mps: float | None = None
    speed_kph: float | None = None
    cadence_rpm: float | None = None
    event_time_unwrapped: float | None = None


@dataclass(frozen=True)
class CombinedSpeedCadenceData:
    """Decoded reading from a combined speed+cadence sensor (121).

    Both readings ride in every message, so both are always present
    here (unlike SpeedCadenceData, which only ever carries one).
    speed_mps/speed_kph/cadence_rpm use the wheel_circumference_m the
    profile was constructed with; a caller with its own (possibly
    user-configurable) wheel size should instead recompute from the
    raw *_event_time/*_revolution_count fields, the same way the
    previous implementation always did.
    """
    cadence_event_time: float
    cadence_revolution_count: int
    speed_event_time: float
    speed_revolution_count: int
    cadence_event_ticks: int = 0
    speed_event_ticks: int = 0
    cadence_rpm: float | None = None
    speed_mps: float | None = None
    speed_kph: float | None = None
    cadence_event_time_unwrapped: float | None = None
    speed_event_time_unwrapped: float | None = None


class SpeedCadenceProfile(ANTProfile):
    """ANT+ Bicycle Speed/Cadence profile.

    Device types:
      121 = combined speed/cadence
      122 = cadence
      123 = speed

    The native profile keeps what's stateful across messages: rollover-safe rates (speed_mps/cadence_rpm) and the
    wheel-circumference-dependent math, wheel size being a runtime setting.
    """
    device_types = {121, 122, 123}

    @classmethod
    def can_handle(cls, device_type):
        return device_type in cls.device_types

    def __init__(self, device_type=None, wheel_circumference_m=2.199):
        self._device_type = device_type
        self._attach(core_module().SpeedCadenceProfile(device_type or 0, float(wheel_circumference_m)))

    def _attach(self, core):
        super()._attach(core)
        if not hasattr(self, "_device_type"):
            self._device_type = core.device_type

    @property
    def device_type(self):
        return self._device_type

    @property
    def wheel_circumference_m(self):
        return self._core.wheel_circumference_m

    @wheel_circumference_m.setter
    def wheel_circumference_m(self, value):
        self._core.wheel_circumference_m = float(value)

    def _reading(self, fields):
        if self._core.device_type == 121:
            return CombinedSpeedCadenceData(**fields)
        return SpeedCadenceData(**fields)

    @property
    def last_payload(self):
        if self._core.device_type == 121:
            return self._core.last_combined_payload
        raw = self._core.last_raw
        return None if raw is None else SpeedCadencePage(*raw)

    @property
    def event_time_unwrapped(self):
        return self._core.event_time_unwrapped

    @property
    def cadence_event_time_unwrapped(self):
        return self._core.cadence_event_time_unwrapped

    @property
    def speed_event_time_unwrapped(self):
        return self._core.speed_event_time_unwrapped

    @property
    def held_cadence_rpm(self):
        """The cadence as a display shows it: `latest` has none for a message repeating the last crank event, this
        holds the last one - until 12 messages (about 3 s) pass without a new event, when it drops to 0. None until
        a cadence has been computed."""
        return self._core.held_cadence_rpm

    @property
    def held_speed_mps(self):
        """The speed held the same way as held_cadence_rpm"""
        return self._core.held_speed_mps
