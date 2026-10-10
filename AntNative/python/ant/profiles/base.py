from abc import ABC, abstractmethod
class ANTProfile(ABC):
    device_type=None
    @classmethod
    def can_handle(cls,device_type): return cls.device_type==device_type
    @abstractmethod
    def on_message(self,message): ...
