"""ANTManager: the stick's channel set-up (one scan channel for discovery, or one dedicated channel per chosen
device) and routing each message to the ANTDevice it came from.

A thin layer over the native Manager (src/core/ant_manager.cpp), which does the work. It drives whatever `native`
object it's given exactly as this class used to (the real stick through NativeTransport, or the tests' fake), and
takes its time from this module's monotonic() and sleep(), which the tests replace with a fake clock.
"""

import weakref
from time import monotonic, sleep

from . import device as _device_module
from .device import ANTDevice
from .profiles import wrap_profile
from .transport import NativeTransport
from .transport.native import ant_native as _core


#
# Channel modes. "scan" is one continuous RX scan channel that hears every
# device (discovery). "channels" is one dedicated slave channel per chosen
# device (see open_device_channels()) - the only mode the stick can
# transmit in: a scan channel rejects every send with
# INVALID_SCAN_TX_CHANNEL, and continuous scan takes the whole radio so it
# can't run alongside other channels.
#
MODE_SCAN = "scan"
MODE_CHANNELS = "channels"

CHANNEL_TYPE_SCAN = 1
CHANNEL_TYPE_SLAVE = 0x00

#
# ANT+ message periods (1/32768 s units) per device profile (the native
# manager's, Manager::channel_period)
#
CHANNEL_PERIODS = {
    120: 8070,   # heart rate
    11: 8182,    # bicycle power
    17: 8192,    # fitness equipment (FE-C)
    121: 8086,   # combined speed/cadence
    122: 8102,   # cadence
    123: 8118,   # speed
}
DEFAULT_CHANNEL_PERIOD = 8192

#
# Search timeouts, 2.5 s units: a dedicated channel searches forever in low
# priority when its device drops out (Manager::open_slave_channel)
#
LOW_PRIORITY_SEARCH_FOREVER = 0xFF
HIGH_PRIORITY_SEARCH_OFF = 0x00
SEARCH_FOREVER = 0xFF

#
# Our own channel closes make the stick report EVENT_CHANNEL_CLOSED right
# back; they're ignored for this long after a reconfiguration (see
# events_suppressed())
#
RECONFIGURE_EVENT_GRACE_S = 3.0

#
# Closing the scan channel right after opening it leaves the stick refusing
# every later channel change until it's reset (seen on an ANT USBStick2: 0 ms
# fails every time, 10 ms already works), so it's kept open at least this long
#
SCAN_MIN_OPEN_S = 0.1


def _manager_clock():
    return monotonic()


def _manager_sleep(seconds):
    sleep(seconds)


def _sensor_clock():
    return _device_module.monotonic()


