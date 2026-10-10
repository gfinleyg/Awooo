from dataclasses import dataclass
from .fitness_equipment import FitnessEquipmentProfile


@dataclass(frozen=True)
class TrainerMessage:
    page: int
    raw_page: int
    payload: bytes


class ControllableTrainerProfile(FitnessEquipmentProfile):
    """Receive-side profile state for ANT+ controllable fitness equipment.

    TX is intentionally exposed by the channel/native transport rather than
    embedding command bytes here. This keeps transport/channel operations
    generic and lets trainer command pages be implemented from the selected
    FE-C profile version.
    """

    device_type = 17

    def __init__(self):
        super().__init__()
        self.last_trainer_message = None

    def on_message(self, message):
        super().on_message(message)
        if self.last_raw is not None:
            self.last_trainer_message = TrainerMessage(
                self.last_raw.page, self.last_raw.raw_page, self.last_raw.payload
            )
