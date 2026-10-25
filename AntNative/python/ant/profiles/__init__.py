from .base import ANTProfile
from .heart_rate import HeartRateProfile, HeartRateData, HeartRatePage
from .speed_cadence import SpeedCadenceProfile, SpeedCadenceData, CombinedSpeedCadenceData, SpeedCadencePage
from .power_meter import PowerMeterProfile, PowerMeterPage, PowerMeterData
from .fitness_equipment import (
    FitnessEquipmentProfile, FitnessEquipmentPage, FitnessEquipmentData,
    FitnessEquipmentCapabilities, target_power_payload, basic_resistance_payload,
    wind_resistance_payload, track_resistance_payload, user_configuration_payload,
    TARGET_POWER_MIN_WATTS, TARGET_POWER_MAX_WATTS,
)

PROFILE_CLASSES = [
    HeartRateProfile,
    SpeedCadenceProfile,
    PowerMeterProfile,
    FitnessEquipmentProfile,
]


def wrap_profile(core):
    """The profile class over a native profile (a sensor's, created by the native manager), or None"""
    if core is None:
        return None
    cls = next((cls for cls in PROFILE_CLASSES if cls.can_handle(core.device_type)), None)
    return cls._wrap(core) if cls else None


__all__ = [
    "ANTProfile", "HeartRateProfile", "HeartRateData", "HeartRatePage",
    "SpeedCadenceProfile", "SpeedCadenceData", "CombinedSpeedCadenceData", "SpeedCadencePage",
    "PowerMeterProfile", "PowerMeterPage", "PowerMeterData",
    "FitnessEquipmentProfile", "FitnessEquipmentPage", "FitnessEquipmentData",
    "FitnessEquipmentCapabilities", "target_power_payload", "basic_resistance_payload",
    "wind_resistance_payload", "track_resistance_payload", "user_configuration_payload",
    "TARGET_POWER_MIN_WATTS", "TARGET_POWER_MAX_WATTS",
    "PROFILE_CLASSES", "wrap_profile",
]
