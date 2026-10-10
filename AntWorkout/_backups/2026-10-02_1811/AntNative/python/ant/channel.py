from dataclasses import dataclass
from .exceptions import ANTConfigurationError
@dataclass
class ANTChannel:
    number:int; channel_type:int=0x00; network_number:int=0
    def configure_scan(self,native,rf_frequency=57):
        if not native.assign_channel(self.number,self.channel_type,self.network_number): raise ANTConfigurationError('channel assignment failed')
        if not native.set_channel_id(self.number,0,0,0): raise ANTConfigurationError('channel ID configuration failed')
        if not native.set_channel_rf_frequency(self.number,rf_frequency): raise ANTConfigurationError('RF configuration failed')
