"""The whole app on a fake ANT stick, as SimulantTests/run_app_test.py drives it against SimulANT+ but with the
sensors' pages made up here: MainWindow, its bridge thread, the setup screen's auto-select and connect, a ride on
the workout screen (its clock stepped a second at a time), grading, the synopsis, back to setup and the exit.
It doesn't replace the SimulANT+ tests: those check the radio side (real channels, timing and dropouts on the air)"""

import json
import os
import time

import pytest
from PyQt6 import sip
from PyQt6.QtCore import QEvent
from PyQt6.QtWidgets import QApplication

from AppState import AppState
import ClassAntDevices as CAD
import fakes
import KeyShortcuts
import WorkoutData
from ant.manager import ANTManager, MODE_CHANNELS, MODE_SCAN
from ant.profiles.fitness_equipment import basic_resistance_payload, target_power_payload


pytestmark = pytest.mark.whole_app

FTP = 200
WORKOUT = "T:App Flow\n1:10,0.75\n2:10,0.5,1.0\n3:2;1:5,1.2;0:5\n1:10,1.6\n"
#same power ramps and one block intervals, which the setup screen writes out as the blocks they amount to
CONVERTED_WORKOUT = ("T:Converted\n2:20,0.6,0.6\n3:3;2:20,0.7,0.7|Up|5|10\n3:4;1:10,1.1|Go|3\n3:2;0:10\n"
                     "3:2;2:10,0.7,0.7;0:10\n")


