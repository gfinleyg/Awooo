"""
PreviewRide.py

A workout ridden without an ANT stick: a fake bridge stands in for it, a simulated rider gives the readings, and
run_hud() steps the real workout screen (HUDWindow) through the ride a second at a time. LayoutViewer.py shows its
screens this way, and the theme picker (ClassThemeSelectWindow.py) its preview of the workout screen.

The app keeps a ride's state in AppState and WorkoutData. RideSandbox gives a simulated ride its own copy of them, so
one can be built (and redrawn) inside the running app, even mid-workout, without touching the real ride's.
"""

import datetime
import math
import os
import random
import time
from types import SimpleNamespace

from PyQt6.QtCore import QObject, Qt, pyqtSignal
from PyQt6.QtWidgets import QLabel, QVBoxLayout, QWidget

from AppState import AppState
from ClassAntDevices import ROLE_POWER, ROLE_HRM, ROLE_SPEED_CADENCE, ROLE_BATTERY_NAMES
import WorkoutData


###----------------------------------------------------------------------------------------------
### Fake ANT bridge: everything connected, nothing ever changes
###----------------------------------------------------------------------------------------------
class FakeBridge(QObject):
    deviceDiscovered = pyqtSignal(int, int)
    devicesConnected = pyqtSignal()
    devicesDisconnected = pyqtSignal()
    dataUpdated = pyqtSignal(str)
    statusMessage = pyqtSignal(str)
    errorOccurred = pyqtSignal(str)
    calibrationUpdate = pyqtSignal(int, str, object, int)
    batteryChanged = pyqtSignal()

    def __init__(self, batteryWarnings=False, trainer=False):
        super().__init__()
        self.batteryWarnings = batteryWarnings
        #FE-C trainer as the power device: free mode gets the Watts slider, the setup screen its Max Watts box
        self.trainer = trainer

    @property
    def power_type(self): return 17 if self.trainer else 11
    @property
    def devices(self): return [f"{self.power_type} 12345", "120 23456", "121 34567"]

    def found_devices(self): return list(self.devices)
    def is_initialized(self): return True
    def scan_settle_remaining(self): return 0
    def take_error(self): return None
    def waiting_for_stick(self): return None
    def selected_roles(self): return {ROLE_POWER, ROLE_HRM, ROLE_SPEED_CADENCE}
    def selected_device(self, role): return SimpleNamespace(device_type=self.power_type, device_number=12345, battery=None)
    def is_connect_requested(self): return True
    def is_ready(self): return True
    def low_battery_warnings(self):
        return [f"{name} battery low (5%)" for name in ROLE_BATTERY_NAMES.values()] if self.batteryWarnings else []
    def trainer_selected(self): return self.trainer
    def start(self): pass
    def stop(self): pass
    def clear_connection_flags(self): pass
    def request_connect(self, selected): pass
    def request_disconnect(self): pass
    def request_power_calibration(self, device_number): pass
    def cancel_power_calibration(self): pass
    def set_trainer_target_power(self, watts): pass
    def release_trainer_power(self): pass


###----------------------------------------------------------------------------------------------
### Simulated rider
###----------------------------------------------------------------------------------------------
class Rider:
    def __init__(self, seed=7):
        self.rng = random.Random(seed)
        self.hr = 95.0

    def sample(self, seconds, requestedPower, cadenceTarget=False):
        #(Musical Cadence Mode asks for a cadence, not watts)
        base = requestedPower if requestedPower > 0 and not cadenceTarget else 140
        power = max(0, base + self.rng.gauss(0, 8) + 6*math.sin(seconds/7))
        targetHr = 95 + 85*min(power/max(int(AppState.ftp), 1), 1.1)
        self.hr += (targetHr - self.hr)*0.04
        cadence = ((requestedPower if cadenceTarget and requestedPower > 0 else 88)
                   + 4*math.sin(seconds/23) + self.rng.gauss(0, 1.5))
        speed = 3.2*power**0.33 #MPH
        return int(power), int(self.hr + self.rng.gauss(0, 0.6)), cadence, speed


