"""
ClassAntDevices.py

Bridge between AntWorkout.py's UI and AntNative's ANTManager/RecoveryManager.

Replaces the old StartAnt(QThread), which used to launch AntWorkout.exe and
then get out of the way while the exe wrote data files. AntWorkoutBridge
instead OWNS the live ANT connection for the life of the thread: it opens the
ANT stick, configures the shared RX scan channel for device discovery
(switching to one dedicated channel per selected device once connected -
see _open_device_channels()), starts the recovery
watchdog/escalation, and keeps running so AntDevices (in AntWorkout.py) can
pull live decoded sensor data straight off ANTDevice.latest instead of
polling text files.

Event-driven, not polled: the UI used to find out about new devices, a
completed connect, or a disconnect by re-reading text files on a fixed
timer. Now this bridge emits Qt signals the instant each of those actually
happens (deviceDiscovered, devicesConnected, devicesDisconnected,
dataUpdated, statusMessage, errorOccurred), and the UI thread's request to
connect/disconnect is a direct thread-safe method call (request_connect/
request_disconnect) instead of creating/deleting a flag file the bridge
used to poll for every 200ms.

Setup state the UI used to read back out of text files now lives on this
bridge, read through plain methods:

  found_devices()          - every "<device_type> <device_number>" seen
                             this session (was foundDevicesANT.txt)
  is_initialized()         - the stick opened and is scanning (was
                             initANT.txt existing)
  take_error()             - initialization failure text, handed out once
                             (was errorANT.txt, deleted once shown)
  is_connect_requested()   - request_connect() called and not undone yet
                             (was connectANT.txt existing)
  is_ready()               - that connect has been processed (was
                             readyANT.txt existing)

The selection itself lives in WorkoutHelpers (Read/WriteSelectedDeviceLines,
was selectedDevicesANT.txt). Recovery events and initialization failures
go to the unified log (ClassAntLogger), which replaced warningsANT.txt.

AntDevicesPoller reads decoded values straight from
AntWorkoutBridge.selected_device(role) and keeps the workout's sensor
history in memory (see WorkoutData.py).
"""

import os
import sys
import threading
import time

# ant_native.vcxproj's CopyToAntWorkout target vendors the "ant" package
# into AntWorkout\AntNative\python\ant (mirroring AntNative's own layout,
# see the vcxproj comment for why the depth matters), not AntWorkout\ant
# directly - so that directory has to be on sys.path before "ant" can be
# imported below.
#
# IMPORTANT: this import has to happen before PyQt6 is imported anywhere
# in the process, not just in this file - importing PyQt6 first causes a
# hard access violation (0xC0000005) the moment ant_native.pyd loads (some
# DLL PyQt6 pulls onto the process's search path shadows/conflicts with
# one ant_native.pyd or ANT_LIB.dll depends on). AntWorkout.py forces this
# same import at the very top of the file, before its own PyQt6 imports,
# so by the time this module is imported ant_native is already loaded and
# this is just a cheap sys.modules hit - but keep the order here too in
# case this module is ever imported on its own (a test script, etc.)
# without going through AntWorkout.py first.
_ANT_NATIVE_PYTHON_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "AntNative",
    "python",
)
if _ANT_NATIVE_PYTHON_DIR not in sys.path:
    sys.path.insert(0, _ANT_NATIVE_PYTHON_DIR)

from ant.manager import ANTManager
from ant.recovery import RecoveryManager
from ant.profiles.fitness_equipment import (
    basic_resistance_payload,
    target_power_payload,
    TARGET_POWER_MIN_WATTS,
    TARGET_POWER_MAX_WATTS,
)

from PyQt6.QtCore import QThread, pyqtSignal

import ClassAntLogger
from WorkoutHelpers import ReadSelectedDeviceLines

# Every ANT response event this session sees, logged for visibility even
# when RecoveryManager doesn't act on it (only ACTIONABLE_EVENTS trigger
# a retry/reset - see recovery.py). Routine events are skipped:
# RESPONSE_NO_ERROR is the ack for every close_channel/assign_channel/etc.
# call we make ourselves; on dedicated channels EVENT_RX_FAIL fires for
# every single missed message and the transfer events for every
# acknowledged send (a failed trainer send is logged, throttled, by
# _service_trainer_target() instead). EVENT_CHANNEL_COLLISION is two
# dedicated channels whose slightly different ANT+ periods drift into the
# same timeslot - the stick skips one message on one of them, routine
# with several sensors connected.
_SKIP_LOGGING_EVENTS = frozenset((
    "RESPONSE_NO_ERROR",
    "EVENT_RX_FAIL",
    "EVENT_CHANNEL_COLLISION",
    "EVENT_TX",
    "EVENT_TRANSFER_TX_START",
    "EVENT_TRANSFER_TX_COMPLETED",
    "EVENT_TRANSFER_TX_FAILED",
))


