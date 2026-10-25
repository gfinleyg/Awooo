from dataclasses import dataclass

from .base import ANTProfile, core_module


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

    Page decoding (page number, bpm, beat count, RR interval) happens in the native layer, which fills the message's
    page/heart_beat_* fields; the native profile (src/core/ant_profiles.cpp) assembles those into HeartRateData,
    unwrapping the event time across its 64 s rollover.
    """
    device_type = 120

    def __init__(self):
        self._attach(core_module().HeartRateProfile())

    def _reading(self, fields):
        return HeartRateData(**fields)

    @property
    def last_payload(self):
        raw = self._core.last_raw
        return None if raw is None else HeartRatePage(*raw)

    @property
    def event_time_unwrapped(self):
        return self._core.event_time_unwrapped