def wait_until(condition, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        QApplication.processEvents()
        if condition():
            return True
        time.sleep(0.01)
    return False


class Sensors:
    """Made-up sensor pages, a round per call"""
    def __init__(self, native, trainer=False):
        self.native = native
        self.trainer = trainer
        self.i = 0
        self.watts = 180

    def round(self):
        self.i += 1
        i = self.i
        if self.trainer:
            self.native.deliver(fakes.trainer_page(300, self.watts + i % 3, cadence=88, transmission_type=5,
                                                   channel=self.native_channel(17)))
        else:
            self.native.deliver(fakes.power_page(100, self.watts + i % 3, cadence=90, transmission_type=5,
                                                 event_count=i, channel=self.native_channel(11)))
            self.native.deliver(fakes.combined_page(400, (i*256) & 0xFFFF, (i*3//8) & 0xFFFF, (i*256) & 0xFFFF,
                                                    i & 0xFFFF, transmission_type=1, channel=self.native_channel(121)))
        self.native.deliver(fakes.hr_page(200, 130 + i % 5, i & 0xFF, (i*256) & 0xFFFF, transmission_type=1,
                                          channel=self.native_channel(120)))

    def native_channel(self, device_type):
        return self.channels.get(device_type, 0) if hasattr(self, "channels") else 0


@pytest.fixture
def app_window(qapp, monkeypatch):
    with open("savedSettingsANT.json", "w") as f:
        json.dump({"wheelCircumference": 2.1, "ftp": FTP, "maxHr": 185, "maxWatts": 1200, "powerDriveCutoff": 300,
                   "musicVolume": 0}, f)
    native = fakes.FakeNative()
    monkeypatch.setattr(CAD, "ANTManager", lambda on_log=None: ANTManager(native, on_log=on_log))
    monkeypatch.setattr(CAD, "SCAN_SETTLE_S", 0.3)
    import AppTheme
    import AppStyle
    from PyQt6.QtGui import QFont
    AppTheme.install()
    monkeypatch.setattr(AppTheme, "register_fonts", lambda: [])
    AppState.defaultFont = QFont(qapp.font())
    qapp.setStyleSheet(AppStyle.APP_STYLESHEET)
    AppTheme.clear()
    KeyShortcuts.install(qapp)
    errors = []
    monkeypatch.setattr("sys.excepthook", lambda *info: errors.append(info))
    from ClassMainWindow import MainWindow
    WorkoutData.clear()
    window = MainWindow()
    window.show()
    yield window, native, errors
    bridge = window.startExe
    bridge.stop()
    bridge.wait(5000)
    for owner in (getattr(window, "workoutHudWidget", None), getattr(window, "synopsisWidget", None),
                  window.setupWidget):
        for name in ("timer", "blinkTimer", "smoothBarTimer"):
            timer = getattr(owner, name, None) if owner is not None and not sip.isdeleted(owner) else None
            if timer is not None and not sip.isdeleted(timer):
                timer.stop()
    for widget in QApplication.topLevelWidgets():
        widget.hide()
    #(AppLayout keeps the app's one main window to rescale it)
    import AppLayout
    AppLayout._window = None
    #just the main window (its screens with it), as the app's own screens are discarded: the popups are left to
    #their owners
    window.deleteLater()
    #(processEvents alone leaves deleteLater()s waiting for an event loop that never runs here)
    QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete.value)
    QApplication.processEvents()


def connect_sensors(window, native, sensors):
    bridge, setup = window.startExe, window.setupWidget
    assert wait_until(bridge.is_initialized)
    #heard while scanning, until the setup screen has picked them and the bridge is on their own channels
    assert wait_until(lambda: (sensors.round(), bridge.is_ready())[1], timeout=15)
    sensors.channels = {identity[0]: channel for channel, identity in bridge.ant.channel_devices().items()}
    return bridge, setup


def load_workout(setup, text=WORKOUT):
    path = os.path.abspath("app_flow.awo").replace("\\", "/")
    with open(path, "w") as f:
        f.write(text)
    assert setup.process_workout_file(path)
    AppState.workoutFilename, AppState.workoutFilenameShort = path, "app_flow.awo"
    setup.check_enable_start_button()


def ride(window, sensors, seconds):
    """The workout screen's clock stepped a second at a time (as LayoutViewer.run_hud does), the sensors
    sending a round of pages each second"""
    hud = window.workoutHudWidget
    hud.timer.stop()
    for _ in range(seconds):
        sensors.round()
        QApplication.processEvents()
        hud.lastTimestamp = time.monotonic() - 1
        hud.process_events()
        if hud.workoutCompleteShown:
            break
    return hud


def test_sensors_auto_connect_ride_and_synopsis(app_window):
    window, native, errors = app_window
    sensors = Sensors(native)
    bridge, setup = connect_sensors(window, native, sensors)
    #auto-selected: one of each, so connected without asking
    assert bridge.selected_roles() == {"power", "hrm", "speed_cadence"}
    assert bridge.ant.mode == MODE_CHANNELS
    assert AppState.ftp == str(FTP) and AppState.wheelCircumference in ("2.1", "2.10")

    load_workout(setup)
    assert AppState.workoutDurationSeconds == 50
    assert wait_until(setup.introStartButton.isEnabled)
    setup.introStartButton.click()
    hud = window.workoutHudWidget
    assert hud is not None and hud.isVisible() and not setup.isVisible()
    hud.startEndButton.click()
    assert hud.startedBool

    ride(window, sensors, 60)
    assert hud.workoutCompleteShown
    #the readings reached the gauges and the ride's history
    assert hud.wattGaugeDisplay.ui.widget.value >= 180
    assert len(WorkoutData.power) > 40 and len(WorkoutData.heartRate) > 40 and len(WorkoutData.speedCadence) > 40
    assert 130 <= hud.hrGaugeDisplay.ui.widget.value <= 135
    #every block and interval rep graded: 2 blocks, 2 reps of 2, 1 block
    assert len([g for g in AppState.blockGrades.values() if g["pct"] is not None]) == 7
    assert errors == []

    #Esc: the End Workout prompt; then Exit to Synopsis
    window.escape_pressed()
    from ClassConfirmEndWorkoutWindow import ConfirmEndWorkoutWindow
    assert isinstance(hud.w, ConfirmEndWorkoutWindow) and hud.w.isVisible()
    window.escape_pressed()          #again: the same prompt, not a second one
    hud.w.endWorkoutConfirmButton.click()
    synopsis = window.synopsisWidget
    #(its best power figures need real time between readings: the ride above was squeezed into about a second)
    assert wait_until(synopsis.isVisible) and not hud.isVisible()
    assert errors == []

    #back to setup: disconnected, scanning, then connected again by itself
    synopsis.SynopsisRestartButton.click()
    assert wait_until(lambda: bridge.ant.mode == MODE_SCAN and setup.isVisible())
    sensors.channels = {}
    assert wait_until(lambda: (sensors.round(), bridge.is_ready())[1], timeout=15)
    assert errors == []

    #the exit prompt, and Yes closes the stick
    window.escape_pressed()
    from ClassConfirmExitWindow import ConfirmExitWindow
    assert isinstance(window.w, ConfirmExitWindow) and window.w.isVisible()
    window.w.exitSynopsisConfirmButton.click()
    assert bridge.isFinished() and native.names()[-2:] == ["stop_message_pump", "close"]


def test_converted_blocks_ride_and_grade(app_window):
    window, native, errors = app_window
    sensors = Sensors(native)
    bridge, setup = connect_sensors(window, native, sensors)
    load_workout(setup, CONVERTED_WORKOUT)
    assert AppState.workoutDurationSeconds == 20 + 60 + 40 + 20 + 40
    assert wait_until(setup.introStartButton.isEnabled)
    setup.introStartButton.click()
    window.workoutHudWidget.startEndButton.click()
    hud = ride(window, sensors, 200)
    assert hud.workoutCompleteShown
    #every block graded: the steady "ramp", the 60s, 40s and 20s blocks, 2 reps of 2
    assert len(AppState.blockGrades) == 8
    assert [text for _, _, text in AppState.workoutMessages] == ["Up"]*3 + ["Go"]*4
    assert errors == []


def test_free_mode_ride(app_window):
    window, native, errors = app_window
    sensors = Sensors(native)
    bridge, setup = connect_sensors(window, native, sensors)
    setup.freeModeButton.click() if hasattr(setup, "freeModeButton") else \
        next(b for b, m in setup.modeButtons if m == "free").click()
    assert not AppState.workoutMode
    assert wait_until(setup.introStartButton.isEnabled)
    setup.introStartButton.click()
    ride(window, sensors, 20)
    assert AppState.secondsPassed >= 15
    assert window.workoutHudWidget.wattGaugeDisplay.ui.widget.value >= 180
    window.escape_pressed()
    window.workoutHudWidget.w.endWorkoutConfirmButton.click()
    assert wait_until(window.synopsisWidget.isVisible)
    assert errors == []


def test_trainer_follows_the_workout(app_window):
    window, native, errors = app_window
    sensors = Sensors(native, trainer=True)
    bridge, setup = window.startExe, window.setupWidget
    assert wait_until(bridge.is_initialized)
    #no speed/cadence sensor: picked automatically but connected by hand, as with SimulANT+
    assert wait_until(lambda: (sensors.round(), setup.pwrSelected and setup.hrmSelected)[1], timeout=15)
    setup.connect_selected_devices()
    setup.w.connectForSureButton.click()
    assert wait_until(lambda: (sensors.round(), bridge.is_ready())[1])
    sensors.channels = {identity[0]: channel for channel, identity in bridge.ant.channel_devices().items()}
    assert bridge.trainer_selected()

    load_workout(setup)
    assert wait_until(setup.introStartButton.isEnabled)
    setup.introStartButton.click()
    window.workoutHudWidget.startEndButton.click()
    ride(window, sensors, 60)
    #the bridge thread sends as the trainer is heard
    for _ in range(20):
        sensors.round()
        time.sleep(0.02)
    sent = [payload for _, payload, _ in native.calls_to("send_acknowledged")]
    targets = {payload[6] | (payload[7] << 8) for payload in sent if payload[0] == 0x31}
    watts = {quarter/4 for quarter in targets}
    #150W first block, the ramp up from 100W to 200W, the intervals' 240W
    assert 150 in watts and 240 in watts
    assert any(100 < w < 200 and w != 150 for w in watts)
    #the 320W block is over the 300W Power Drive Cutoff: released, never asked for
    assert basic_resistance_payload(0) in sent and 320 not in watts
    assert errors == []
    #ending the ride holds the trainer at 0W
    window.escape_pressed()
    window.workoutHudWidget.w.endWorkoutConfirmButton.click()
    assert wait_until(lambda: (sensors.round(), target_power_payload(0) in
                               [p for _, p, _ in native.calls_to("send_acknowledged")])[1])