class ViewerMusicPlayer:
    """Stands in for MusicCadence.MusicPlayer: silent, its clock moved along by run_hud, or by real time while
    the viewer plays (see run_clock)"""
    def __init__(self, seconds=0.0):
        self.seconds = seconds
        self._paused = False
        self.skipped = []
        self.since = None #while the clock runs: when seconds was last brought up to date

    def _catch_up(self):
        if self.since is None:
            return
        now = time.monotonic()
        if not self._paused:
            self.seconds += now - self.since
        self.since = now

    def run_clock(self, running):
        self._catch_up()
        self.since = time.monotonic() if running else None

    @property
    def paused(self):
        return self._paused

    @paused.setter
    def paused(self, paused):
        self._catch_up()
        self._paused = paused

    def position(self):
        self._catch_up()
        return self.seconds

    def stop(self):
        pass


def save_reading(t, reading):
    """A reading into the graph's recent values, as AntDevicesPoller keeps them"""
    power, hr, cadence, speed = reading
    window = AppState.graphSecondsShown - AppState.graphBreakpoint
    for saved, value in ((AppState.savedPowerVals, power), (AppState.savedHRVals, hr),
                         (AppState.savedCadenceVals, int(cadence))):
        saved[0].append(t)
        saved[1].append(value)
        while saved[0][-1] - saved[0][0] > window:
            saved[0].pop(0)
            saved[1].pop(0)


def show_reading(hud, reading):
    """A reading on the HUD's gauges and speed/distance (the distance from hud.viewerRevolutions)"""
    power, hr, cadence, speed = reading
    hud.wattGaugeDisplay.ui.widget.setValue(power)
    hud.hrGaugeDisplay.ui.widget.setValue(hr)
    hud.cadenceGaugeDisplay.ui.widget.setValue(cadence)
    hud.set_speed(str(round(speed, 1)))
    hud.set_distance(str(round(float(AppState.wheelCircumference)*hud.viewerRevolutions*0.000621371, 2)))


def run_hud(hud, untilSeconds, rider, record=None):
    """Steps a HUDWindow's own process_events() one simulated second at a time until the workout
    timer (free mode: the elapsed time) reaches untilSeconds, or the workout ends.
    record(seconds, power, hr, cadence, speed) gets every reading; the HUD's own graph/gauges get
    them like AntDevicesPoller would."""
    hud.timer.stop()
    if hasattr(hud, "smoothBarTimer"):
        hud.smoothBarTimer.stop()
    #the power gauge's rings a second each, one per simulated tick (the app's are shorter, see PowerRings)
    hud.powerRings.set_seconds(1)
    workoutMode = AppState.workoutMode
    if hud.cadenceMode:
        hud.music_start = lambda: setattr(hud, "musicPlayer", ViewerMusicPlayer())
    if workoutMode:
        hud.start_workout()
    drawGraph = hud.hud_graph_data_update
    hud.hud_graph_data_update = lambda: None #only the final state is drawn
    reading = (0, 0, 0, 0)
    revolutions = 0.0
    for _ in range(int(untilSeconds) + 5):
        if workoutMode and (not hud.startedBool or hud.workoutSecondsPassed >= untilSeconds):
            break
        if not workoutMode and AppState.secondsPassed >= untilSeconds:
            break
        hud.lastTimestamp = time.monotonic() - 1
        if hud.cadenceMode and hud.musicPlayer is not None:
            hud.musicPlayer.seconds += 1
        hud.process_events()
        t = AppState.secondsPassed
        reading = rider.sample(t, hud.currentPower, hud.cadenceMode)
        revolutions += reading[3]/2.23694/float(AppState.wheelCircumference)
        save_reading(t, reading)
        if record:
            record(t, *reading)
    hud.hud_graph_data_update = drawGraph
    drawGraph()
    #(to play on from here, see DisplayWindow.play)
    hud.viewerRider = rider
    hud.viewerRevolutions = revolutions
    show_reading(hud, reading)
    hud.update_power_rings()
    if hud.cadenceMode:
        hud.cadenceGaugeDisplay.ui.widget.displayValue = hud.cadenceGaugeDisplay.ui.widget.value #(no easing in)
    if hasattr(hud, "cadenceGaugeDisplay"):
        #the beat caught at its brightest
        hud.update_center_rings(1.0)


