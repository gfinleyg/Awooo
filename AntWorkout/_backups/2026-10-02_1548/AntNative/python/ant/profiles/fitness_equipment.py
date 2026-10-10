from dataclasses import dataclass
from .base import ANTProfile


# ANT+ FE-C data pages decoded here
GENERAL_FE_DATA_PAGE = 0x10
SPECIFIC_TRAINER_DATA_PAGE = 0x19
FE_CAPABILITIES_PAGE = 0x36

# ANT+ FE-C control page: Target Power (page 49), 0.25 W units. The
# profile's accepted target range is 0-4000 W.
TARGET_POWER_PAGE = 0x31
TARGET_POWER_MIN_WATTS = 0
TARGET_POWER_MAX_WATTS = 4000


def target_power_payload(watts):
    """8-byte FE-C Target Power (page 49) command for `watts`, clamped to
    the profile's accepted 0-4000 W range."""
    watts = max(TARGET_POWER_MIN_WATTS, min(TARGET_POWER_MAX_WATTS, watts))
    quarter_watts = int(round(watts * 4))
    return bytes([
        TARGET_POWER_PAGE,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        quarter_watts & 0xFF, (quarter_watts >> 8) & 0xFF,
    ])


@dataclass(frozen=True)
class FitnessEquipmentPage:
    page: int
    raw_page: int
    page_change_toggle: bool
    payload: bytes


@dataclass(frozen=True)
class FitnessEquipmentData:
    """Decoded Specific Trainer/Stationary Bike Data (page 0x19). Field
    names match PowerMeterData so power-role consumers treat an FE-C
    trainer and a power meter alike - including using its cadence when a
    dedicated cadence sensor drops out."""
    page: int
    instantaneous_power_watts: int
    # None when the trainer reports 0xFF (cadence not available)
    power_meter_cadence_rpm: int | None = None
    # Trainer status bits (byte 6, bits 4-7): bicycle power calibration,
    # resistance calibration or user configuration required
    trainer_status: int = 0


@dataclass(frozen=True)
class FitnessEquipmentCapabilities:
    """FE Capabilities (page 0x36)."""
    # Newtons, None when not reported
    maximum_resistance_newtons: int | None
    supports_basic_resistance: bool
    supports_target_power: bool
    supports_simulation: bool


class FitnessEquipmentProfile(ANTProfile):
    """ANT+ Fitness Equipment (FE-C) receive-side profile.

    Device type 17. Every page is kept raw (last_raw) so further decoders
    can be added independently; the trainer data page (0x19) is decoded
    into `latest` (instantaneous power and cadence) and FE Capabilities
    (0x36) into `capabilities`. Control pages (Target Power) are built by
    target_power_payload() and transmitted by the owner of the channel.
    """
    device_type = 17

    def __init__(self):
        self.latest = None
        self.last_raw = None
        self.last_page = None
        self.pages_received = 0
        # New trainer data (page 0x19) readings only - see PowerMeterProfile
        self.readings_received = 0
        self.capabilities = None
        self.page_counts = {}
        self._last_payload_key = None

    def on_message(self, message):
        payload = message.get("payload", b"")
        if len(payload) < 8:
            return
        raw_page = payload[0]
        page = raw_page & 0x7F
        toggle = bool(raw_page & 0x80)
        key = (page, bytes(payload))
        if key == self._last_payload_key:
            return
        self._last_payload_key = key
        self.last_page = page
        self.pages_received += 1
        self.page_counts[page] = self.page_counts.get(page, 0) + 1
        self.last_raw = FitnessEquipmentPage(page, raw_page, toggle, bytes(payload))

        if page == SPECIFIC_TRAINER_DATA_PAGE:
            self._decode_trainer_data(payload)
        elif page == FE_CAPABILITIES_PAGE:
            self._decode_capabilities(payload)

    def _decode_trainer_data(self, payload):
        # byte 2 cadence (RPM), bytes 5-6 bits 0-11 instantaneous power (W),
        # 0xFFF meaning invalid
        watts = payload[5] | ((payload[6] & 0x0F) << 8)
        if watts == 0xFFF:
            return
        cadence = payload[2]
        self.readings_received += 1
        self.latest = FitnessEquipmentData(
            page=SPECIFIC_TRAINER_DATA_PAGE,
            instantaneous_power_watts=watts,
            power_meter_cadence_rpm=None if cadence == 0xFF else cadence,
            trainer_status=(payload[6] >> 4) & 0x0F,
        )

    def _decode_capabilities(self, payload):
        max_resistance = payload[5] | (payload[6] << 8)
        flags = payload[7]
        self.capabilities = FitnessEquipmentCapabilities(
            maximum_resistance_newtons=None if max_resistance == 0xFFFF else max_resistance,
            supports_basic_resistance=bool(flags & 0x01),
            supports_target_power=bool(flags & 0x02),
            supports_simulation=bool(flags & 0x04),
        )
