from dataclasses import dataclass

from .base import ANTProfile, core_module


@dataclass(frozen=True)
class PowerMeterPage:
    page: int
    raw_page: int
    page_change_toggle: bool
    payload: bytes


@dataclass(frozen=True)
class PowerMeterData:
    page: int
    instantaneous_power_watts: int
    # Crank cadence as reported directly by the power meter itself (RPM,
    # already a plain instantaneous value - no rollover math needed,
    # unlike a dedicated cadence sensor's revolution/tick counters). None
    # when the power meter reports 0xFF (not supported by this hardware).
    # See ClassAntDevicesPoller's cadence fallback for why this exists.
    power_meter_cadence_rpm: int | None = None


class PowerMeterProfile(ANTProfile):
    """ANT+ Bicycle Power profile.

    The native layer decodes the Standard Power-Only page (0x10) into message["instantaneous_power_watts"]; the
    native profile turns that and the page's cadence byte into PowerMeterData. Every other page (torque, pedal
    smoothness, calibration, battery, ...) is decoded field by field into `pages` (see ant.pages).
    """
    device_type = 11

    def __init__(self):
        self._attach(core_module().PowerMeterProfile())

    def _reading(self, fields):
        return PowerMeterData(**fields)

    @property
    def last_payload(self):
        raw = self._core.last_raw
        return None if raw is None else PowerMeterPage(*raw)
