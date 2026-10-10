from dataclasses import dataclass
from .base import ANTProfile


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

    Page-format decoding lives in the pybind11 layer (ant_native.cpp
    profile_decode): it extracts page/event-ticks/revolution-count for
    122/123 (standard BSC page 0) and cadence+speed ticks/revolutions
    for 121 (the page-less combined layout, a direct port of the
    previous C/C++ receiver's live decode path). This class keeps only
    what's genuinely stateful across messages and can't live in a
    stateless transport layer: rollover-safe rate computation
    (speed_mps/cadence_rpm) and the wheel-circumference-dependent math,
    since wheel size is a runtime UI setting the transport layer has no
    business knowing about.
    """
    device_types = {121, 122, 123}

    @classmethod
    def can_handle(cls, device_type):
        return device_type in cls.device_types

    def __init__(self, device_type=None, wheel_circumference_m=2.199):
        self.device_type = device_type
        self.wheel_circumference_m = float(wheel_circumference_m)
        self.last_page = None
        self.last_payload = None
        self.latest = None
        self.pages_received = 0
        self.page_counts = {}
        self._last_payload_key = None

        # Single-function (122/123) rollover-tracking state.
        self._last_event_ticks = None
        self._last_revolutions = None
        self.event_time_unwrapped = None

        # Combined (121) rollover-tracking state, kept separate since
        # both readings arrive together on every message and need
        # independent tick/revolution history.
        self._last_cadence_ticks = None
        self._last_cadence_revs = None
        self._last_speed_ticks = None
        self._last_speed_revs = None
        self.cadence_event_time_unwrapped = None
        self.speed_event_time_unwrapped = None

    def on_message(self, message):
        payload = message.get("payload", b"")
        if len(payload) < 8:
            return

        if self.device_type == 121:
            self._handle_combined(payload, message)
            return

        self._handle_single_function(payload, message)

    def _handle_single_function(self, payload, message):
        if "page" not in message:
            return
        page = message["page"]
        toggle = message.get("page_change_toggle", False)

        key = (page, bytes(payload))
        if key == self._last_payload_key:
            return
        self._last_payload_key = key

        self.last_page = page
        self.last_payload = SpeedCadencePage(page, payload[0], toggle, bytes(payload))
        self.pages_received += 1
        self.page_counts[page] = self.page_counts.get(page, 0) + 1

        if "event_ticks" in message:
            event_ticks = message["event_ticks"]
            revolutions = message["revolution_count"]
        else:
            # The native decode only fills these in for page 0, but every
            # page (0-5, and a legacy sensor's page-less layout) carries
            # event time + cumulative revolutions in bytes 4-7
            event_ticks = payload[4] | (payload[5] << 8)
            revolutions = payload[6] | (payload[7] << 8)
        event_time = event_ticks / 1024.0

        if self._last_event_ticks is None:
            self.event_time_unwrapped = event_time
        else:
            dt = (event_ticks - self._last_event_ticks) & 0xFFFF
            if dt < 0x8000:
                self.event_time_unwrapped += dt / 1024.0

        speed_mps = speed_kph = cadence_rpm = None
        if self._last_event_ticks is not None and self._last_revolutions is not None:
            dt_ticks = (event_ticks - self._last_event_ticks) & 0xFFFF
            drev = (revolutions - self._last_revolutions) & 0xFFFF
            if dt_ticks and dt_ticks < 0x8000:
                dt = dt_ticks / 1024.0
                if self.device_type == 123:
                    speed_mps = drev * self.wheel_circumference_m / dt
                    speed_kph = speed_mps * 3.6
                elif self.device_type == 122:
                    cadence_rpm = drev * 60.0 / dt

        self._last_event_ticks = event_ticks
        self._last_revolutions = revolutions
        self.latest = SpeedCadenceData(
            page=page,
            event_time=event_time,
            revolution_count=revolutions,
            event_ticks=event_ticks,
            speed_mps=speed_mps,
            speed_kph=speed_kph,
            cadence_rpm=cadence_rpm,
            event_time_unwrapped=self.event_time_unwrapped,
        )

    def _handle_combined(self, payload, message):
        # No page number for this format (see profile_decode in
        # ant_native.cpp) - dedup on the raw payload alone.
        key = bytes(payload)
        if key == self._last_payload_key:
            return
        self._last_payload_key = key

        if "cadence_event_ticks" not in message:
            return

        self.last_payload = payload
        self.pages_received += 1

        cadence_ticks = message["cadence_event_ticks"]
        cadence_event_time = message["cadence_event_time"]
        cadence_revolutions = message["cadence_revolution_count"]
        speed_ticks = message["speed_event_ticks"]
        speed_event_time = message["speed_event_time"]
        speed_revolutions = message["speed_revolution_count"]

        if self._last_cadence_ticks is None:
            self.cadence_event_time_unwrapped = cadence_event_time
        else:
            dt = (cadence_ticks - self._last_cadence_ticks) & 0xFFFF
            if dt < 0x8000:
                self.cadence_event_time_unwrapped += dt / 1024.0

        if self._last_speed_ticks is None:
            self.speed_event_time_unwrapped = speed_event_time
        else:
            dt = (speed_ticks - self._last_speed_ticks) & 0xFFFF
            if dt < 0x8000:
                self.speed_event_time_unwrapped += dt / 1024.0

        cadence_rpm = None
        if self._last_cadence_ticks is not None and self._last_cadence_revs is not None:
            dt_ticks = (cadence_ticks - self._last_cadence_ticks) & 0xFFFF
            drev = (cadence_revolutions - self._last_cadence_revs) & 0xFFFF
            if dt_ticks and dt_ticks < 0x8000:
                cadence_rpm = drev * 60.0 / (dt_ticks / 1024.0)

        speed_mps = speed_kph = None
        if self._last_speed_ticks is not None and self._last_speed_revs is not None:
            dt_ticks = (speed_ticks - self._last_speed_ticks) & 0xFFFF
            drev = (speed_revolutions - self._last_speed_revs) & 0xFFFF
            if dt_ticks and dt_ticks < 0x8000:
                dt = dt_ticks / 1024.0
                speed_mps = drev * self.wheel_circumference_m / dt
                speed_kph = speed_mps * 3.6

        self._last_cadence_ticks = cadence_ticks
        self._last_cadence_revs = cadence_revolutions
        self._last_speed_ticks = speed_ticks
        self._last_speed_revs = speed_revolutions

        self.latest = CombinedSpeedCadenceData(
            cadence_event_time=cadence_event_time,
            cadence_revolution_count=cadence_revolutions,
            speed_event_time=speed_event_time,
            speed_revolution_count=speed_revolutions,
            cadence_event_ticks=cadence_ticks,
            speed_event_ticks=speed_ticks,
            cadence_rpm=cadence_rpm,
            speed_mps=speed_mps,
            speed_kph=speed_kph,
            cadence_event_time_unwrapped=self.cadence_event_time_unwrapped,
            speed_event_time_unwrapped=self.speed_event_time_unwrapped,
        )