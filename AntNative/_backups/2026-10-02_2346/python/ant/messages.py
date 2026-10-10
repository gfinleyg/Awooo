from dataclasses import dataclass
@dataclass(frozen=True)
class ANTMessage:
    message_id:int; data:bytes; channel:int|None; payload:bytes; message_size:int
    device_number:int|None=None; device_type:int|None=None; transmission_type:int|None=None; extended_bitfield:int|None=None
    @property
    def has_device_id(self): return self.device_number is not None
