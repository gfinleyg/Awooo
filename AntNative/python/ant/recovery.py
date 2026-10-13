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
                       if it's still silent (see _handle_channel_escalation)
  scan channel       - channel retries, then a stick reset

Recovery state is stored on ANTDevice.
This manager only monitors devices.
"""

from threading import Thread, Event, Lock
from time import monotonic

from .manager import MODE_CHANNELS


#
# ANT+ device types that get a value-plausibility check in
# _frozen_value_is_suspicious() below. Kept here (rather than importing
# from ant.profiles) since recovery only needs the numeric ID, not the
# profile classes themselves.
#
DEVICE_TYPE_POWER = 11
DEVICE_TYPE_HRM = 120
DEVICE_TYPE_SPEED_CADENCE_COMBINED = 121
DEVICE_TYPE_CADENCE = 122
DEVICE_TYPE_SPEED = 123


class RecoveryManager:

    #
    # RESPONSE_EVENT (message ID 0x40) names - decoded in the C++
    # transport layer (ant_native.cpp response_event_name) - that are
    # worth reacting to sooner than the ordinary silence timer would
    # notice. Anything else (RESPONSE_NO_ERROR, EVENT_TX, transfer/burst
    # events, channel collision, ...) is either not applicable to a
    # single shared RX-scan channel or too routine to react to - logged
    # if seen, never actioned.
    #
    URGENT_RETRY_EVENTS = frozenset((
        "EVENT_RX_FAIL_GO_TO_SEARCH",
        "EVENT_RX_SEARCH_TIMEOUT",
    ))

    URGENT_RESET_EVENTS = frozenset((
        "EVENT_CHANNEL_CLOSED",
    ))

    #
    # EVENT_SERIAL_QUE_OVERFLOW/EVENT_QUE_OVERFLOW mean a message queue
    # (host-side or on the stick) briefly filled up faster than it was
    # drained - a slow-consumer problem, not a sign the channel/link
    # itself is broken. This fires easily and harmlessly right when
    # several devices are discovered within milliseconds of opening the
    # channel (a burst of initial broadcasts to process at once), and a
    # channel close/reopen doesn't address a queue backlog anyway - it
    # just adds churn at exactly the moment the queue needs draining, not
    # more work. So these are deliberately NOT in ACTIONABLE_EVENTS
    # (still logged by the caller for visibility - see
    # ClassAntDevices.py's _on_message - just never turned into a retry).
    #
    NON_ACTIONABLE_QUEUE_EVENTS = frozenset((
        "EVENT_SERIAL_QUE_OVERFLOW",
        "EVENT_QUE_OVERFLOW",
    ))

    ACTIONABLE_EVENTS = URGENT_RETRY_EVENTS | URGENT_RESET_EVENTS

    #
    # Dedicated channels (manager MODE_CHANNELS): a channel that closes on
    # its own gets reopened. EVENT_RX_FAIL_GO_TO_SEARCH is routine there
    # (the channel lost its device and is searching again by itself), so
    # it's never acted on in that mode.
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

        self.stale_timeout = stale_timeout
        self.lost_timeout = lost_timeout
        self.interval = interval


        #
        # Escalation tuning.
        #
        # Tier 1 (channel retry): cheap, reopens just the scan
        # channel. Tried up to channel_retry_limit times, no
        # closer together than channel_retry_cooldown apart.
        #
        # Tier 2 (device reset): only after tier 1 is exhausted
        # AND devices have stayed LOST for device_reset_timeout.
        # Full ANTNative.reset(), so it's gated by its own
        # device_reset_cooldown to avoid reset-storming the stick.
        # Both default short (7s) since this often runs unattended
        # mid-workout - channel retries (tier 1) still get tried
        # first, so a reset isn't the first response to a blip.
        #
        self.channel_retry_limit = channel_retry_limit
        self.channel_retry_cooldown = channel_retry_cooldown
        self.device_reset_timeout = device_reset_timeout
        self.device_reset_cooldown = device_reset_cooldown

        #
        # Dedicated channels (MODE_CHANNELS) use a different ladder - see
        # _handle_channel_escalation: one channel reopen when a device goes
        # LOST, then one stick reset if it's still silent
        # channel_reopen_wait after that. channel_retry_limit/cooldown and
        # device_reset_timeout only apply to the scan channel, which is
        # still what's running if dedicated channels failed to open.
        #
        self.channel_reopen_wait = channel_reopen_wait

        #
        # Minimum time between stick resets triggered by that ladder -
        # longer than device_reset_cooldown, since a sensor that keeps
        # dropping would otherwise reset the stick (and briefly every other
        # sensor with it) every loss episode.
        #
        self.channel_stick_reset_cooldown = channel_stick_reset_cooldown

        #
        # How often to retry opening dedicated channels that failed to
        # open (the stick fell back to the scan channel, so the trainer
        # can't be controlled) - see _retry_device_channels.
        #
        self.channel_open_retry_interval = channel_open_retry_interval
        self._last_channel_open_retry = 0.0

        #
        # Data-plausibility tuning (see _check_data_freshness /
        # _frozen_value_is_suspicious). Separate from the tiers above:
        # these never touch the ANT connection, they only flag a device
        # whose radio link is fine but whose decoded reading has stopped
        # changing in a way that isn't explained by a legitimate
        # idle/coast/rest state.
        #
        # HRM gets its own (longer) timeout because ANY unchanged HR
        # page - regardless of the bpm value - is flagged: beat-count/
        # event-time advance every heartbeat even at a slow resting
        # rate, so a page that's genuinely byte-for-byte frozen this
        # long isn't physiologically possible. Power/cadence/speed use
        # the shorter timeout but only flag a FROZEN NON-ZERO reading -
        # frozen at zero is just coasting/stopped, which is normal and
        # already handled by the existing hold-last-value smoothing.
        #
        self.hr_frozen_timeout = hr_frozen_timeout
        self.value_frozen_timeout = value_frozen_timeout

        #
        # Optional callable(str) for surfacing recovery events to
        # a caller's own logging (e.g. the test scripts' log()).
        # Never allowed to break recovery if it raises.
        #
        self.on_event = on_event


        self.stop_event = Event()

        #
        # Lets notify_channel_event() (called from the ANT message-pump
        # thread - see its docstring for why nothing else may run there)
        # wake the watchdog loop immediately instead of waiting out the
        # rest of `interval`, without the watchdog loop ever running on
        # any thread but its own.
        #
        self._wake_event = Event()

        self.thread = None

        self.running = False


        #
        # Guards against the periodic watchdog thread and a
        # manually-called check() overlapping mid-escalation.
        #
        self._escalation_lock = Lock()

        self._lost_since = None
        self._channel_retry_count = 0
        self._last_channel_retry = 0.0
        self._last_device_reset = 0.0

        #
        # None = watch every device the manager has ever seen (matches
        # prior behavior, and what the standalone recovery tests
        # exercise). A caller that actually knows which devices this
        # session depends on - AntWorkoutBridge, once devices are
        # selected - narrows this with set_watched_devices() so a
        # bystander ANT+ device (someone else's sensor sharing the same
        # network key, or something glimpsed once during scanning but
        # never selected) can't trigger a channel retry or a full stick
        # reset just because it goes quiet; it was never this workout's
        # device to lose.
        #
        self._watched_devices = None

        #
        # Urgent channel events reported by notify_channel_event(),
        # waiting to be drained by the next check() on this manager's
        # own thread. Never touched from any other thread without
        # holding _pending_lock.
        #
        self._pending_events = set()    # (event_name, channel)
        self._pending_lock = Lock()

        #
        # MODE_CHANNELS only: channel -> {"reopened": time of the one
        # channel reopen this loss, "stick_reset": whether the one stick
        # reset has been used}. Cleared for a channel once its device is
        # heard again.
        #
        self._channel_reopens = {}

        #
        # Lets a caller deliberately test/exercise escalation
        # (or deliberately suppress it, e.g. while running a
        # software-only drop simulation that shouldn't be
        # touching the real radio) without tearing the manager
        # down. See enable_escalation()/disable_escalation().
        #
        self.escalation_enabled = True


        self.stats = {

            "checks": 0,
            "stale": 0,
            "lost": 0,
            "recoveries": 0,

            "channel_retries": 0,
            "channel_retry_failures": 0,
            "device_resets": 0,
            "device_reset_failures": 0,

            "channel_open_retries": 0,
            "channel_open_retry_failures": 0,

            "stalled": 0,
            "stalled_recovered": 0

        }



    def start(self):

        if self.running:
            return


        self.running = True

        self.stop_event.clear()


        self.thread = Thread(
            target=self._run,
            daemon=True
        )

        self.thread.start()



    def stop(self):

        self.running = False

        self.stop_event.set()

        #
        # Without this, stop() could be left waiting up to `interval`
        # for _run()'s wait() to time out, since that wait is now on
        # _wake_event, not stop_event directly (see _run).
        #
        self._wake_event.set()



    def _run(self):

        while not self.stop_event.is_set():

            #
            # Sleeps for `interval` as before UNLESS notify_channel_event()
            # sets _wake_event first, in which case an urgent channel
            # event gets picked up by check() right away instead of
            # waiting out the rest of the tick.
            #
            self._wake_event.wait(self.interval)

            if self.stop_event.is_set():
                break

            self._wake_event.clear()

            try:

                self.check()

            except Exception as e:

                self._log(f"RECOVERY internal error: {e}")



    def notify_channel_event(self, event_name, channel=None):
        """Record an urgent ANT channel event for the watchdog thread to
        act on - safe to call from ANY thread, including the ANT message-
        pump thread that dispatch_message() runs on.

        This deliberately does NOT act on the event itself. Tier-2
        recovery (_attempt_device_reset) calls native.stop_message_pump(),
        which has to join that same pump thread - calling it directly
        from inside a callback running ON the pump thread would be
        joining itself and deadlock. So this only flags that something
        happened; the actual retry/reset still only ever runs from
        check(), on this RecoveryManager's own background thread.
        """

        if event_name not in self.ACTIONABLE_EVENTS:
            return

        with self._pending_lock:
            self._pending_events.add((event_name, channel))

        self._wake_event.set()


    def set_watched_devices(self, identities):
        """Restrict stale/lost tracking and escalation to just these
        devices - an iterable of (device_type, device_number) tuples -
        or pass None to watch every device the manager has ever seen
        (the default; see __init__ for why).
        """

        self._watched_devices = (
            None if identities is None else set(identities)
        )

        #
        # A new selection reuses channel numbers 0, 1, ... for different
        # devices - don't carry a loss episode over to them
        #
        self._channel_reopens = {}

    def _is_watched(self, device):

        return (
            self._watched_devices is None
            or (device.device_type, device.device_number)
            in self._watched_devices
        )

    def _drain_pending_events(self):

        with self._pending_lock:

            events = set(self._pending_events)

            self._pending_events.clear()

        return events



    def check(self):

        now = monotonic()

        self.stats["checks"] += 1


        for device in self.manager.get_devices():

            #
            # Not part of this workout (or nothing has been
            # selected/connected yet) - leave it alone entirely. It never
            # enters STALE/LOST and never factors into escalation below;
            # a bystander device going quiet isn't this workout's
            # problem, and reacting to it just wastes the shared stick's
            # attention (or worse, resets it) over nothing.
            #
            if not self._is_watched(device):
                continue

            age = (
                now - device.last_seen
            )


            #
            # ACTIVE
            #
            if age < self.stale_timeout:


                if device.state in (
                    "STALE",
                    "LOST"
                ):

                    self.stats["recoveries"] += 1


                    self._log(
                        f"DEVICE_RECOVERED id={device.device_number} "
                        f"type={device.device_type} age={age:.2f}s"
                    )


                device.mark_state(
                    "ACTIVE"
                )



            #
            # STALE
            #
            elif age < self.lost_timeout:


                if device.state != "STALE":

                    self.stats["stale"] += 1


                    device.stale_time = now


                    self._log(
                        f"DEVICE_STALE id={device.device_number} "
                        f"type={device.device_type} silent={age:.2f}s"
                    )


                device.mark_state(
                    "STALE"
                )



            #
            # LOST
            #
            else:


                if device.state != "LOST":

                    self.stats["lost"] += 1


                    device.lost_time = now


                    self._log(
                        f"DEVICE_LOST id={device.device_number} "
                        f"type={device.device_type} silent={age:.2f}s "
                        f"packets={device.packet_count}"
                    )


                device.mark_state(
                    "LOST"
                )


            #
            # Data-plausibility check - orthogonal to the link-state
            # machine above. Only meaningful while the link itself is
            # fine (ACTIVE): a STALE/LOST device is already flagged by
            # its state, and "stalled" would be a redundant, confusing
            # second signal on top of that.
            #
            if device.state == "ACTIVE":

                self._check_data_freshness(device, now)

            else:

                device.clear_stalled()


        self._handle_escalation()


    def _check_data_freshness(self, device, now):
        """Flag a device that's still transmitting (state is ACTIVE, so
        the radio link is fine) but whose decoded reading has stopped
        changing in a way that isn't explained by a legitimate idle/
        coast/rest state.

        Deliberately never triggers a channel retry or device reset:
        the link is fine by definition here, so resetting it can't fix
        a stuck sensor measurement. This only marks the device and logs
        a distinctly-worded warning so it doesn't get confused with a
        connectivity problem.
        """

        #
        # A stalled reading that moved again was cleared by
        # ANTDevice.update() the moment it did - reported here
        #
        if device.data_resumed:

            device.data_resumed = False

            self._log_data_recovered(device)

        data_age = now - device.last_new_data_seen

        timeout = (
            self.hr_frozen_timeout
            if device.device_type == DEVICE_TYPE_HRM
            else self.value_frozen_timeout
        )

        if (
            data_age >= timeout
            and self._frozen_value_is_suspicious(device)
        ):

            if not device.data_stalled:

                self.stats["stalled"] += 1

                self._log(
                    "DEVICE_DATA_STALLED "
                    f"id={device.device_number} "
                    f"type={device.device_type} "
                    f"frozen={data_age:.1f}s - sensor is still "
                    "transmitting but its reading hasn't changed; "
                    "check battery/contact (no channel action taken)"
                )

            device.mark_stalled()

        elif device.data_stalled:

            self._log_data_recovered(device)

            device.clear_stalled()


    def _log_data_recovered(self, device):

        self.stats["stalled_recovered"] += 1

        self._log(
            "DEVICE_DATA_RECOVERED "
            f"id={device.device_number} "
            f"type={device.device_type}"
        )


    def _frozen_value_is_suspicious(self, device):

        latest = device.latest

        device_type = device.device_type

        if device_type == DEVICE_TYPE_HRM:

            #
            # Beat-count/event-time advance every heartbeat, even at a
            # very slow resting rate. A page that hasn't changed at all
            # for hr_frozen_timeout isn't physiologically possible,
            # whatever the bpm reading itself says.
            #
            return True

        if device_type == DEVICE_TYPE_POWER:

            return bool(
                getattr(latest, "instantaneous_power_watts", None)
            )

        if device_type == DEVICE_TYPE_SPEED_CADENCE_COMBINED:

            cadence = getattr(latest, "cadence_rpm", None)
            speed = getattr(latest, "speed_mps", None)

            return bool(cadence) or bool(speed)

        if device_type == DEVICE_TYPE_CADENCE:

            return bool(getattr(latest, "cadence_rpm", None))

        if device_type == DEVICE_TYPE_SPEED:

            return bool(getattr(latest, "speed_mps", None))

        #
        # Unhandled profile type (e.g. fitness equipment/trainer) -
        # last_new_data_seen is still kept fresh by device.update(), so
        # timing data exists, but there's no validated field to judge
        # zero-vs-nonzero plausibility against yet. Track only, don't
        # guess.
        #
        return False


    def _log(self, message):
        #No console output - AntWorkout.py runs without a visible
        #console on the user's machine, so a bare print() here was
        #invisible and left no record once the window closed. Every
        #caller (AntWorkoutBridge) wires on_event to the unified
        #ClassAntLogger, so this is the one place all of RecoveryManager's
        #own events (DEVICE_STALE/LOST/RECOVERED, channel retries, device
        #resets, internal errors) end up.

        if self.on_event:

            try:
                self.on_event(message)
            except Exception:
                pass


    def enable_escalation(self):

        self.escalation_enabled = True


    def disable_escalation(self):

        #
        # Only suppresses new tier-1/tier-2 attempts. Does not
        # reset in-flight bookkeeping, so re-enabling resumes
        # from wherever the cooldown/attempt counters were.
        #
        self.escalation_enabled = False


    def _handle_escalation(self):

        if not self.escalation_enabled:
            return

        #
        # Only one escalation attempt in flight at a time. If the
        # periodic watchdog thread and a manually-called check()
        # (as ant_recovery_test.py does in its own loop) land at
        # the same time, the second one just skips this round.
        #
        if not self._escalation_lock.acquire(blocking=False):
            return

        try:

            now = monotonic()

            pending = self._drain_pending_events()

            #
            # A stick that refused a channel change and the fall back to
            # the scan channel takes no channel changes at all until it's
            # reset (see ANTManager.needs_reset) - retrying the channels
            # can't help, a reset (which reapplies them) does
            #
            if (
                self.manager.needs_reset
                and now - self._last_device_reset
                >= self.device_reset_cooldown
            ):

                self._attempt_device_reset(
                    [], reason="the stick refused a channel change"
                )

                return

            if (
                self.manager.wants_device_channels()
                and now - self._last_channel_open_retry
                >= self.channel_open_retry_interval
                and self._retry_device_channels()
            ):
                return

            if self.manager.mode == MODE_CHANNELS:

                self._handle_channel_escalation(now, pending)

                return

            pending_events = {name for name, _ in pending}

            #
            # Our own channel closes come right back as
            # EVENT_CHANNEL_CLOSED - only one outside the manager's
            # reconfiguration grace for its channel is unexpected
            #
            urgent_reset = any(
                name in self.URGENT_RESET_EVENTS
                and not self.manager.events_suppressed(channel, now)
                for name, channel in pending
            )

            #
            # Watched devices only, same as check(). check() stops updating
            # a device's state once it's unwatched, so one that was LOST
            # when the selection changed (Disconnect and Rescan, back to
            # setup, a different sensor picked) would otherwise stay LOST
            # forever and keep driving retries/stick resets.
            #
            lost_devices = [
                d for d in self.manager.get_devices()
                if d.state == "LOST" and self._is_watched(d)
            ]

            urgent_retry = bool(
                pending_events & self.URGENT_RETRY_EVENTS
            )

            if not lost_devices and not urgent_reset and not urgent_retry:

                self._lost_since = None
                self._channel_retry_count = 0

                return

            if self._lost_since is None:
                self._lost_since = now

            time_lost = now - self._lost_since


            #
            # An unexpected EVENT_CHANNEL_CLOSED means the channel isn't
            # just quiet, it's actually closed - a "retry" (close/
            # unassign/reassign) doesn't really apply to something
            # that's already closed, so this skips tier 1 and goes
            # straight to reassigning/reopening it via the device-level
            # path. Still gated by device_reset_cooldown so a channel
            # that keeps flapping closed can't reset-storm the stick.
            #
            if (
                urgent_reset
                and (
                    now - self._last_device_reset
                    >= self.device_reset_cooldown
                )
            ):

                self._attempt_device_reset(
                    lost_devices,
                    reason="EVENT_CHANNEL_CLOSED"
                )

                return


            #
            # Tier 1: channel-level retry. Reached either because
            # devices have gone LOST on the ordinary silence timer, or
            # because an urgent event (go-to-search / search-timeout /
            # queue overflow) says the link degraded before any
            # device's silence timer would have noticed it - either
            # way, still throttled by the same limit/cooldown so a
            # flapping link can't hammer this.
            #
            if (
                (lost_devices or urgent_retry)
                and self._channel_retry_count < self.channel_retry_limit
                and (
                    now - self._last_channel_retry
                    >= self.channel_retry_cooldown
                )
            ):

                self._attempt_channel_retry(
                    lost_devices,
                    reason=(
                        "silence timeout"
                        if lost_devices
                        else "urgent event"
                    )
                )

                return


            #
            # Tier 2: device-level (whole stick) reset, only once
            # channel retries are exhausted and devices have stayed
            # LOST past device_reset_timeout.
            #
            if (
                lost_devices
                and time_lost >= self.device_reset_timeout
                and (
                    now - self._last_device_reset
                    >= self.device_reset_cooldown
                )
            ):

                self._attempt_device_reset(lost_devices, reason="timeout")

        finally:

            self._escalation_lock.release()


    def _attempt_channel_retry(self, lost_devices, reason="silence timeout"):

        self._channel_retry_count += 1
        self._last_channel_retry = monotonic()
        self.stats["channel_retries"] += 1

        ids = [d.device_number for d in lost_devices]

        self._log(
            f"RECOVERY channel retry "
            f"#{self._channel_retry_count}/{self.channel_retry_limit} "
            f"(reason={reason}, lost={ids})"
        )

        try:

            #
            # Closing the channel ourselves legitimately makes the stick
            # report EVENT_CHANNEL_CLOSED right back - otherwise
            # indistinguishable from an unexpected closure once it reaches
            # notify_channel_event(). The manager's reconfiguration grace
            # (see ANTManager.events_suppressed()) covers it, so this retry's
            # own close can't trigger a full device reset on the next
            # check().
            #
            self.manager.reopen_scan_channel()

            self._log(
                "RECOVERY channel retry succeeded, channel reopened"
            )

        except Exception as e:

            self.stats["channel_retry_failures"] += 1

            self._log(
                f"RECOVERY channel retry failed: {e}"
            )


    def _attempt_device_reset(self, lost_devices, reason="timeout"):

        self._last_device_reset = monotonic()
        self.stats["device_resets"] += 1

        ids = [d.device_number for d in lost_devices]

        if reason == "timeout":

            self._log(
                f"RECOVERY channel retries exhausted after "
                f"{self._lost_since and round(monotonic() - self._lost_since, 1)}s, "
                f"escalating to device-level reset (lost={ids})"
            )

        else:

            self._log(
                f"RECOVERY escalating to device-level reset "
                f"(reason={reason}, lost={ids})"
            )

        native = self.manager.native

        scan_config = self.manager._scan_config

        if scan_config is None:

            self.stats["device_reset_failures"] += 1

            self._log(
                "RECOVERY device-level reset skipped: "
                "no prior scan configuration to reapply"
            )

            return False

        try:

            native.stop_message_pump()

            #
            # Resets the stick itself (first step of the scan setup -
            # raises if the reset fails) and, since a reset clears the
            # network key + channel assignment, reapplies the whole setup
            # (scan channel, or every dedicated channel).
            #
            self.manager.reapply_configuration()

            native.start_message_pump(
                self.manager._on_message
            )

            #
            # Give devices a fresh recovery window rather than
            # immediately re-escalating on the next check().
            #
            self._channel_retry_count = 0
            self._lost_since = None

            self._log(
                "RECOVERY device-level reset complete, "
                "channel configuration reapplied"
            )

            return True

        except Exception as e:

            self.stats["device_reset_failures"] += 1

            self._log(
                f"RECOVERY device-level reset FAILED: {e}"
            )

            return False


    def _retry_device_channels(self):
        """Retry opening the dedicated channels after they failed to open
        and the stick fell back to the scan channel. Returns True if they
        opened."""

        self._last_channel_open_retry = monotonic()
        self.stats["channel_open_retries"] += 1

        self._log(
            "RECOVERY retrying dedicated channel open "
            "(stuck on the scan channel)"
        )

        try:

            if not self.manager.retry_device_channels():
                return False

        except Exception as e:

            self.stats["channel_open_retry_failures"] += 1

            self._log(f"RECOVERY dedicated channel open failed again: {e}")

            return False

        #
        # Scan-mode escalation bookkeeping doesn't carry over
        #
        self._lost_since = None
        self._channel_retry_count = 0
        self._channel_reopens = {}

        self._log("RECOVERY dedicated channels opened")

        return True


    def _handle_channel_escalation(self, now, pending):
        """Escalation for dedicated channels (manager MODE_CHANNELS).

        Each device has its own channel that searches for it forever, so
        recovery is per channel: a channel that closes on its own is
        reopened straight away, and a device gone LOST (silent for
        lost_timeout) gets its channel reopened once. If that device still
        hasn't been heard channel_reopen_wait after the reopen, the whole
        stick is reset once - but only when none of the other watched
        devices is being heard either. While one is, the stick is working
        and the lost device is just switched off or out of range, which a
        reset can't fix; it would only cut off the devices that are fine
        for several seconds (an FE-C trainer's control included). With a
        single watched device there's nothing to tell by, so it resets.
        After that the channel is just left searching until the device
        comes back, which starts a fresh loss episode next time.
        channel_stick_reset_cooldown spaces stick resets apart, so several
        channels dropping together only reset it once and a sensor that
        keeps dropping can't keep resetting it.
        """

        devices = [
            d for d in self.manager.get_devices()
            if self._is_watched(d)
        ]

        #
        # Channels the stick closed on its own - ignored right after our
        # own reconfiguration of that channel, which closes it deliberately
        #
        closed = sorted({
            channel for name, channel in pending
            if name in self.CHANNEL_CLOSED_EVENTS
            and channel is not None
            and not self.manager.events_suppressed(channel, now)
        })

        for channel in closed:

            self._reopen_channel(channel, "channel closed")

        lost_devices = [d for d in devices if d.state == "LOST"]

        reset_channel = None

        for device in devices:

            channel = self.manager.channel_for(
                device.device_type, device.device_number
            )

            if channel is None:
                continue

            #
            # Heard again - loss episode over
            #
            if device.state != "LOST":

                self._channel_reopens.pop(channel, None)

                continue

            episode = self._channel_reopens.get(channel)

            if episode is None:

                self._channel_reopens[channel] = {
                    "reopened": now,
                    "stick_reset": False,
                }

                self._reopen_channel(
                    channel,
                    f"silent {now - device.last_seen:.1f}s, "
                    f"device={device.device_number} type={device.device_type}"
                )

                continue

            if (
                not episode["stick_reset"]
                and now - episode["reopened"] >= self.channel_reopen_wait
            ):

                reset_channel = channel

        if reset_channel is None:
            return

        #
        # Another watched device on its own channel still coming through:
        # the stick is fine. The lost devices' episodes are closed off as if
        # reset, so this is only decided (and logged) once per episode
        #
        heard = [
            d for d in devices
            if d.state == "ACTIVE"
            and self.manager.channel_for(d.device_type, d.device_number) is not None
        ]

        if heard:

            waiting = []

            for device in lost_devices:

                episode = self._channel_reopens.get(
                    self.manager.channel_for(
                        device.device_type, device.device_number
                    )
                )

                if episode is not None and not episode["stick_reset"]:

                    episode["stick_reset"] = True
                    waiting.append(device.device_number)

            self._log(
                f"RECOVERY not resetting the stick for lost={waiting}: "
                f"still hearing {[d.device_number for d in heard]}, so the "
                f"stick is working - leaving the channel searching"
            )

            return

        if now - self._last_device_reset < self.channel_stick_reset_cooldown:
            return

        if self._attempt_device_reset(
            lost_devices,
            reason=(
                f"channel {reset_channel} still silent "
                f"{self.channel_reopen_wait}s after reopen"
            )
        ):

            #
            # The reset reopened every channel, so it counts as the one
            # stick reset for every channel that was down at the time. A
            # failed reset isn't counted - it's retried after
            # channel_stick_reset_cooldown.
            #
            for device in lost_devices:

                episode = self._channel_reopens.get(
                    self.manager.channel_for(
                        device.device_type, device.device_number
                    )
                )

                if episode is not None:
                    episode["stick_reset"] = True


    def _reopen_channel(self, channel, reason):

        self.stats["channel_retries"] += 1

        self._log(f"RECOVERY reopening channel {channel} ({reason})")

        try:

            self.manager.reopen_channel(channel)

        except Exception as e:

            self.stats["channel_retry_failures"] += 1

            self._log(f"RECOVERY channel {channel} reopen failed: {e}")


    def get_devices(self):

        return self.manager.get_devices()

    def report(self):

        return dict(
            self.stats
        )