NETWORK_KEY = bytes([
    0xB9, 0xA5, 0x21, 0xFB,
    0xBD, 0x72, 0xC3, 0x45
])


# Roles the UI cares about today.
ROLE_HRM = "hrm"
ROLE_POWER = "power"
ROLE_SPEED_CADENCE = "speed_cadence"
ROLE_CADENCE = "cadence"
ROLE_SPEED = "speed"

ROLE_BY_DEVICE_TYPE = {
    120: ROLE_HRM,
    11: ROLE_POWER,
    # Fitness Equipment (17) covers both plain and "controllable" FE-C
    # trainers - ANT+ doesn't give FE-C its own device type, just extra
    # capability pages on the same type 17. The setup UI lets you pick one
    # as your power source (select_power_device() treats 11/17 as the same
    # slot). FitnessEquipmentProfile decodes the trainer data page into
    # instantaneous power/cadence for the gauges, and the trainer's target
    # power is driven through set_trainer_target_power().
    17: ROLE_POWER,
    121: ROLE_SPEED_CADENCE,
    122: ROLE_CADENCE,
    123: ROLE_SPEED,
}


# Names used in low battery warnings. Device type 17 shares ROLE_POWER
# but is a trainer, not a power meter. Combined speed/cadence sensors
# (121) can't report battery status, so ROLE_SPEED_CADENCE never warns.
ROLE_BATTERY_NAMES = {
    ROLE_HRM: "Heart rate monitor",
    ROLE_POWER: "Power meter",
    ROLE_CADENCE: "Cadence sensor",
    ROLE_SPEED: "Speed sensor",
}
TRAINER_BATTERY_NAME = "Trainer"
SHOW_ALL_BATTERY_WARNINGS = False #TEMPORARY layout testing override, see low_battery_warnings()


# ANT initialization retries: close/reopen/reconfigure the stick up to
# INIT_ATTEMPTS times in total, waiting these delays between attempts
INIT_ATTEMPTS = 4
INIT_RETRY_DELAYS_S = (1.0, 2.0, 4.0)


# Files older versions used to pass setup state and sensor data through;
# none are written any more, removed at startup so leftovers don't linger.
STALE_FILES = (
    "foundDevicesANT.txt",
    "selectedDevicesANT.txt",
    "connectANT.txt",
    "readyANT.txt",
    "errorANT.txt",
    "warningsANT.txt",
    "initANT.txt",
    "hrmANT.txt",
    "pmANT.txt",
    "scANT.txt",
)

# ANT+ Bicycle Power calibration (data page 0x01). The display sends a
# manual zero offset request (calibration ID 0xAA, remaining bytes 0xFF);
# the power meter answers on the same page with 0xAC (success) or 0xAF
# (failed), carrying its measured offset as a signed little-endian int in
# bytes 6-7.
CALIBRATION_PAGE = 0x01
CALIBRATION_REQUEST_MANUAL_ZERO = 0xAA
CALIBRATION_RESPONSE_SUCCESS = 0xAC
CALIBRATION_RESPONSE_FAILED = 0xAF
CALIBRATION_REQUEST_PAYLOAD = bytes([
    CALIBRATION_PAGE, CALIBRATION_REQUEST_MANUAL_ZERO,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
])
# Sent on the power meter's dedicated channel, re-sent right after a
# message is heard from it (so it's known to be in range) until it answers
# or this window runs out.
CALIBRATION_TIMEOUT_S = 15.0
CALIBRATION_RESEND_INTERVAL_S = 1.0

# FE-C trainer target power (ANT+ FE-C Target Power, page 49). Sent the
# same way as the calibration request (right after the trainer is heard,
# see above): immediately when the target changes, then re-sent every
# TRAINER_TARGET_RESEND_S so a lost command, or a trainer that dropped
# out and came back, still ends up at the right power.
DEVICE_TYPE_FITNESS_EQUIPMENT = 17
TRAINER_TARGET_RESEND_S = 2.0
# A send that fails (no acknowledgement back - a missed message on either
# side, common enough over the air) is tried again sooner, up to this many
# times, before falling back to TRAINER_TARGET_RESEND_S: otherwise a new
# target lost twice took ~5s to reach the trainer
TRAINER_RETRY_S = 0.5
TRAINER_FAST_RETRIES = 3
# Trainer target standing in for a watts value: out of ERG mode, see release_trainer_power()
TRAINER_RELEASED = "released"

