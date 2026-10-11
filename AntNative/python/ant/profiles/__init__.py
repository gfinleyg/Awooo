from .base import ANTProfile
from .heart_rate import HeartRateProfile, HeartRateData, HeartRatePage
from .speed_cadence import SpeedCadenceProfile, SpeedCadenceData, SpeedCadencePage
from .power_meter import PowerMeterProfile, PowerMeterPage
from .fitness_equipment import (
    FitnessEquipmentProfile, FitnessEquipmentPage, FitnessEquipmentData,
    FitnessEquipmentCapabilities, target_power_payload,
    TARGET_POWER_MIN_WATTS, TARGET_POWER_MAX_WATTS,
)

PROFILE_CLASSES = [
    HeartRateProfile,
    SpeedCadenceProfile,
    PowerMeterProfile,
    FitnessEquipmentProfile,
]

__all__ = [
    "ANTProfile", "HeartRateProfile", "HeartRateData", "HeartRatePage",
    "SpeedCadenceProfile", "SpeedCadenceData", "SpeedCadencePage",
    "PowerMeterProfile", "PowerMeterPage",
    "FitnessEquipmentProfile", "FitnessEquipmentPage", "FitnessEquipmentData",
    "FitnessEquipmentCapabilities", "target_power_payload",
    "TARGET_POWER_MIN_WATTS", "TARGET_POWER_MAX_WATTS",
    "PROFILE_CLASSES",
]