def reset_ride_state():
    AppState.secondsPassed = 0.0
    AppState.musicPlayed = []
    AppState.workoutPowerRequestsSeconds.clear()
    AppState.workoutPowerRequestsWatts.clear()
    AppState.workoutPowerRequestsSysTime.clear()
    AppState.savedPowerVals = ([], [])
    AppState.savedHRVals = ([], [])
    AppState.savedCadenceVals = ([], [])
    AppState.totalRevolutions = 0
    #a simulated ride is the only one in its session (see Session.py)
    AppState.sessionRides = []
    AppState.rideStart = None
    AppState.rideRecorded = False
    WorkoutData.clear()


###----------------------------------------------------------------------------------------------
### A simulated ride's own AppState and WorkoutData
###----------------------------------------------------------------------------------------------
#the sensor histories WorkoutData keeps (module attributes, swapped whole)
_HISTORIES = ("power", "heartRate", "speedCadence", "cadence", "speed", "fallbackCadence")


def _state_names():
    return [name for name, value in vars(AppState).items()
            if not name.startswith("__") and not isinstance(value, (staticmethod, classmethod)) and not callable(value)]


def _copy(value):
    #lists, tuples and dicts all the way down (the app changes some of AppState's lists in place); anything else
    #(colors, the size policy) is shared
    if isinstance(value, list):
        return [_copy(v) for v in value]
    if isinstance(value, tuple):
        return tuple(_copy(v) for v in value)
    if isinstance(value, dict):
        return {k: _copy(v) for k, v in value.items()}
    return value


class RideSandbox:
    """with sandbox: AppState and WorkoutData are the simulated ride's own until the block ends, then the app's again
    (and the ride's are kept for the next time). It starts from a copy of the app's. Everything in the block has to
    run straight through (no event loop: the app's own timers would see the ride's state)"""
    def __init__(self):
        self.state = {name: _copy(getattr(AppState, name)) for name in _state_names()}
        self.histories = {name: WorkoutData.SensorHistory(len(getattr(WorkoutData, name).columns))
                          for name in _HISTORIES}
        self.saved = None

    def __enter__(self):
        self.saved = ({name: getattr(AppState, name) for name in self.state},
                      {name: getattr(WorkoutData, name) for name in _HISTORIES})
        for name, value in self.state.items():
            setattr(AppState, name, value)
        for name, history in self.histories.items():
            setattr(WorkoutData, name, history)
        return self

    def __exit__(self, *exc):
        state, histories = self.saved
        self.saved = None
        self.state = {name: getattr(AppState, name) for name in state}
        self.histories = {name: getattr(WorkoutData, name) for name in _HISTORIES}
        for name, value in state.items():
            setattr(AppState, name, value)
        for name, history in histories.items():
            setattr(WorkoutData, name, history)
        return False


###----------------------------------------------------------------------------------------------
### The workout screen, part way through a ride, as a picture (the theme picker's preview)
###----------------------------------------------------------------------------------------------
PREVIEW_WORKOUT = os.path.join("awoWorkouts", "0_Little_bit_of_everything.awo")
PREVIEW_PROGRESS = 0.4 #of the way through the workout (a free ride: of PREVIEW_FREE_SECONDS)
PREVIEW_FREE_SECONDS = 20*60
PREVIEW_RIDER = {"ftp": "250", "maxHr": "185", "wheelCircumference": "2.105"}
APP_TITLE = "Awooo Workout"
#the app's window behind the screen, and its title (see MainWindow and CustomTitleBar)
WINDOW_STYLE = "#ridePreview{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}"
TITLE_STYLE = """font: 900 20pt 'Orbitron'; background-color:#bdd6de; border: 2px solid #bdd6de;
                 border-radius: 16px;"""
TITLE_HEIGHT = 36


