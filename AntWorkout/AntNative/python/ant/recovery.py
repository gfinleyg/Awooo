"""
ANTNative Recovery Manager

Device watchdog state machine.

States:

ACTIVE
   |
   v
STALE
   |
   v
LOST
   |
   v
RECOVERED
   |
   v
ACTIVE

Escalation when a watched device goes LOST:
  dedicated channels - reopen its channel once, then reset the stick once
                       if it's still silent (and no other watched device
                       is being heard)
  scan channel       - channel retries, then a stick reset

A thin layer over the native Recovery (src/core/ant_recovery.cpp), which does
the work; time comes from this module's monotonic(), which the tests replace
with a fake clock. Recovery state is stored on ANTDevice.
"""

import weakref
from time import monotonic

from .manager import MODE_CHANNELS  # noqa: F401  (kept for importers)
from .transport.native import ant_native as _core


#
# ANT+ device types that get a value-plausibility check (see the native
# Recovery::frozen_value_is_suspicious)
#
DEVICE_TYPE_POWER = 11
DEVICE_TYPE_HRM = 120
DEVICE_TYPE_SPEED_CADENCE_COMBINED = 121
DEVICE_TYPE_CADENCE = 122
DEVICE_TYPE_SPEED = 123

_OPTIONS = ("stale_timeout", "lost_timeout", "interval", "channel_retry_limit", "channel_retry_cooldown",
            "device_reset_timeout", "device_reset_cooldown", "channel_reopen_wait", "channel_stick_reset_cooldown",
            "channel_open_retry_interval", "hr_frozen_timeout", "value_frozen_timeout")


def _recovery_clock():
    return monotonic()


def _option(name):
    return property(lambda self: self._core.options()[name],
                    lambda self, value: self._core.set_options({name: value}))


class RecoveryManager:

    #
    # RESPONSE_EVENT names (decoded in the native transport) worth reacting
    # to sooner than the silence timer would notice
    #
    URGENT_RETRY_EVENTS = frozenset((
        "EVENT_RX_FAIL_GO_TO_SEARCH",
        "EVENT_RX_SEARCH_TIMEOUT",
    ))

    URGENT_RESET_EVENTS = frozenset((
        "EVENT_CHANNEL_CLOSED",
    ))

    #
    # A queue that briefly filled up (it does right as several devices are
    # discovered at once) isn't a broken link, and a channel reopen doesn't
    # drain a queue - logged by the caller, never acted on
    #
    NON_ACTIONABLE_QUEUE_EVENTS = frozenset((
        "EVENT_SERIAL_QUE_OVERFLOW",
        "EVENT_QUE_OVERFLOW",
    ))

    ACTIONABLE_EVENTS = URGENT_RETRY_EVENTS | URGENT_RESET_EVENTS

    #
    # Dedicated channels: a channel that closes on its own gets reopened.
    # EVENT_RX_FAIL_GO_TO_SEARCH is routine there, never acted on.
    #
    CHANNEL_CLOSED_EVENTS = frozenset((
        "EVENT_CHANNEL_CLOSED",
        "EVENT_RX_SEARCH_TIMEOUT",
    ))

    def __init__(
        self,
        manager,
        stale_timeout=1.5,
        lost_timeout=3,
        interval=1,
        channel_retry_limit=2,
        channel_retry_cooldown=5,
        device_reset_timeout=7,
        device_reset_cooldown=7,
        channel_reopen_wait=5,
        channel_stick_reset_cooldown=30,
        channel_open_retry_interval=10,
        hr_frozen_timeout=10,
        value_frozen_timeout=6,
        on_event=None
    ):
        self.manager = manager

        #
        # Optional callable(str) for surfacing recovery events to a caller's
        # own logging. Never allowed to break recovery if it raises.
        #
        self.on_event = on_event

        options = dict(stale_timeout=stale_timeout, lost_timeout=lost_timeout, interval=interval,
                       channel_retry_limit=channel_retry_limit, channel_retry_cooldown=channel_retry_cooldown,
                       device_reset_timeout=device_reset_timeout, device_reset_cooldown=device_reset_cooldown,
                       channel_reopen_wait=channel_reopen_wait,
                       channel_stick_reset_cooldown=channel_stick_reset_cooldown,
                       channel_open_retry_interval=channel_open_retry_interval,
                       hr_frozen_timeout=hr_frozen_timeout, value_frozen_timeout=value_frozen_timeout)

        this = weakref.ref(self)

        def log(message):
            recovery = this()
            if recovery is not None:
                recovery._log(message)

        self._core = _core.Recovery(manager._core, options, _recovery_clock, log)

    stale_timeout = _option("stale_timeout")
    lost_timeout = _option("lost_timeout")
    interval = _option("interval")
    channel_retry_limit = _option("channel_retry_limit")
    channel_retry_cooldown = _option("channel_retry_cooldown")
    device_reset_timeout = _option("device_reset_timeout")
    device_reset_cooldown = _option("device_reset_cooldown")
    channel_reopen_wait = _option("channel_reopen_wait")
    channel_stick_reset_cooldown = _option("channel_stick_reset_cooldown")
    channel_open_retry_interval = _option("channel_open_retry_interval")
    hr_frozen_timeout = _option("hr_frozen_timeout")
    value_frozen_timeout = _option("value_frozen_timeout")

    @property
    def running(self):
        return self._core.running

    def start(self):
        self._core.start()

    def stop(self):
        self._core.stop()

    def check(self):
        self._core.check()

    def notify_channel_event(self, event_name, channel=None):
        """Record an urgent ANT channel event for the watchdog to act on -
        safe to call from any thread, including the ANT message-pump thread:
        it only flags the event, which the next check() acts on."""
        self._core.notify_channel_event(event_name, channel)

    def set_watched_devices(self, identities):
        """Restrict stale/lost tracking and escalation to just these
        devices - an iterable of (device_type, device_number) tuples -
        or pass None to watch every device the manager has ever seen."""
        self._core.set_watched_devices(None if identities is None else [tuple(i) for i in identities])

    @property
    def _watched_devices(self):
        return self._core.watched_devices

    @property
    def _channel_reopens(self):
        """{channel: {"reopened": when, "stick_reset": whether the one stick reset was used}} - dedicated channels'
        loss episodes"""
        return self._core.channel_reopens()

    def enable_escalation(self):
        self._core.enable_escalation()

    def disable_escalation(self):
        # Only suppresses new attempts; the cooldown/attempt bookkeeping is
        # kept, so re-enabling resumes from where it was
        self._core.disable_escalation()

    @property
    def escalation_enabled(self):
        return self._core.escalation_enabled

    @escalation_enabled.setter
    def escalation_enabled(self, enabled):
        if enabled:
            self.enable_escalation()
        else:
            self.disable_escalation()

    @property
    def stats(self):
        return self._core.report()

    def _log(self, message):
        # Every caller (AntWorkoutBridge) wires on_event to the unified
        # ClassAntLogger, so this is where all of RecoveryManager's own events
        # end up
        if self.on_event:
            try:
                self.on_event(message)
            except Exception:
                pass

    def get_devices(self):
        return self.manager.get_devices()

    def report(self):
        return dict(self._core.report())
