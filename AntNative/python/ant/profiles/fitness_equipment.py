from dataclasses import dataclass

from .base import ANTProfile, core_module


# ANT+ FE-C data pages decoded into `latest` and `capabilities` (every page is
# also decoded field by field into `pages`, see ant.pages)
GENERAL_FE_DATA_PAGE = 0x10
SPECIFIC_TRAINER_DATA_PAGE = 0x19
FE_CAPABILITIES_PAGE = 0x36

# ANT+ FE-C control pages
BASIC_RESISTANCE_PAGE = 0x30      # page 48, 0.5% units
TARGET_POWER_PAGE = 0x31          # page 49, 0.25 W units, 0-4000 W
WIND_RESISTANCE_PAGE = 0x32       # page 50
TRACK_RESISTANCE_PAGE = 0x33      # page 51
USER_CONFIGURATION_PAGE = 0x37    # page 55
TARGET_POWER_MIN_WATTS = 0
TARGET_POWER_MAX_WATTS = 4000


def target_power_payload(watts):
    """8-byte FE-C Target Power (page 49) command for `watts`, clamped to
    the profile's accepted 0-4000 W range."""
    return core_module().page_target_power(watts)


def basic_resistance_payload(percent):
    """8-byte FE-C Basic Resistance (page 48) command for `percent` of the
    trainer's maximum resistance, clamped to 0-100%. Sending it takes the
    trainer out of target power (ERG) mode."""
    return core_module().page_basic_resistance(percent)


def wind_resistance_payload(coefficient_kg_per_m=None, wind_speed_kmh=None, drafting_factor=None):
    """8-byte FE-C Wind Resistance (page 50) command: simulation mode's air resistance. None leaves a field at the
    trainer's default (0.51 kg/m, no wind, no drafting)."""
    return core_module().page_wind_resistance(coefficient_kg_per_m, wind_speed_kmh, drafting_factor)


def track_resistance_payload(grade_percent=None, rolling_resistance=None):
    """8-byte FE-C Track Resistance (page 51) command: simulation mode's slope (-200 to 200%, 0.01% steps) and
    coefficient of rolling resistance. None leaves a field at the trainer's default (flat, 0.004)."""
    return core_module().page_track_resistance(grade_percent, rolling_resistance)


def user_configuration_payload(user_weight_kg=None, bicycle_weight_kg=None, wheel_diameter_m=None, gear_ratio=None,
                               wheel_diameter_offset_mm=None):
    """8-byte FE-C User Configuration (page 55): what simulation mode computes resistance and speed from. None
    sends a field as invalid (the trainer assumes 75 kg rider, 10 kg bike, 0.7 m wheel)."""
    return core_module().page_user_configuration(user_weight_kg, bicycle_weight_kg, wheel_diameter_m, gear_ratio,
                                                 wheel_diameter_offset_mm)


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

    Device type 17. The trainer data page (0x19) becomes `latest` (instantaneous power and cadence) and FE
    Capabilities (0x36) `capabilities`; every page is decoded field by field into `pages` (general FE data with
    speed and distance, settings, metabolic data, treadmill/rower/... data, command status, ...). Control pages are
    built by the *_payload() functions above and sent by the owner of the channel.
    """
    device_type = 17

    def __init__(self):
        self._attach(core_module().FitnessEquipmentProfile())

    def _attach(self, core):
        super()._attach(core)
        self._capabilities = None
        self._capabilities_version = None

    def _reading(self, fields):
        return FitnessEquipmentData(**fields)

    @property
    def capabilities(self):
        version = self._core.capabilities_version
        if version != self._capabilities_version:
            fields = self._core.capabilities
            self._capabilities = None if fields is None else FitnessEquipmentCapabilities(**fields)
            self._capabilities_version = version
        return self._capabilities

    @property
    def last_raw(self):
        raw = self._core.last_raw
        return None if raw is None else FitnessEquipmentPage(*raw)
