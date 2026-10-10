from dataclasses import dataclass
from .base import ANTProfile


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

    Page decoding lives in the pybind11 layer (ant_native.cpp
    profile_decode), which decodes the Standard Power-Only page (0x10)
    and populates message["instantaneous_power_watts"] directly - the
    one page the previous C/C++ receiver ever read from real hardware.
    This class has no per-message state to track (power has no rollover
    math), so it's just a thin pass-through into PowerMeterData.
    """
    device_type = 11

    def __init__(self):
        self.latest = None
        self.last_page = None
        self.last_payload = None
        self.pages_received = 0
        # New power readings only (Standard Power-Only pages) - pages_received
        # also counts every other page (torque, battery, manufacturer/product
        # info), which carry no new wattage. Consumers recording power
        # samples should key off this one.
        self.readings_received = 0
        self.page_counts = {}
        self._last_payload_key = None

    def on_message(self, message):
        payload = message.get("payload", b"")
        if len(payload) < 8 or "page" not in message:
            return

        page = message["page"]
        toggle = message.get("page_change_toggle", False)

        key = (page, bytes(payload))
        if key == self._last_payload_key:
            return
        self._last_payload_key = key

        self.last_page = page
        self.last_payload = PowerMeterPage(page, payload[0], toggle, bytes(payload))
        self.pages_received += 1
        self.page_counts[page] = self.page_counts.get(page, 0) + 1

        if "instantaneous_power_watts" not in message:
            return

        self.readings_received += 1
        self.latest = PowerMeterData(
            page=page,
            instantaneous_power_watts=message["instantaneous_power_watts"],
            power_meter_cadence_rpm=message.get("power_meter_cadence_rpm"),
        )