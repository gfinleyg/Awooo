from dataclasses import dataclass
from .base import ANTProfile


@dataclass(frozen=True)
class HeartRateData:
    page: int
    heart_beat_count: int
    heart_beat_event_time: float
    computed_heart_rate: int
    previous_heart_beat_event_time: float | None = None
    rr_interval: float | None = None
    event_time_unwrapped: float | None = None


@dataclass(frozen=True)
class HeartRatePage:
    page: int
    raw_page: int
    page_change_toggle: bool
    payload: bytes


class HeartRateProfile(ANTProfile):
    """ANT+ HRM profile.

    Page-format decoding (page number, bpm, beat count, RR interval) now
    happens in the pybind11 layer (ant_native.cpp profile_decode), which
    populates message["page"], message["heart_beat_event_time"], etc.
    directly. This class only assembles those fields into HeartRateData
    and tracks the one thing that genuinely needs to live in Python:
    rollover-safe event-time unwrapping, which is stateful across
    messages and has no reason to live in the transport layer.
    """
    device_type = 120

    def __init__(self):
        self.latest = None
        self.last_page = None
        self.last_payload = None
        self.pages_received = 0
        # New heart rate readings only (the pages carrying HR fields) -
        # pages_received also counts pages with nothing new in them.
        self.readings_received = 0
        self.page_counts = {}
        self._last_payload_key = None
        self._last_event_ticks = None
        self.event_time_unwrapped = None

    def on_message(self, message):
        payload = message.get("payload", b"")
        if len(payload) < 8 or "page" not in message:
            return

        page = message["page"]
        toggle = message.get("page_change_toggle", False)

        payload_key = (page, bytes(payload))
        if payload_key == self._last_payload_key:
            return
        self._last_payload_key = payload_key

        self.last_page = page
        self.last_payload = HeartRatePage(
            page=page,
            raw_page=payload[0],
            page_change_toggle=toggle,
            payload=bytes(payload),
        )
        self.pages_received += 1
        self.page_counts[page] = self.page_counts.get(page, 0) + 1

        if "heart_beat_event_time" not in message:
            return

        event_time = message["heart_beat_event_time"]
        event_ticks = message["heart_beat_event_ticks"]

        if self._last_event_ticks is None:
            self.event_time_unwrapped = event_time
        else:
            delta_ticks = (event_ticks - self._last_event_ticks) & 0xFFFF
            if delta_ticks < 0x8000:
                self.event_time_unwrapped += delta_ticks / 1024.0
        self._last_event_ticks = event_ticks

        self.readings_received += 1
        self.latest = HeartRateData(
            page=page,
            heart_beat_count=message["heart_beat_count"],
            heart_beat_event_time=event_time,
            computed_heart_rate=message["computed_heart_rate"],
            previous_heart_beat_event_time=message.get("previous_heart_beat_event_time"),
            rr_interval=message.get("rr_interval"),
            event_time_unwrapped=self.event_time_unwrapped,
        )
