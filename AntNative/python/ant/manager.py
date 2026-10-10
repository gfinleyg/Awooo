from threading import RLock
from time import monotonic, sleep

from .device import ANTDevice
from .exceptions import (
    ANTNotOpenError,
    ANTConfigurationError
)

from .profiles import (
    PROFILE_CLASSES,
    SpeedCadenceProfile
)

from .transport import NativeTransport


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
# ANT+ message periods (1/32768 s units) per device profile
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
# Search timeouts, 2.5 s units. A dedicated channel whose device drops out
# returns to searching on its own; searching forever (0xFF) in low priority
# keeps the stick from closing the channel and doesn't disturb the other
# channels' reception the way a high priority search would.
#
LOW_PRIORITY_SEARCH_FOREVER = 0xFF
HIGH_PRIORITY_SEARCH_OFF = 0x00
SEARCH_FOREVER = 0xFF

#
# Our own channel closes (switching modes, reopening a channel) make the
# stick report EVENT_CHANNEL_CLOSED right back; RecoveryManager ignores
# those for this long after a reconfiguration (see _suppress_events_until).
#
RECONFIGURE_EVENT_GRACE_S = 3.0

#
# Closing the scan channel right after opening it (both within the same
# millisecond, as a full reapply after a stick reset used to do) leaves the
# stick refusing every later channel change - open_channel answers
# CHANNEL_IN_WRONG_STATE and open_rx_scan_mode CLOSE_ALL_CHANNELS - until
# it's reset again. Seen on an ANT USBStick2: 0ms fails every time, 10ms
# already works; this leaves a wide margin.
#
SCAN_MIN_OPEN_S = 0.1


