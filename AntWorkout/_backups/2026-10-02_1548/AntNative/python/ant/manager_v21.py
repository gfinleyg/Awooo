"""
AntNative manager_v21.py

Drop-in evolution of manager.py:
- preserves ANTManager behavior
- adds watchdog
- device lifecycle tracking
- stale/lost state
- recovery hooks
- health reporting

Replace/import this instead of manager.py when testing.
"""

import time
from enum import Enum
from threading import RLock, Thread, Event

from .device import ANTDevice
from .exceptions import ANTNotOpenError, ANTConfigurationError
from .profiles import PROFILE_CLASSES, SpeedCadenceProfile
from .transport import NativeTransport


class DeviceState(Enum):
    NEW = "new"
    ACTIVE = "active"
    STALE = "stale"
    LOST = "lost"


DEVICE_STALE_TIME = 5
DEVICE_LOST_TIME = 15


class ANTManager:

    def __init__(self, native=None):
        self.native = native or NativeTransport()
        self.devices = {}
        self._lock = RLock()
        self._callback = None

        self._last_message_time = time.monotonic()

        self._scan_config = None

        self._stop_event = Event()
        self._watchdog_thread = None
        self._recovering = False

        self.recovery_count = 0
        self.message_count = 0


    def open(self, baudrate=57600, device_number=0):
        return self.native.open(
            baudrate,
            device_number
        )


    def close(self):
        self.stop()
        self.native.close()


    def configure_scan(
        self,
        network_key,
        network_number=0,
        channel=0,
        rf_frequency=57
    ):

        self._scan_config = {
            "network_key": bytes(network_key),
            "network_number": network_number,
            "channel": channel,
            "rf_frequency": rf_frequency
        }

        if not self.native.is_open():
            raise ANTNotOpenError(
                "ANT device is not open"
            )

        steps = [
            (self.native.reset, ()),
            (self.native.set_network_key,
             (network_number, bytes(network_key))),
            (self.native.assign_channel,
             (channel, 1, network_number)),
            (self.native.set_channel_id,
             (channel, 0, 0, 0)),
            (self.native.set_channel_rf_frequency,
             (channel, rf_frequency)),
            (self.native.set_rx_extended_messages_enabled,
             (True,)),
            (self.native.open_rx_scan_mode, ())
        ]

        for fn, args in steps:
            if not fn(*args):
                raise ANTConfigurationError(
                    "ANT configuration failed"
                )


    def start(self, callback=None):

        self._callback = callback

        self._stop_event.clear()

        self.native.start_message_pump(
            self._on_message
        )

        self._watchdog_thread = Thread(
            target=self._watchdog,
            daemon=True
        )

        self._watchdog_thread.start()


    def stop(self):

        self._stop_event.set()

        self.native.stop_message_pump()

        self._callback = None


    def _on_message(self, message):

        self.message_count += 1
        self._last_message_time = time.monotonic()

        device = None

        if message.get("has_device_id"):

            identity = (
                message.get("device_number"),
                message.get("device_type"),
                message.get("transmission_type")
            )

            if all(v is not None for v in identity):

                with self._lock:

                    device = self.devices.get(identity)

                    if device is None:

                        profile_cls = next(
                            (
                                cls for cls in PROFILE_CLASSES
                                if cls.can_handle(identity[1])
                            ),
                            None
                        )

                        profile = None

                        if profile_cls:
                            if profile_cls is SpeedCadenceProfile:
                                profile = profile_cls(identity[1])
                            else:
                                profile = profile_cls()

                        device = ANTDevice(
                            identity[0],
                            identity[1],
                            identity[2],
                            int(message.get("channel", 0)),
                            profile
                        )

                        self.devices[identity] = device

                    device.update(message)

                    device.state = DeviceState.ACTIVE

                if self._callback:
                    self._callback(message, device)

                return

        if self._callback:
            self._callback(message, None)


    def check_devices(self):

        now = time.monotonic()

        with self._lock:

            for device in self.devices.values():

                age = now - device.last_seen

                if age > DEVICE_LOST_TIME:
                    device.state = DeviceState.LOST

                elif age > DEVICE_STALE_TIME:
                    device.state = DeviceState.STALE

                else:
                    device.state = DeviceState.ACTIVE


    def _watchdog(self):

        while not self._stop_event.wait(2):

            self.check_devices()

            if (
                time.monotonic()
                -
                self._last_message_time
                > 10
            ):
                self.recover()


    def recover(self):

        if self._recovering:
            return False

        self._recovering = True

        try:
            print("ANT recovery")

            self.native.stop_message_pump()

            self.native.reset()

            if self._scan_config:
                self.configure_scan(
                    **self._scan_config
                )

            self.native.start_message_pump(
                self._on_message
            )

            self.recovery_count += 1
            self._last_message_time = time.monotonic()

            return True

        except Exception as e:
            print("ANT recovery failed:", e)
            return False

        finally:
            self._recovering = False


    def health(self):

        with self._lock:

            return {
                "messages": self.message_count,
                "devices": len(self.devices),
                "recoveries": self.recovery_count,
                "last_message_age":
                    time.monotonic() -
                    self._last_message_time,
                "device_states": {
                    str(d.identity):
                    d.state.value
                    for d in self.devices.values()
                }
            }


    def get_devices(self):

        with self._lock:
            return list(self.devices.values())
