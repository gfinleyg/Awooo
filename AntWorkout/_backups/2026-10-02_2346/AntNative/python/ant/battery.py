"""ANT+ battery status decoding.

Battery status arrives on different pages depending on the profile, all
sharing the same "descriptive bit field" byte layout:
  bits 0-3  coarse battery voltage (0x0F = invalid)
  bits 4-6  battery status (see BATTERY_STATUS_NAMES)
  bit  7    cumulative operating time resolution (unused here)

  Common Page 82 (0x52) - power meters (11) and fitness equipment (17):
      byte 2 battery identifier (bits 4-7 id, for multi-battery devices
      such as dual-sided power meters), byte 6 fractional voltage (1/256 V),
      byte 7 descriptive bit field.
  HRM page 7 (120):
      byte 1 battery level (0-100%, 0xFF = not supported),
      byte 2 fractional voltage, byte 3 descriptive bit field.
  Speed-only/cadence-only page 4 (122, 123):
      byte 2 fractional voltage, byte 3 descriptive bit field.

Combined speed/cadence sensors (121) use a page-less format and have no way
to report battery status at all.

Decoded here from the raw payload rather than in ant_native.cpp so no
native rebuild is needed; it's a handful of byte reads per page.
"""

from dataclasses import dataclass


COMMON_PAGE_BATTERY = 0x52
HRM_PAGE_BATTERY = 0x07
BSC_PAGE_BATTERY = 0x04

BATTERY_STATUS_NEW = 1
BATTERY_STATUS_GOOD = 2
BATTERY_STATUS_OK = 3
BATTERY_STATUS_LOW = 4
BATTERY_STATUS_CRITICAL = 5
BATTERY_STATUS_INVALID = 7

BATTERY_STATUS_NAMES = {
    BATTERY_STATUS_NEW: "New",
    BATTERY_STATUS_GOOD: "Good",
    BATTERY_STATUS_OK: "Ok",
    BATTERY_STATUS_LOW: "Low",
    BATTERY_STATUS_CRITICAL: "Critical",
}

# Only used when a device reports a percentage but no usable status (some
# HRM straps do this)
LOW_BATTERY_PERCENT = 15

# Device types whose page byte carries the page-change toggle in bit 7
_TOGGLE_PAGE_DEVICE_TYPES = {120, 122, 123}


@dataclass(frozen=True)
class BatteryStatus:
    status: int                      # BATTERY_STATUS_* (0/6/7 = unknown)
    level_percent: int | None = None
    voltage: float | None = None
    battery_id: int = 0

    @property
    def status_name(self):
        return BATTERY_STATUS_NAMES.get(self.status, "Unknown")

    @property
    def is_low(self):
        if self.status in (BATTERY_STATUS_LOW, BATTERY_STATUS_CRITICAL):
            return True
        if self.status not in BATTERY_STATUS_NAMES and self.level_percent is not None:
            return self.level_percent <= LOW_BATTERY_PERCENT
        return False

    @property
    def severity(self):
        """Higher is worse, for picking the weakest of several batteries."""
        if self.status == BATTERY_STATUS_CRITICAL:
            return 3
        if self.is_low:
            return 2
        if self.status in BATTERY_STATUS_NAMES:
            return 1
        return 0


def _voltage(fractional, descriptive):
    coarse = descriptive & 0x0F
    if coarse == 0x0F:
        return None
    return coarse + fractional / 256.0


def decode_battery(device_type, payload):
    """Return a BatteryStatus if `payload` is a battery status page for
    `device_type`, else None."""
    if payload is None or len(payload) < 8 or device_type == 121:
        return None
    payload = bytes(payload)

    page = payload[0]
    if device_type in _TOGGLE_PAGE_DEVICE_TYPES:
        page &= 0x7F

    if page == COMMON_PAGE_BATTERY:
        descriptive = payload[7]
        return BatteryStatus(
            status=(descriptive >> 4) & 0x07,
            voltage=_voltage(payload[6], descriptive),
            battery_id=(payload[2] >> 4) & 0x0F if payload[2] != 0xFF else 0,
        )

    if device_type == 120 and page == HRM_PAGE_BATTERY:
        descriptive = payload[3]
        level = payload[1] if payload[1] <= 100 else None
        return BatteryStatus(
            status=(descriptive >> 4) & 0x07,
            level_percent=level,
            voltage=_voltage(payload[2], descriptive),
        )

    if device_type in (122, 123) and page == BSC_PAGE_BATTERY:
        descriptive = payload[3]
        return BatteryStatus(
            status=(descriptive >> 4) & 0x07,
            voltage=_voltage(payload[2], descriptive),
        )

    return None