class ANTManager:


    def __init__(self, native=None, on_log=None):

        self.native = (
            native
            or NativeTransport()
        )

        self.devices = {}

        self.device_number = None

        #
        # Recovery test hook:
        #
        # Device numbers placed here will have
        # packets discarded before ANTDevice.update()
        #
        self.drop_devices = set()

        self._lock = RLock()

        self._callback = None

        # Set by configure_scan() once it succeeds - RecoveryManager reads
        # this (as self.manager._scan_config) to reopen the same channel
        # on a tier-1 retry, or to fully reapply it after a tier-2 device
        # reset (native.reset() wipes the network key/channel assignment,
        # so it has to be redone from scratch either way). None until the
        # first successful configure_scan().
        self._scan_config = None

        #
        # Current channel mode (MODE_SCAN/MODE_CHANNELS, None before the
        # first configure_scan()) and, in MODE_CHANNELS, the dedicated
        # channel -> (device_type, device_number, transmission_type) each
        # device is on. Every channel (re)configuration runs under
        # _config_lock, since the bridge thread and RecoveryManager's
        # thread both reconfigure.
        #
        self.mode = None
        self._channel_ids = {}
        self._config_lock = RLock()

        #
        # When the scan channel was last opened (see SCAN_MIN_OPEN_S), and
        # whether the stick has stopped accepting channel changes - a
        # channel switch and the fall back to the scan channel both failed,
        # which only a reset clears (RecoveryManager resets it, see
        # reapply_configuration()).
        #
        self._scan_opened_at = 0.0
        self.needs_reset = False

        #
        # EVENT_CHANNEL_CLOSED grace after our own reconfiguration (see
        # events_suppressed()): until when for every channel (mode
        # switches, full reapply), and per channel for a single channel's
        # reopen, so reopening one channel doesn't hide a real closure on
        # another.
        #
        self._suppress_events_until = 0.0
        self._suppress_channel_until = {}

        #
        # The dedicated channels asked for by the last
        # open_device_channels(), kept while they're wanted even if
        # opening them failed and the stick fell back to scan mode, so
        # retry_device_channels() and reapply_configuration() know what to
        # reopen. Cleared by return_to_scan().
        #
        self._wanted_channels = None

        #
        # Optional callable(str) for routing this manager's own log
        # lines (device enumeration, currently) to a caller's logging
        # instead of stdout. Never raised through if it fails.
        #
        self.on_log = on_log

        #
        # NOTE: this manager does NOT own a RecoveryManager. It used to
        # construct one here and start()/stop() it alongside the message
        # pump, but every real caller (AntWorkoutBridge, the standalone
        # ant_recovery_test.py) already builds and runs its own
        # RecoveryManager against this same manager - so that internal
        # instance was a second, untuned (default 1.5s/3s/7s/7s timing,
        # no on_event wired up) watchdog running silently in parallel
        # the whole time, able to independently retry/reset the same
        # channel the caller's own (properly tuned) RecoveryManager was
        # already managing. Removed rather than fixed in place, since
        # nothing outside this file ever referenced self.recovery.


    def open(
        self,
        baudrate=57600,
        device_number=None
    ):

        #
        # Explicit device requested
        #
        if device_number is not None:

            result = self.native.open(
                baudrate,
                device_number
            )

            if result:
                self.device_number = device_number

            return result


        #
        # Automatic device selection
        #
        devices = self.native.list_devices()

        for device in devices:

            index = device["index"]

            self._log(
                f"Trying ANT device {index}: "
                f"{device.get('product_description')}"
            )

            if self.native.open(
                baudrate,
                index
            ):

                self.device_number = index

                self._log(
                    f"Selected ANT device {index}"
                )

                return True


        return False



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


        if not self.native.is_open():

            raise ANTNotOpenError(
                "ANT device is not open"
            )

        with self._config_lock:

            self._configure_scan(network_key, network_number, channel, rf_frequency)



    def _configure_scan(self, network_key, network_number, channel, rf_frequency):

        steps = [

            (
                self.native.reset,
                ()
            ),

            (
                self.native.set_network_key,
                (
                    network_number,
                    bytes(network_key)
                )
            ),

            (
                self.native.assign_channel,
                (
                    channel,
                    1,
                    network_number
                )
            ),

            (
                self.native.set_channel_id,
                (
                    channel,
                    0,
                    0,
                    0
                )
            ),

            (
                self.native.set_channel_rf_frequency,
                (
                    channel,
                    rf_frequency
                )
            ),

            (
                self.native.set_rx_extended_messages_enabled,
                (True,)
            ),

            (
                self.native.open_rx_scan_mode,
                ()
            )

        ]


        for fn,args in steps:

            if not fn(*args):

                raise ANTConfigurationError(
                    "ANT scan configuration failed"
                )

        # Recorded as kwargs matching this method's own signature, since
        # _attempt_device_reset() calls self.manager.configure_scan(
        # **scan_config) to reapply it verbatim after a full reset.
        self._scan_config = {
            "network_key": network_key,
            "network_number": network_number,
            "channel": channel,
            "rf_frequency": rf_frequency,
        }

        self.mode = MODE_SCAN
        self._channel_ids = {}
        self._scan_opened_at = monotonic()
        self.needs_reset = False



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

        if self._scan_config is None:

            raise ANTConfigurationError(
                "open_device_channels() needs configure_scan() first"
            )

        with self._config_lock:

            self._wanted_channels = [tuple(i) for i in identities]

            if self.mode == MODE_SCAN:

                wait = SCAN_MIN_OPEN_S - (monotonic() - self._scan_opened_at)

                if wait > 0:
                    sleep(wait)

            self._suppress_events()

            self._close_all_channels()

            self.mode = MODE_CHANNELS

            try:

                for channel, identity in enumerate(identities):

                    self._open_slave_channel(channel, identity)

                    self._channel_ids[channel] = tuple(identity)

            except Exception as e:

                self._close_all_channels()

                try:

                    self._open_scan_channel()

                except ANTConfigurationError as scan_error:

                    #
                    # The stick won't take any channel change now - left
                    # looking like scan mode (so the dedicated channels are
                    # still wanted, see wants_device_channels()), flagged
                    # for RecoveryManager to reset it
                    #
                    self.mode = MODE_SCAN
                    self._channel_ids = {}
                    self.needs_reset = True

                    raise ANTConfigurationError(
                        f"{e}; falling back to the scan channel also "
                        f"failed ({scan_error}), the stick needs a reset"
                    ) from e

                raise



    def return_to_scan(self):
        """Close every dedicated channel and go back to the scan channel
        (discovery), e.g. for Disconnect and Rescan."""

        with self._config_lock:

            self._wanted_channels = None

            if self.mode != MODE_CHANNELS:
                return

            self._suppress_events()

            self._close_all_channels()

            try:

                self._open_scan_channel()

            except ANTConfigurationError:

                #
                # Same as a failed fall back in open_device_channels()
                #
                self.mode = MODE_SCAN
                self._channel_ids = {}
                self.needs_reset = True

                raise



    def wants_device_channels(self):
        """True when dedicated channels were asked for but the stick is on
        the scan channel instead (opening them failed). False while
        another thread is reconfiguring - mid switch, they're wanted and
        the stick is still on the scan channel, but nothing has failed."""

        if not self._config_lock.acquire(blocking=False):
            return False

        try:
            return self.mode == MODE_SCAN and self._wanted_channels is not None
        finally:
            self._config_lock.release()



    def retry_device_channels(self):
        """Try again to open the dedicated channels that failed to open
        (recovery). Returns True if they're open now. Raises
        ANTConfigurationError like open_device_channels()."""

        with self._config_lock:

            #
            # Re-checked under the lock - a disconnect (return_to_scan) on
            # the bridge thread may have landed since the caller looked
            #
            if not self.wants_device_channels():
                return self.mode == MODE_CHANNELS

            self.open_device_channels(self._wanted_channels)

            return True



    def reopen_channel(self, channel):
        """Close and reopen one dedicated channel (recovery)."""

        with self._config_lock:

            identity = self._channel_ids.get(channel)

            if self.mode != MODE_CHANNELS or identity is None:
                return

            self._suppress_events(channel)

            self.native.close_channel(channel)
            self.native.unassign_channel(channel)

            self._open_slave_channel(channel, identity)



    def reopen_scan_channel(self):
        """Close and reopen the scan channel (recovery, MODE_SCAN)."""

        with self._config_lock:

            if self.mode != MODE_SCAN:
                return

            channel = self._scan_config["channel"] if self._scan_config else 0

            self._suppress_events(channel)

            self.native.close_channel(channel)
            self.native.unassign_channel(channel)

            self._open_scan_channel()



    def reapply_configuration(self):
        """Rebuild the whole setup after native.reset() wiped the network
        key and channel assignments - scan mode, or the same dedicated
        channels as before."""

        with self._config_lock:

            #
            # The wanted channels, not just the open ones - a reset is
            # also a chance to get dedicated channels that failed to open
            # earlier going
            #
            identities = self._wanted_channels

            self._configure_scan(**self._scan_config)

            if identities:

                #
                # A failure here leaves the stick working in scan mode -
                # don't raise out of the reset over it (the caller still
                # has to restart the message pump), retry_device_channels()
                # picks it up from there.
                #
                try:

                    self.open_device_channels(identities)

                except ANTConfigurationError as e:

                    self._log(
                        f"Dedicated channels failed to reopen after reset, "
                        f"staying on the scan channel: {e}"
                    )



    def channel_for(self, device_type, device_number):
        """The dedicated channel a device is on, or None (not in
        MODE_CHANNELS, or not one of the opened devices)."""

        for channel, identity in list(self._channel_ids.items()):

            if identity[0] == device_type and identity[1] == device_number:

                return channel

        return None



    def channel_devices(self):
        """{channel: (device_type, device_number, transmission_type)}"""

        return dict(self._channel_ids)



    def _suppress_events(self, channel=None):
        """Start the EVENT_CHANNEL_CLOSED grace for one channel, or for
        every channel when None."""

        until = monotonic() + RECONFIGURE_EVENT_GRACE_S

        if channel is None:

            self._suppress_events_until = until
            self._suppress_channel_until = {}

        else:

            self._suppress_channel_until[channel] = until



    def events_suppressed(self, channel, now=None):
        """Whether a channel event on `channel` (None = unknown channel)
        is still within the grace after our own reconfiguration."""

        if now is None:
            now = monotonic()

        if now < self._suppress_events_until:
            return True

        if channel is None:

            return any(
                now < until for until in self._suppress_channel_until.values()
            )

        return now < self._suppress_channel_until.get(channel, 0.0)



    def _close_all_channels(self):

        #
        # Best-effort: a channel may already be closed/unassigned
        #
        channels = set(self._channel_ids)

        if self._scan_config is not None:
            channels.add(self._scan_config["channel"])

        for channel in sorted(channels):

            self.native.close_channel(channel)
            self.native.unassign_channel(channel)

        self._channel_ids = {}



    def _open_scan_channel(self):

        config = self._scan_config

        channel = config["channel"]

        ok = (
            self.native.assign_channel(
                channel, CHANNEL_TYPE_SCAN, config["network_number"]
            )
            and self.native.set_channel_id(channel, 0, 0, 0)
            and self.native.set_channel_rf_frequency(
                channel, config["rf_frequency"]
            )
            and self.native.set_rx_extended_messages_enabled(True)
            and self.native.open_rx_scan_mode()
        )

        if not ok:

            raise ANTConfigurationError(
                "ANT scan channel configuration failed"
            )

        self.mode = MODE_SCAN
        self._channel_ids = {}
        self._scan_opened_at = monotonic()



    def _open_slave_channel(self, channel, identity):

        device_type, device_number, transmission_type = identity

        config = self._scan_config

        ok = (
            self.native.assign_channel(
                channel, CHANNEL_TYPE_SLAVE, config["network_number"]
            )
            and self.native.set_channel_id(
                channel, device_number, device_type, transmission_type
            )
            and self.native.set_channel_period(
                channel,
                CHANNEL_PERIODS.get(device_type, DEFAULT_CHANNEL_PERIOD)
            )
            and self.native.set_channel_rf_frequency(
                channel, config["rf_frequency"]
            )
        )

        if not ok:

            raise ANTConfigurationError(
                f"ANT channel {channel} configuration failed for "
                f"device type={device_type} number={device_number}"
            )

        #
        # Search forever in low priority. A stick without low priority
        # search support falls back to searching forever in high priority.
        #
        if not (
            self.native.set_low_priority_search_timeout(
                channel, LOW_PRIORITY_SEARCH_FOREVER
            )
            and self.native.set_search_timeout(
                channel, HIGH_PRIORITY_SEARCH_OFF
            )
        ):

            self.native.set_search_timeout(channel, SEARCH_FOREVER)

        if not self.native.open_channel(channel):

            raise ANTConfigurationError(
                f"ANT channel {channel} failed to open for "
                f"device type={device_type} number={device_number}"
            )



    def start(
        self,
        callback=None
    ):

        self._callback = callback


        self.native.start_message_pump(
            self._on_message
        )



    def stop(self):

        self.native.stop_message_pump()

        self._callback = None



    def _log(self, message):

        if self.on_log:
            try:
                self.on_log(message)
            except Exception:
                pass



    def _on_message(
        self,
        message
    ):


        device = None

        identity = None


        if message.get(
            "has_device_id"
        ):


            identity = (

                message.get(
                    "device_number"
                ),

                message.get(
                    "device_type"
                ),

                message.get(
                    "transmission_type"
                )

            )

        elif "payload" in message and message.get("channel") in self._channel_ids:

            #
            # A dedicated channel only ever receives its own device, so a
            # data message without extended device ID still belongs to it
            # (as long as its full ID, transmission type included, is known)
            #
            device_type, device_number, transmission_type = self._channel_ids[message["channel"]]

            if transmission_type:

                identity = (device_number, device_type, transmission_type)


        if identity is not None:


            if all(
                x is not None
                for x in identity
            ):


                with self._lock:


                    device = self.devices.get(
                        identity
                    )


                    if device is None:


                        profile_cls = next(
                            (
                                cls
                                for cls
                                in PROFILE_CLASSES
                                if cls.can_handle(
                                    identity[1]
                                )
                            ),
                            None
                        )


                        if profile_cls is SpeedCadenceProfile:

                            profile = profile_cls(
                                identity[1]
                            )

                        elif profile_cls:

                            profile = profile_cls()

                        else:

                            profile = None



                        device = ANTDevice(

                            identity[0],
                            identity[1],
                            identity[2],
                            int(
                                message.get(
                                    "channel",
                                    0
                                )
                            ),

                            profile

                        )


                        self.devices[
                            identity
                        ] = device



                    #
                    # Recovery test packet filter
                    #
                    # Drop BEFORE update().
                    #
                    # This simulates a device
                    # going silent on the ANT link.
                    #

                    if device.device_number in self.drop_devices:

                        return



                    #
                    # A device heard on the scan channel moves to its own
                    # channel in MODE_CHANNELS
                    #
                    if "channel" in message:
                        device.channel = int(message["channel"])

                    changed = device.update(message)



                if self._callback:

                    self._callback(
                        message,
                        device,
                        changed
                    )


                return



        if self._callback:

            self._callback(
                message,
                device,
                False
            )



    def get_devices(self):

        with self._lock:

            return list(
                self.devices.values()
            )