class ANTManager:

    def __init__(self, native=None, on_log=None):

        self.native = (
            native
            or NativeTransport()
        )

        #
        # Optional callable(str) for routing this manager's own log lines
        # (device enumeration, channel reapply) to a caller's logging.
        # Never raised through if it fails.
        #
        self.on_log = on_log

        #
        # Recovery test hook: device numbers placed here have their packets
        # discarded before ANTDevice.update(), simulating a device going
        # silent on the ANT link
        #
        self.drop_devices = set()
        self._dropping = frozenset()

        self._callback = None
        self._devices = {}

        # The native manager reaches back through weak references, so it
        # doesn't keep this one alive
        this = weakref.ref(self)

        def sink(message):
            manager = this()
            if manager is not None:
                manager._on_message(message)

        def log(message):
            manager = this()
            if manager is not None:
                manager._log(message)

        self._core = _core.Manager(self.native, sink, _manager_clock, _manager_sleep, _sensor_clock, log)

        #
        # NOTE: this manager does NOT own a RecoveryManager - every caller
        # (AntWorkoutBridge, ant_recovery_test.py) builds and runs its own
        # against it.
        #

    @property
    def device_number(self):
        return self._core.device_number

    @property
    def mode(self):
        """MODE_SCAN, MODE_CHANNELS, or None before the first configure_scan()"""
        return self._core.mode

    @property
    def needs_reset(self):
        """The stick has stopped accepting channel changes - a channel switch and the fall back to the scan channel
        both failed, which only a reset clears (RecoveryManager resets it, see reapply_configuration())"""
        return self._core.needs_reset

    @property
    def _scan_config(self):
        """configure_scan()'s arguments once it succeeded, or None"""
        return self._core.scan_config

    @property
    def devices(self):
        return {device.identity: device for device in self.get_devices()}

    def open(self, baudrate=57600, device_number=None):
        """Opens stick `device_number`, or the first one that opens"""
        return self._core.open(baudrate, device_number)

    def close(self):
        self._core.close()
        self._callback = None

    def configure_scan(self, network_key, network_number=0, channel=0, rf_frequency=57):
        self._core.configure_scan(bytes(network_key), network_number, channel, rf_frequency)

    #
    # Dedicated channels (MODE_CHANNELS)
    #

    def open_device_channels(self, identities):
        """Stop scanning and open one dedicated slave channel per device -
        identities is a list of (device_type, device_number,
        transmission_type), transmission_type 0 matching any. The devices
        get channels 0, 1, 2, ... in order. Requires configure_scan() to
        have run (it holds the network setup). Raises
        ANTConfigurationError if a channel can't be opened, leaving the
        stick back in scan mode.
        """
        self._core.open_device_channels([tuple(identity) for identity in identities])

    def return_to_scan(self):
        """Close every dedicated channel and go back to the scan channel
        (discovery), e.g. for Disconnect and Rescan."""
        self._core.return_to_scan()

    def wants_device_channels(self):
        """True when dedicated channels were asked for but the stick is on
        the scan channel instead (opening them failed). False while
        another thread is reconfiguring."""
        return self._core.wants_device_channels()

    def retry_device_channels(self):
        """Try again to open the dedicated channels that failed to open
        (recovery). Returns True if they're open now. Raises
        ANTConfigurationError like open_device_channels()."""
        return self._core.retry_device_channels()

    def reopen_channel(self, channel):
        """Close and reopen one dedicated channel (recovery)."""
        self._core.reopen_channel(channel)

    def reopen_scan_channel(self):
        """Close and reopen the scan channel (recovery, MODE_SCAN)."""
        self._core.reopen_scan_channel()

    def reapply_configuration(self):
        """Rebuild the whole setup after native.reset() wiped the network
        key and channel assignments - scan mode, or the same dedicated
        channels as before."""
        self._core.reapply_configuration()

    def channel_for(self, device_type, device_number):
        """The dedicated channel a device is on, or None (not in
        MODE_CHANNELS, or not one of the opened devices)."""
        return self._core.channel_for(device_type, device_number)

    def channel_devices(self):
        """{channel: (device_type, device_number, transmission_type)}"""
        return self._core.channel_devices()

    def events_suppressed(self, channel, now=None):
        """Whether a channel event on `channel` (None = unknown channel)
        is still within the grace after our own reconfiguration."""
        return self._core.events_suppressed(channel, now)

    def start(self, callback=None):
        self._callback = callback
        self._core.start()

    def stop(self):
        self._core.stop()
        self._callback = None

    def _log(self, message):
        if self.on_log:
            try:
                self.on_log(message)
            except Exception:
                pass

    def _device(self, sensor):
        """The ANTDevice over a native sensor, the same one every time"""
        key = (sensor.device_number, sensor.device_type, sensor.transmission_type)
        device = self._devices.get(key)
        if device is None:
            device = ANTDevice._wrap(sensor, wrap_profile(sensor.profile))
            self._devices[key] = device
        return device

    def _on_message(self, message):
        if self.drop_devices != self._dropping:
            self._dropping = frozenset(self.drop_devices)
            self._core.set_drop_devices(set(self._dropping))

        sensor, changed, dropped = self._core.on_message(message)
        if dropped:
            return

        device = None
        if sensor is not None:
            device = self._device(sensor)
            device.last_message = message

        if self._callback:
            self._callback(message, device, changed)

    def get_devices(self):
        return [self._device(sensor) for sensor in self._core.get_devices()]