# How long the scan channel has to have been listening before the setup
# screen auto-selects - ANT+ sensors transmit about 4 times a second, so by
# then every device in range has been heard and a slot with two candidates
# (a power meter and an FE-C trainer, say) is seen as a choice to make
# rather than whichever happened to be heard first. See scan_settle_remaining().
SCAN_SETTLE_S = 5.0
TRAINER_FAILURE_LOG_S = 30.0
ANTFRAMER_PASS = 1


def read_selected_devices():
    """The UI's current device selection as {role: (device_type, device_number)}."""
    selected = {}

    # ReadSelectedDeviceLines() only ever holds well formed
    # "<type> <number>" entries
    for line in ReadSelectedDeviceLines():
        parts = line.split()
        device_type = int(parts[0])
        device_number = int(parts[1])

        role = ROLE_BY_DEVICE_TYPE.get(device_type)
        if role is not None:
            selected[role] = (device_type, device_number)

    return selected


class AntWorkoutBridge(QThread):
    """Owns the live ANT connection for the life of the workout session.

    Usage mirrors the old StartAnt: MainWindow creates one and calls
    .start(). Unlike StartAnt (which launched the exe and returned right
    away), this thread keeps running for as long as AntWorkout.py needs
    live sensor data - stop() tears the ANT connection down cleanly.

    The UI talks to this bridge two ways:
      - Signals (deviceDiscovered, devicesConnected, devicesDisconnected,
        dataUpdated, statusMessage, errorOccurred), emitted from this
        thread and safely queued by Qt onto whichever thread the
        connected slot lives on (the GUI thread, in every real usage).
      - request_connect()/request_disconnect(), plain thread-safe methods
        the GUI thread calls directly - safe because they only touch
        plain Python state under self._request_lock, never a Qt widget.
    """

    deviceDiscovered = pyqtSignal(int, int)  # device_type, device_number
    devicesConnected = pyqtSignal()
    devicesDisconnected = pyqtSignal()
    dataUpdated = pyqtSignal(str)            # role whose profile just changed
    statusMessage = pyqtSignal(str)          # replaces polling initANT.txt
    errorOccurred = pyqtSignal(str)          # replaces polling errorANT.txt
    # device_number, result ("success"/"failed"/"timeout"/"sending"),
    # meter-reported offset (int) or None, attempt count
    calibrationUpdate = pyqtSignal(int, str, object, int)
    # A selected device's battery status changed, or the selection itself
    # did - re-read low_battery_warnings()
    batteryChanged = pyqtSignal()

    def __init__(self):
        super(AntWorkoutBridge, self).__init__()

        self.ant = ANTManager(
            on_log=lambda message: ClassAntLogger.log_event("ANT", message)
        )

        # Loosened from the original 1.5s/3s/7s/7s (still validated in
        # ant_recovery_test.py's escalation_test() - that test passes its
        # own values directly, so this change doesn't affect it) after
        # real hardware testing showed the tighter timing escalating too
        # readily. Combined with set_watched_devices() below (only
        # devices actually selected for the workout can trigger a retry
        # or reset - a bystander ANT+ device going quiet no longer
        # counts) and the fix in _attempt_channel_retry() that stops a
        # retry's own channel-close from immediately re-triggering a
        # full device reset, this should make recovery noticeably less
        # trigger-happy while still catching a genuinely dropped sensor.
        # On dedicated channels: a device silent for lost_timeout gets its
        # channel reopened once, and if it's still silent
        # channel_reopen_wait after that the stick is reset once (at most
        # every channel_stick_reset_cooldown).
        self.recovery = RecoveryManager(
            self.ant,
            stale_timeout=2.5,
            lost_timeout=5,
            channel_reopen_wait=5,
            device_reset_timeout=10,
            device_reset_cooldown=12,
            on_event=self._log_warning,
        )

        self.running = False

        # (device_type, device_number) pairs seen this session. Replaced
        # (never mutated in place) on the message pump thread, so the GUI
        # thread can read it through found_devices() without a lock.
        self._known_devices = frozenset()

        # When the scan channel last started listening (initialization, or
        # back from dedicated channels after a disconnect), None while it
        # isn't - see scan_settle_remaining()
        self._scan_started = None

        # Selected (device_type, device_number) per role, latched in once
        # a connect request is processed.
        self._selected = {}
        self._connected = False

        # Setup screen state, see the module docstring. _connect_requested
        # and _ready are only changed under _request_lock.
        self._initialized = False
        self._error = None
        self._connect_requested = False
        self._ready = False

        # Event-driven connect/disconnect handshake, replacing the old
        # 200ms poll of connectANT.txt's existence. request_connect()/
        # request_disconnect() (called directly from the GUI thread) set
        # the pending request under this lock and wake the run() loop
        # immediately; the loop only ever reads/clears the pending
        # request on its own thread, same pattern as RecoveryManager's
        # own notify_channel_event()/_wake_event.
        self._request_lock = threading.Lock()
        self._wake_event = threading.Event()
        self._pending_connect = None    # {role: (type, number)} or None
        self._pending_disconnect = False

        # In-progress ANT+ manual zero offset calibration, or None:
        # {"device_number", "deadline", "last_sent", "attempts", "heard"}.
        # Guarded by _request_lock like the connect/disconnect requests;
        # only ever transmitted from this thread (run() loop).
        self._calibration = None

        # FE-C trainer target power, or None when the trainer isn't being
        # commanded: {"watts", "attempted_watts", "last_sent", "heard"}. Guarded
        # by _request_lock; only ever transmitted from this thread.
        self._trainer_target = None

        # (device_type, device_number) -> last (status, level_percent)
        # seen, so a battery change is logged/emitted once, not per page
        self._battery_seen = {}

    def selected_device(self, role):
        """Return the live ANTDevice for `role`, or None.

        Safe to call at any time, including before any device is selected
        or connected - just returns None until there's something to give
        back.
        """
        identity = self._selected.get(role)

        if identity is None:
            return None

        device_type, device_number = identity

        for device in self.ant.get_devices():
            if (
                device.device_type == device_type
                and device.device_number == device_number
            ):
                return device

        return None

    def found_devices(self):
        """Every "<device_type> <device_number>" seen this session, sorted."""
        return [f"{device_type} {device_number}" for device_type, device_number in sorted(self._known_devices)]

    def is_initialized(self):
        return self._initialized

    def scan_settle_remaining(self):
        """Seconds until the scan has listened long enough (SCAN_SETTLE_S)
        for found_devices() to hold every device in range - 0 once it has,
        SCAN_SETTLE_S while the scan isn't running yet."""
        started = self._scan_started
        if started is None:
            return SCAN_SETTLE_S
        return max(0.0, SCAN_SETTLE_S - (time.monotonic() - started))

    def take_error(self):
        """The initialization failure text, or None - returned only once."""
        error, self._error = self._error, None
        return error

    def selected_roles(self):
        """The roles latched in by the last processed connect request."""
        return set(self._selected)

    def is_connect_requested(self):
        return self._connect_requested

    def is_ready(self):
        return self._ready

    def clear_connection_flags(self):
        """Workout over: drop the connecting/ready state without actually
        disconnecting (go_back_to_setup() does that)."""
        with self._request_lock:
            self._connect_requested = False
            self._ready = False

    def low_battery_warnings(self):
        """One warning string per selected device whose battery reports
        low/critical (GUI thread). Empty when nothing is selected - only
        the devices actually in use for this workout matter.
        """
        #TEMPORARY: show every possible warning so the layouts can be checked; set back to False when done
        if SHOW_ALL_BATTERY_WARNINGS:
            #one per battery reporting role (power meter and trainer share the power slot) - never more than 4
            return [f"{name} battery low (5%)" for name in ROLE_BATTERY_NAMES.values()]
        warnings = []
        for role in sorted(self._selected):
            if role not in ROLE_BATTERY_NAMES:
                continue
            device = self.selected_device(role)
            battery = device.battery if device is not None else None
            if battery is None or not battery.is_low:
                continue
            name = TRAINER_BATTERY_NAME if device.device_type == 17 else ROLE_BATTERY_NAMES[role]
            text = f"{name} battery {battery.status_name.lower() if battery.status_name != 'Unknown' else 'low'}"
            if battery.level_percent is not None:
                text += f" ({battery.level_percent}%)"
            warnings.append(text)
        return warnings

    def request_connect(self, selected):
        """Called directly from the GUI thread (no signal needed - this
        is GUI-thread-to-worker-thread, the direction a plain thread-safe
        method call handles fine) once the setup screen has an
        unambiguous, fully-resolved device selection - either built by
        auto_select_and_connect() or confirmed manually through
        ConnectDevicesWindow. `selected` is a {role: (device_type,
        device_number)} mapping, the same shape read_selected_devices()
        produces.

        Wakes run()'s loop immediately rather than waiting out its
        fallback interval.
        """
        with self._request_lock:
            self._pending_connect = dict(selected)
            self._pending_disconnect = False
            self._connect_requested = True
            # Not ready again until run() has latched in THIS selection -
            # otherwise is_ready()/selected_roles() would describe the
            # previous connection in the meantime
            self._ready = False
        self._wake_event.set()

    def request_disconnect(self):
        """The GUI-thread side of "Disconnect and Rescan" / going back to
        setup. Same thread-safety story as request_connect().
        """
        with self._request_lock:
            self._pending_disconnect = True
            self._pending_connect = None
            self._connect_requested = False
            self._ready = False
            # A real rescan: forget what was found before (devices are
            # rediscovered as they're heard) and hold off auto-selecting
            # until the scan has run again for SCAN_SETTLE_S
            self._known_devices = frozenset()
            self._scan_started = None
        self._wake_event.set()

    def request_power_calibration(self, device_number):
        """Start an ANT+ manual zero offset calibration of power meter
        `device_number` (GUI thread). Progress and the meter's answer
        arrive through calibrationUpdate.
        """
        with self._request_lock:
            self._calibration = {
                "device_number": int(device_number),
                "deadline": time.monotonic() + CALIBRATION_TIMEOUT_S,
                "last_sent": 0.0,
                "attempts": 0,
                "heard": False,
            }
        ClassAntLogger.log_event("CALIBRATE", f"Manual zero offset requested for power meter {device_number}")
        self._wake_event.set()

    def cancel_power_calibration(self):
        with self._request_lock:
            self._calibration = None

    def trainer_selected(self):
        """True when the selected power source is an FE-C trainer."""
        identity = self._selected.get(ROLE_POWER)
        return identity is not None and identity[0] == DEVICE_TYPE_FITNESS_EQUIPMENT

    def set_trainer_target_power(self, watts):
        """Set the selected FE-C trainer's target power in watts, or None to
        stop commanding it (GUI thread). Cheap to call repeatedly - only a
        change is sent right away, otherwise the target is refreshed every
        TRAINER_TARGET_RESEND_S.
        """
        if watts is not None:
            watts = max(TARGET_POWER_MIN_WATTS, min(TARGET_POWER_MAX_WATTS, int(round(watts))))
        self._set_trainer_target(watts)

    def release_trainer_power(self):
        """Take the selected FE-C trainer out of target power (ERG) mode by
        sending it Basic Resistance 0% (GUI thread), so the rider's power
        isn't controlled. Refreshed like a target power; any later
        set_trainer_target_power() puts it back under control.
        """
        self._set_trainer_target(TRAINER_RELEASED)

    def _set_trainer_target(self, watts):
        with self._request_lock:
            current = self._trainer_target
            if watts is None:
                self._trainer_target = None
            elif current is None:
                self._trainer_target = {"watts": watts, "attempted_watts": None, "last_sent": 0.0, "heard": False}
            else:
                current["watts"] = watts

    def run(self):
        self.running = True

        self._startup_cleanup()

        # A stick left in a bad state (e.g. by a previous session that
        # didn't shut down cleanly) can open fine but ignore the reset at
        # the start of configure_scan(). Rather than giving up, drive a
        # reset: close the stick, give it a moment, reopen and reconfigure
        # (configure_scan() sends a fresh system reset every time), backing
        # off a little more each attempt. Only the final failure is
        # reported to the UI.
        error = None
        for attempt in range(1, INIT_ATTEMPTS + 1):
            if not self.running:
                return
            if attempt > 1:
                delay = INIT_RETRY_DELAYS_S[min(attempt - 2, len(INIT_RETRY_DELAYS_S) - 1)]
                ClassAntLogger.log_event(
                    "ANT",
                    f"Resetting ANT USB stick and retrying initialization "
                    f"(attempt {attempt}/{INIT_ATTEMPTS}) in {delay}s",
                )
                try:
                    self.ant.close()
                except Exception as e:
                    ClassAntLogger.log_event("ANT", f"Closing ANT USB stick before retry failed: {e}")
                # interruptible so stop() during a retry isn't held up
                self._wake_event.wait(timeout=delay)
                self._wake_event.clear()
                if not self.running:
                    return
            error = self._initialize_ant()
            if error is None:
                break
            ClassAntLogger.log_event("ANT", error)

        if error is not None:
            try:
                self.ant.close()
            except Exception:
                pass
            ClassAntLogger.log_event(
                "ANT",
                f"ANT initialization failed after {INIT_ATTEMPTS} attempts, giving up: {error}",
            )
            self._error = error
            self.errorOccurred.emit(error)
            self.running = False
            return

        self.recovery.start()

        self._initialized = True
        self._scan_started = time.monotonic()
        ClassAntLogger.log_event("ANT", "ANT initialized, scanning for devices")
        self.statusMessage.emit("ANT initialized, scanning for devices")

        # Event-driven: blocks here until request_connect()/
        # request_disconnect() wakes it, rather than waking up on a fixed
        # interval to go check a flag file's existence. The timeout is
        # purely a defensive fallback, not the primary trigger.
        while self.running:
            # Short fallback tick while a calibration is running so its
            # timeout/resend is serviced even if the meter goes quiet
            woken = self._wake_event.wait(timeout=0.25 if self._calibration else 2.0)
            if woken:
                self._wake_event.clear()
            self._process_pending_request()
            self._service_calibration()
            self._service_trainer_target()

        self._shutdown()

    def _initialize_ant(self):
        """One open + scan configure + message pump start. Returns None on
        success, otherwise the error text to log/show."""
        try:
            ClassAntLogger.log_event("ANT", "Opening ANT USB stick...")

            if not self.ant.open():
                return "Failed to open ANT USB stick"

            self.ant.configure_scan(NETWORK_KEY, channel=0)
            self.ant.start(self._on_message)
            return None

        except Exception as e:
            return f"ANT initialization failed: {e}"

    def stop(self):
        self.running = False
        # Unblock the wait() above immediately instead of leaving it to
        # time out, mirroring RecoveryManager.stop()'s own reasoning.
        self._wake_event.set()

    def _process_pending_request(self):
        with self._request_lock:
            pending_connect = self._pending_connect
            pending_disconnect = self._pending_disconnect
            self._pending_connect = None
            self._pending_disconnect = False

        if pending_connect is not None:
            self._selected = pending_connect
            self._connected = True

            # Now that specific devices are actually relied on for this
            # workout, the watchdog should care if THEY go quiet - but
            # still not about anything else it happens to see on the
            # shared scan channel.
            self.recovery.set_watched_devices(set(self._selected.values()))

            ClassAntLogger.log_event(
                "CONNECT",
                "Connecting with selection: "
                + ", ".join(
                    f"{role}={identity}"
                    for role, identity in sorted(self._selected.items())
                ),
            )

            self._open_device_channels()

            # A disconnect requested while this was being processed wins
            with self._request_lock:
                if not self._pending_disconnect:
                    self._ready = True

            self.devicesConnected.emit()
            self.batteryChanged.emit()

        elif pending_disconnect and self._connected:
            self._selected = {}
            self._connected = False
            with self._request_lock:
                self._trainer_target = None
            self.recovery.set_watched_devices(set())
            #back to the scan channel so the setup screen can discover devices again
            try:
                self.ant.return_to_scan()
            except Exception as e:
                ClassAntLogger.log_event("CONNECT", f"Returning to device scan failed: {e}")
            ClassAntLogger.log_event("CONNECT", "Disconnected - selection cleared")
            self.devicesDisconnected.emit()
            self.batteryChanged.emit()

        if pending_disconnect:
            # scanning (again) from here, whether or not a connection was up
            with self._request_lock:
                if not self._pending_disconnect and self._pending_connect is None:
                    self._scan_started = time.monotonic()
                    # cleared again now the scan is actually back, in case the message pump re-added
                    # an old entry while request_disconnect() was clearing it; anything really in
                    # range is rediscovered on its next message
                    self._known_devices = frozenset()

    def _service_calibration(self):
        # Runs on this thread only: _send_to_device() blocks until the
        # stick reports the transfer result, which must never happen on
        # the GUI thread or inside the message pump's own callback.
        with self._request_lock:
            cal = self._calibration
            if cal is None:
                return
            now = time.monotonic()
            if now > cal["deadline"]:
                self._calibration = None
                timed_out = True
            else:
                timed_out = False
                send_now = cal["heard"] and now - cal["last_sent"] >= CALIBRATION_RESEND_INTERVAL_S
                if send_now:
                    cal["heard"] = False
                    cal["last_sent"] = now
                    cal["attempts"] += 1
            device_number = cal["device_number"]
            attempts = cal["attempts"]

        if timed_out:
            ClassAntLogger.log_event("CALIBRATE", f"No calibration response from power meter {device_number} after {attempts} attempts")
            self.calibrationUpdate.emit(device_number, "timeout", None, attempts)
            return
        if not send_now:
            return

        result = self._send_to_device(11, device_number, CALIBRATION_REQUEST_PAYLOAD)
        ClassAntLogger.log_event("CALIBRATE", f"Calibration request attempt {attempts} sent, result={result}")
        self.calibrationUpdate.emit(device_number, "sending", None, attempts)

    def _open_device_channels(self):
        # Scanning hears every device but can't transmit (the stick rejects
        # any send on a scan channel), so once the workout's devices are
        # chosen each gets its own dedicated channel - which is what lets
        # the trainer's target power and calibration requests be sent.
        # The transmission type comes from when the device was heard while
        # scanning (0 = match any, if it somehow wasn't).
        heard = {(d.device_type, d.device_number): d.transmission_type for d in self.ant.get_devices()}
        identities = [
            (device_type, device_number, heard.get((device_type, device_number), 0))
            for _, (device_type, device_number) in sorted(self._selected.items())
        ]
        try:
            self.ant.open_device_channels(identities)
        except Exception as e:
            # still connected on the scan channel, sensors keep working but nothing can be sent
            # until RecoveryManager's periodic retry gets the dedicated channels open
            ClassAntLogger.log_event("CONNECT", f"Opening dedicated device channels failed, staying on the scan channel and retrying: {e}")

    def _send_to_device(self, device_type, device_number, payload):
        """Acknowledged send of an 8-byte page to one device on its own
        dedicated channel (this thread only - it blocks until the stick
        reports the transfer result). Returns the framer result
        (ANTFRAMER_PASS on success) or an error string.
        """
        channel = self.ant.channel_for(device_type, device_number)
        if channel is None:
            return "error: device has no dedicated channel"
        try:
            return self.ant.native.send_acknowledged(channel, payload, 500)
        except Exception as e:
            return f"error: {e}"

    def _service_trainer_target(self):
        # Runs on this thread only, for the same reason as
        # _service_calibration(): the send blocks.
        with self._request_lock:
            target = self._trainer_target
            if target is None or not target["heard"] or not self.trainer_selected():
                return
            now = time.monotonic()
            if not self._trainer_send_due(target, now):
                return
            if target["watts"] != target["attempted_watts"]:
                target["failures"] = 0
            target["heard"] = False
            target["last_sent"] = now
            target["attempted_watts"] = target["watts"]
            watts = target["watts"]
            device_number = self._selected[ROLE_POWER][1]

        if watts == TRAINER_RELEASED:
            payload = basic_resistance_payload(0)
        else:
            payload = target_power_payload(watts)
        result = self._send_to_device(DEVICE_TYPE_FITNESS_EQUIPMENT, device_number, payload)
        passed = result == ANTFRAMER_PASS

        #only failures are logged (a successful send happens every couple of seconds), and a failure
        #that keeps repeating is only logged when the result changes or every TRAINER_FAILURE_LOG_S
        with self._request_lock:
            if passed:
                target["failures"] = 0
                target.pop("failure_logged", None)
                return
            target["failures"] = target.get("failures", 0) + 1
            last = target.get("failure_logged")
            if last is not None and last[0] == result and now - last[1] < TRAINER_FAILURE_LOG_S:
                return
            target["failure_logged"] = (result, now)
        what = "Release (basic resistance 0%)" if watts == TRAINER_RELEASED else f"Target power {watts}W"
        ClassAntLogger.log_event("TRAINER", f"{what} send failed, result={result}")

    @staticmethod
    def _trainer_send_due(target, now):
        # A new target goes out right away; one whose send failed is tried
        # again after TRAINER_RETRY_S, TRAINER_FAST_RETRIES times; otherwise
        # the same target waits out TRAINER_TARGET_RESEND_S, so a stick that
        # keeps failing isn't hammered on every message
        if target["watts"] != target["attempted_watts"]:
            return True
        elapsed = now - target["last_sent"]
        if 0 < target.get("failures", 0) <= TRAINER_FAST_RETRIES and elapsed >= TRAINER_RETRY_S:
            return True
        return elapsed >= TRAINER_TARGET_RESEND_S

    def _check_trainer_message(self, device):
        # Message-pump thread: the selected trainer was just heard - the
        # moment to transmit, see _service_trainer_target()
        if self._selected.get(ROLE_POWER) != (device.device_type, device.device_number)                 or device.device_type != DEVICE_TYPE_FITNESS_EQUIPMENT:
            return
        with self._request_lock:
            target = self._trainer_target
            if target is None:
                return
            target["heard"] = True
            due = self._trainer_send_due(target, time.monotonic())
        if due:
            self._wake_event.set()

    def _check_calibration_message(self, message, device):
        # Message-pump thread: note that the target meter was just heard
        # (the moment to transmit) and pick out its calibration response.
        with self._request_lock:
            cal = self._calibration
            if cal is None or device.device_type != 11 or device.device_number != cal["device_number"]:
                return
            payload = bytes(message.get("payload", b"") or b"")
            if len(payload) == 8 and payload[0] == CALIBRATION_PAGE and \
                    payload[1] in (CALIBRATION_RESPONSE_SUCCESS, CALIBRATION_RESPONSE_FAILED):
                self._calibration = None
                success = payload[1] == CALIBRATION_RESPONSE_SUCCESS
                offset = int.from_bytes(payload[6:8], "little", signed=True)
                attempts = cal["attempts"]
            else:
                cal["heard"] = True
                success = None
        if success is None:
            self._wake_event.set()
            return
        result = "success" if success else "failed"
        ClassAntLogger.log_event("CALIBRATE", f"Power meter {device.device_number} calibration {result}, offset={offset}")
        self.calibrationUpdate.emit(device.device_number, result, offset, attempts)

    def _shutdown(self):
        try:
            self.recovery.stop()
        except Exception:
            pass

        try:
            self.ant.stop()
            self.ant.close()
        except Exception:
            pass

    def _startup_cleanup(self):
        # Old-run leftovers shouldn't carry forward into a fresh session.
        for path in STALE_FILES:
            try:
                os.remove(path)
            except OSError:
                pass

        self._known_devices = frozenset()
        self._selected = {}
        self._connected = False
        self._initialized = False
        self._error = None

        with self._request_lock:
            self._pending_connect = None
            self._pending_disconnect = False
            self._connect_requested = False
            self._ready = False
        self._wake_event.clear()

        # Nothing is selected yet at startup - a bystander ANT+ device
        # (another person's sensor sharing the same network key, or
        # anything glimpsed during scanning but never picked) shouldn't
        # be able to trigger a channel retry or a full stick reset. See
        # RecoveryManager.set_watched_devices().
        self.recovery.set_watched_devices(set())

        ClassAntLogger.log_event("ANT", "New ANT session starting")

    def _log_warning(self, message):
        # RecoveryManager's on_event hook - channel retry/device reset
        # events go to the unified log
        ClassAntLogger.log_event("RECOVERY", message)

    def _on_message(self, message, device, changed):
        # RESPONSE_EVENT (0x40) messages are channel-level, not
        # per-device, so they always arrive with device=None - forward
        # them to RecoveryManager before the early-out below. See
        # RecoveryManager.notify_channel_event() for why this only
        # records the event rather than reacting to it here: reacting
        # from this thread (the ANT message pump's own callback) risks
        # deadlocking a later device-reset against this same thread.
        event_name = message.get("event_name")
        if event_name:
            self.recovery.notify_channel_event(event_name, message.get("channel"))
            # Logged here regardless of whether RecoveryManager treats it
            # as actionable, so the unified log shows the full protocol
            # picture (what the channel is doing), not just the events
            # that happened to trigger a retry/reset.
            if event_name not in _SKIP_LOGGING_EVENTS:
                #raw codes for anything the native layer has no name for, so it can still be identified
                raw = ""
                if event_name == "UNKNOWN":
                    raw = f", code=0x{message.get('event_code', 0):02X}, response_to=0x{message.get('response_to_message_id', 0):02X}"
                ClassAntLogger.log_event(
                    "ANT_EVENT",
                    f"{event_name} (channel={message.get('channel')}{raw})",
                )

        if device is None:
            return

        identity = (device.device_type, device.device_number)

        if self._calibration is not None:
            self._check_calibration_message(message, device)

        if self._trainer_target is not None:
            self._check_trainer_message(device)

        if identity not in self._known_devices:
            self._known_devices = self._known_devices | {identity}
            ClassAntLogger.log_event(
                "SCAN",
                f"Discovered device type={device.device_type} "
                f"number={device.device_number}",
            )
            # Emitted from this (the ANT message-pump) thread - Qt queues
            # delivery onto the GUI thread automatically since the setup
            # screen's slot lives there, so this is safe without any
            # extra locking on the UI side.
            self.deviceDiscovered.emit(device.device_type, device.device_number)

        self._check_battery(identity, device)

        if changed:
            role = ROLE_BY_DEVICE_TYPE.get(device.device_type)
            if role is not None:
                # AntDevicesPoller re-derives the currently selected
                # device for this role and its own page-count bookkeeping
                # when notified, exactly as it used to do on its 100ms
                # timer tick - so it's fine (and simpler/safer) to emit
                # this for every role-relevant device that changes, not
                # only the one currently selected/connected.
                self.dataUpdated.emit(role)

    def _check_battery(self, identity, device):
        battery = device.battery
        if battery is None:
            return
        state = (battery.status, battery.level_percent)
        if self._battery_seen.get(identity) == state:
            return
        self._battery_seen[identity] = state
        level = f" {battery.level_percent}%" if battery.level_percent is not None else ""
        voltage = f" {battery.voltage:.2f}V" if battery.voltage is not None else ""
        ClassAntLogger.log_event(
            "BATTERY",
            f"Device type={device.device_type} number={device.device_number} "
            f"battery {battery.status_name}{level}{voltage}",
        )
        if identity in self._selected.values():
            self.batteryChanged.emit()
