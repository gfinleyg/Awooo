# ant_manager_v2.py

# ANTManager v2:
# - device lifecycle tracking
# - stale/lost detection
# - recovery hooks

import time
from threading import RLock, Thread, Event
from enum import Enum


class DeviceState(Enum):
    NEW = "new"
    ACTIVE = "active"
    STALE = "stale"
    LOST = "lost"


DEVICE_STALE_TIME = 5
DEVICE_LOST_TIME = 15


class ANTManagerV2:
    def __init__(self, native):
        self.native = native
        self.devices = {}
        self._lock = RLock()
        self._callback = None

        self._last_message_time = time.monotonic()

        self._stop_event = Event()
        self._watchdog_thread = None

        self._scan_config = None
        self._recovering = False

    def mark_message(self):
        self._last_message_time = time.monotonic()

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

    def watchdog(self):
        while not self._stop_event.wait(2):
            self.check_devices()

            if time.monotonic() - self._last_message_time > 10:
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
                self.configure_scan(**self._scan_config)

            self.native.start_message_pump()

            self._last_message_time = time.monotonic()
            return True

        finally:
            self._recovering = False