class WorkoutPreview:
    """The workout screen PREVIEW_PROGRESS of the way through PREVIEW_WORKOUT (ridden by Rider, in its own
    RideSandbox), in a stand-in for the app's window, drawn offscreen: picture() draws it in the current theme.
    setup is the app's setup screen (it reads the workout file)"""
    def __init__(self, setup, size):
        self.sandbox = RideSandbox()
        self.bridge = FakeBridge()
        self.size = size
        with self.sandbox:
            self.build(setup)

    def build(self, setup):
        import AppLayout
        import FtpColors
        from ClassHUDWindow import HUDWindow
        reset_ride_state()
        for name, value in PREVIEW_RIDER.items():
            setattr(AppState, name, value)
        AppState.workoutMode, AppState.cadenceMode, AppState.ftpTestMode = True, False, None
        AppState.antHRM, AppState.antSpeedCadence, AppState.antCadence, AppState.antSpeed = True, True, False, False
        AppState.ftpRanges = FtpColors.power_ranges()
        FtpColors.update_graph_brushes()
        workout = setup.preview_workout(PREVIEW_WORKOUT, keep=True) if os.path.isfile(PREVIEW_WORKOUT) else None
        AppState.workoutMode = bool(workout and workout["ok"])
        if AppState.workoutMode:
            AppState.workoutFilename = PREVIEW_WORKOUT.replace("\\", "/")
            AppState.workoutFilenameShort = os.path.basename(PREVIEW_WORKOUT)
            title = f"   {APP_TITLE}: {AppState.workout_display_name()}"
        else: #(no workout file to show: the Free Ride screen)
            title = "   " + APP_TITLE
        AppState.startTimestamp = datetime.datetime.now()

        self.window = QWidget()
        self.window.setObjectName("ridePreview")
        self.window.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen)
        self.window.setAttribute(Qt.WidgetAttribute.WA_StyledBackground)
        self.window.setStyleSheet(WINDOW_STYLE)
        layout = QVBoxLayout(self.window)
        titleLabel = QLabel(title)
        titleLabel.setStyleSheet(TITLE_STYLE)
        titleLabel.setFixedHeight(TITLE_HEIGHT)
        layout.addWidget(titleLabel)
        self.hud = HUDWindow(SimpleNamespace(startExe=self.bridge))
        #(it's redrawn in its own sandbox, see picture: not along with the app's screens)
        FtpColors.unwatch(self.hud)
        layout.addWidget(self.hud)
        seconds = (AppState.workoutDurationSeconds if AppState.workoutMode else PREVIEW_FREE_SECONDS)*PREVIEW_PROGRESS
        run_hud(self.hud, seconds, Rider(seed=3))
        self.hud.stop_timers()
        for name in AppLayout.SYMBOL_BUTTONS:
            if getattr(self.hud, name, None) is not None:
                AppLayout.set_symbol(getattr(self.hud, name))

    def picture(self):
        """The window as it looks in the current theme (a QPixmap the window's size)"""
        import AppLayout
        from ClassAnalogGauge import AnalogGaugeWidget
        from ClassLongVerticalLabelSeries import LongVLabelSerial
        import FtpColors
        with self.sandbox:
            FtpColors.update_graph_brushes()
            self.hud.refresh_ftp_colors()
            AppLayout._apply_sizes(self.window)
            self.window.resize(self.size)
            self.window.show()
            AppLayout._settle_layouts(self.window)
            AppLayout.fit_text(self.window)
            self.hud.fit_workout_message()
            #the block lists' rows, laid out now rather than from the event loop (see LongVLabelSerial.renderTimer)
            for blockList in self.window.findChildren(LongVLabelSerial):
                blockList.renderTimer.stop()
                blockList.render_lines()
            #the needles right at their readings (not on their way there)
            for gauge in self.window.findChildren(AnalogGaugeWidget):
                gauge.needleTimer.stop()
                gauge.displayValue = gauge.value
                gauge.paintCacheDirty = True
            AppLayout._settle_layouts(self.window)
            picture = self.window.grab()
            self.window.hide()
            self.hud.messageFitTimer.stop()
        return picture

    def close(self):
        self.hud.stop_timers()
        self.window.deleteLater()
        self.bridge.deleteLater()
