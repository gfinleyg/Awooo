import bisect
import datetime
import math
import re
import time

import pyqtgraph as pg

from PyQt6.QtCore import QEvent, QPoint, QPointF, QRect, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QKeySequence, QPainter, QPen, QShortcut
from PyQt6.QtWidgets import (
    QGridLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSizePolicy,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from ClassAnalogGauge import *
from ClassLongVerticalLabelSeries import *
from ClassSolidColoredColumns import SolidColoredColumns
from ClassSpiralProgressBar import *
from ClassPowerSpiral import PowerSpiral
from ClassSmoothProgressBar import SmoothProgressBar
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *

from AppState import AppState
import FtpColors
from ClassAntDevices import TARGET_POWER_MIN_WATTS, TARGET_POWER_MAX_WATTS
from ClassAntDevicesPoller import AntDevicesPoller
from ClassConfirmEndWorkoutWindow import ConfirmEndWorkoutWindow
import SoundEffects
from WorkoutHelpers import (MinimizedDuration, ProcessWorkoutLine, ProcessNonIntervalWorkoutLineText, GetMaxOrZero,
                            WithDropoutsAsZero, DROPOUT_ZERO_SECONDS, TargetsFromText, IsSongHeader,
                            SONG_HEADER_CSS)
import WorkoutGrading
import KeyShortcuts
import FtpTest
import ClassAntLogger
import MusicCadence
import SavedSettings
import Units
from ClassGradeWidgets import GradeList, GradeOverlay

#Musical Cadence Mode's beat: the nested circles in the cadence gauge's middle brighten on each pedal stroke's
#beat (AppState.musicPulses) and fade back over this long
BEAT_SECONDS = 0.2

#the nested circles in the power gauge's middle show the power over the last while: the outermost the power now,
#each one in from it the average power over this many seconds, just before the time the one outside it covers
#(see set_power_rings)
POWER_RING_SECONDS = 3
#as many rings as fit in the gauge's middle at its size on screen (the screen's resolution and where the gauge sits
#on it) with each at least this wide, so a bigger gauge shows further back; never fewer than POWER_RING_MIN
POWER_RING_MIN_PX = 6
POWER_RING_MIN = 3

#Space starts a workout and then pauses it: it doesn't pause for this long after the start
SPACE_PAUSE_GUARD_SECONDS = 0.5


def new_power_gauge():
    """The power gauge as the HUD shows it, the reading in the middle as big as fits in the nested circles
    there (gaugeChooser colors its band, set_power_rings its circles)"""
    gauge = Gauge()
    widget = gauge.ui.widget
    widget.setNeedleColor(*QColor(AppState.gaugeRed).getRgb())
    widget.setGaugeTheme(3)
    widget.outer_circle_bg = [[0.25, QColor(0,0,0,0)], [0.18, QColor(255, 255, 255, 255)],]
    widget.setMouseTracking(False)
    widget.setEnableBarGraph(False)
    widget.units = "Watts"
    widget.big_needle_center_size = 5
    widget.altUnitsDisplay = True
    widget.fit_value_in_center = True #(fitted, the reading is smaller than the default size: the units keep theirs)
    #paler than the cadence gauge's only enough for the reading over them: the innermost shows the power back then
    widget.center_rings_pale = (0.1, 0.4)
    widget.center_ring_count = POWER_RING_MIN
    widget.center_ring_min_px = POWER_RING_MIN_PX
    return gauge


def set_power_rings(gauge):
    """The power gauge's nested circles, each the band's color at a power: the outermost the power now, then
    the average over the last POWER_RING_SECONDS, then over the POWER_RING_SECONDS before that, and so on in"""
    widget = gauge.ui.widget
    ranges = FtpColors.power_ranges() #(power zones, in Musical Cadence Mode too)
    powers = [widget.reading] + [power_average(POWER_RING_SECONDS*(i - 1), POWER_RING_SECONDS*i)
                                 for i in range(1, widget.ring_count())]
    widget.set_center_rings([FtpColors.zone_color(power, ranges) for power in powers])


def power_average(newest, oldest):
    """The average power read from oldest to newest seconds ago (AppState.savedPowerVals, as the gauge showed it);
    with no reading in that time, the power then (see power_ago)"""
    times, watts = AppState.savedPowerVals
    first = bisect.bisect_right(times, AppState.secondsPassed - oldest)
    last = bisect.bisect_right(times, AppState.secondsPassed - newest)
    if last == first:
        return power_ago((newest + oldest)/2)
    return sum(watts[first:last])/(last - first)


def power_ago(seconds):
    """The power read this many seconds ago (AppState.savedPowerVals, as the gauge showed it: blended
    between readings, 0 before the first and once the power meter had been silent DROPOUT_ZERO_SECONDS)"""
    times, watts = AppState.savedPowerVals
    when = AppState.secondsPassed - seconds
    index = bisect.bisect_right(times, when)
    if index == 0:
        return 0
    if index == len(times):
        return watts[-1] if when - times[-1] <= DROPOUT_ZERO_SECONDS else 0
    t0, t1 = times[index - 1], times[index]
    return watts[index - 1] + (watts[index] - watts[index - 1])*(when - t0)/(t1 - t0)




class HUDWindow(QWidget):

    #QLabel signals
    speedChanged = pyqtSignal(str)
    distanceChanged = pyqtSignal(str)

    #Ride At and the timed workout message under it (see fit_workout_message): font, design size and
    #lines each may use, and the room they keep from the cadence and heart rate gauges either side
    RIDE_AT_STYLE = "font: 900 %dpt 'Orbitron'; background-color: rgba(0, 0, 0, 0);"
    RIDE_AT_POINTS = 35
    RIDE_AT_LONGEST = "Ride At: 1000 Watts" #sized once for its longest text, not on every power change
    RIDE_AT_LINES = 2
    MESSAGE_STYLE = "font: 700 %dpt 'Orbitron'; background-color: rgba(0, 0, 0, 0);"
    MESSAGE_POINTS = 22
    MESSAGE_LINES = 3
    MESSAGE_GAP = 12

    #End-of-workout/block blink timing (see progress_blink_check)
    BLINK_WINDOW_SECONDS = 10
    BLINK_HALF_PERIOD_SECONDS = 0.5
    BLINK_MIN_DURATION_SECONDS = 2
    #the spiral's power and cadence: each the average over the last this many seconds (see record_spiral_sample)
    SPIRAL_AVERAGE_SECONDS = 3.0
    SPIRAL_CADENCE_AVERAGE_SECONDS = 1.0
    
    def __init__(self, parent):
        super(HUDWindow, self).__init__()

        self.parent = parent

        #Musical Cadence Mode (see MusicCadence.py): the targets are cadences, set by music that plays while riding
        #and is the workout's clock (see music_seconds); the cadence gauge takes the power gauge's place
        self.cadenceMode = AppState.workoutMode and AppState.cadenceMode
        self.musicPlayer = None #(see setup_music; every mode's pause/restart/hide checks it)
        self.musicSkipsLogged = 0
        self.musicVolume = 1.0 #the Volume slider's setting (0 to 1), kept for each new player (see music_start)
        #the Volume slider's position (0-100%), saved between runs a moment after it stops moving (see volume_changed)
        savedVolume = SavedSettings.load().get("musicVolume")
        self.musicVolumePercent = savedVolume if isinstance(savedVolume, int) and 0 <= savedVolume <= 100 else 100
        self.volumeSaveTimer = QTimer(self)
        self.volumeSaveTimer.setSingleShot(True)
        self.volumeSaveTimer.setInterval(1000)
        self.volumeSaveTimer.timeout.connect(self.save_volume)

        #an FE-C trainer as the power source gets its target power driven from here:
        #the workout's requested power in workout mode, the Watts slider in free mode.
        #Musical Cadence Mode leaves it released (no target power, see start_workout)
        self.trainerControl = self.parent.startExe.trainer_selected() and not self.cadenceMode
        self.trainerCommanded = False #workout mode only commands the trainer once START has been pressed

        #Window Setup
        self.setWindowTitle("Awooo Workout HUD")
        self.setWindowFlag(Qt.WindowType.WindowCloseButtonHint, False) #removes the x to close box from the window

        #Workout Settings
        self.activeWorkout = 0 #tracks which line of the workoutList + workoutListRaw is running
        self.activeDifficulty = 10 #tracks the difficulty setting set
        self.eventDuration = 0 #initialize the next event timer end point
        self.currentEventText = None #tuple containing information to set current event text
        self.currentEventSettings = None #tuple containing information for current ramp power or interval settings
        self.currentPower = 0 #current power to ride at
        self.blockDuration = 0 #initialize the block duration
        self.currentIntervalBlockIndex = 0
        self.int0TotalDuration = 1 #durations stored for each interval block x multiplier as we get to them for spiral duration, 1 for division safety
        self.int1TotalDuration = 1
        self.int2TotalDuration = 1

        #Block grading (see WorkoutGrading.py): the running block's grader, its AppState.blockGrades key, label
        #and start (seconds since AppState.startTimestamp); the interval being ridden, (workoutList index, reps)
        self.grader = None
        self.gradeKey = None
        self.gradeLabel = ""
        self.gradeStart = 0.0
        self.gradeInterval = (0, 1)
        self.gradeTopTarget = 0 #highest target power asked for in the block, the top of its region on the synopsis graph
        self.lastGradeTarget = 0 #target power at the last tick, the step into the next block sets its start grace
        self.workoutGraded = False #the end of workout grades are showing
        AppState.blockGrades = {}
        AppState.musicPlayed = []

        #an FTP test (see FtpTest.py) isn't graded; its effort's (last block's) average power shows in the spiral instead
        self.ftpTest = FtpTest.test()
        #a ramp test adds its blocks as they're ridden; a restart starts from the workout as built again
        self.rampTest = self.ftpTest is not None and self.ftpTest["kind"] == "ramp"
        self.builtWorkoutList = list(AppState.workoutList)
        self.builtWorkoutSeconds = AppState.workoutDurationSeconds
        #the effort starts with the workout's last block as built: an effort test's free ride, a ramp test's first block
        self.effortStartIndex = len(AppState.workoutList) - 1
        self.reset_ftp_test()

        #These values are emitted from the AntDevicesPoller class (MPH and miles), shown in the distance unit (Units.py)
        self.currentSpeed = "0"
        self.currentDistance = "0"
        self.speedMph = 0.0
        self.speedReadAt = 0.0 #AppState.secondsPassed of the last speed reading
        self.distanceMiles = 0.0

        #Graph related data
        self.timeGraphList = []
        self.timeGraphList.clear()
        self.powerGraphList = []
        self.powerGraphList.clear()
        # track how far back in the workout power list to go when viewing
        self.pastPowerIndex = 0

        #Timer settings (time.monotonic(): a system clock change mid-ride doesn't move the workout clock)
        self.lastTimestamp = time.monotonic()
        self.timeDelta = 0.0 #difference between current AppState.secondsPassed and previous
        self.workoutDelta = 0.0 #how far the workout timers moved on the last tick (see workout_time_delta)
        self.workoutSecondsPassed = 0.0 #initialize the workout timer
        self.blockSecondsPassed = 0.0 #initialize the block workout timer
        self.eventTimerSeconds = 0.0 #initialize the next event timer
        self.int0PassedTime = 0.0 #passed time for each interval block x multiplier needs to be stored too for spiral duration
        self.int1PassedTime = 0.0
        self.int2PassedTime = 0.0
        self.eventOverage = 0.0
        self.blockOverage = 0.0

        #Flags
        self.startedAt = 0.0 #time.monotonic() of the last START (see space_pause)
        self.startedBool = False # apply workout if in workout mode, enable pause button usage which sets requested power to 0 temporarily; picks up again when unpaused
        self.workoutUnpausedBool = False # active once HUD is open, show and record power, cadence, HR, speed/distance
        self.difficultyChangedBool = False #tracks if the difficulty has changed
        self.newWorkoutStart = False #track when the workout is initialized
        self.workoutCompleteShown = False #the end of the workout has been shown (once per run, see process_workout_events)
        self.workoutEnding = False #track when the workout end blinking is in progress (for spiral durations)
        #Blink Toggle Flags (see progress_blink_check)
        self.blinkWorkout = False #workout timer currently in its blinked colors
        self.blinkBlock = False #block timer currently in its blinked colors
        self.spiralBlinkState = "default" #"default", "workout" or "block" colors on the spiral
        #Start Button Blink Handling
        self.startButtonBlink = False
        self.startButtonBlinkTgl = False
        self.startButtonBlinkCounter = 0
        self.startButtonStyle = None #the start/end button's stylesheet now (see process_workout_events)

        #Text styles (laid out for 1920x1200; other screen sizes are scaled, see AppLayout.py)
        xxlTextStyleSheet = "font: 900 34pt 'Orbitron';"
        xlTextStyleSheet = "font: 900 22pt 'Orbitron';"
        lgTextStyleSheet = "font: 900 18pt 'Orbitron';"
        mdTextStyleSheet = "font: 900 16pt 'Orbitron';"
        smTextStyleSheet = "font: 900 10pt 'Orbitron';"
        
        #Widget Style Settings
        self.startButtonBlinkOnStyle = "QPushButton {color: black; \
                                            background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 #1CB5E0, stop: 1 #dadbde); \
                                            border-style: solid; border-width: 2px; border-radius: 15px; padding: 4px; \
                                            font: bold 34px 'Segoe UI Black'; border-color: white;} \
                                            QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #1f15e1);} \
                                            QPushButton:disabled { background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #4a4952, stop: 1 #716f7d); \
                                            color: grey;}"
        self.startButtonBlinkOffStyle = "QPushButton {color: black; \
                                            background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 #1CB5E0, stop: 1 #dadbde); \
                                            border-style: outset; border-width: 2px; border-radius: 15px; padding: 4px; \
                                            font: bold 34px 'Segoe UI Black'; border-color: black;} \
                                            QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #1f15e1);} \
                                            QPushButton:disabled { background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #4a4952, stop: 1 #716f7d); \
                                            color: grey;}"


        ### HUD Widget
        self.setStyleSheet("""
                                QLabel:disabled{color:#808080;}
                           """)
        self.hudLayout = QGridLayout()

        if AppState.workoutMode:
            self.upNextLayout = QGridLayout()
            self.upNextLabelHeader = QLabel("Up Next:")
            self.upNextLabelHeader.setStyleSheet(xlTextStyleSheet)
            self.upNextLabelHeader.setMinimumWidth(490)
            self.upNextLabelHeader.setMaximumHeight(40)
            self.upNextLayout.addWidget(self.upNextLabelHeader, 0,0)
            self.upNextLabelDescription = QLabel()
            self.upNextLabelDescription.setFixedHeight(90)
            self.upNextLabelDescription.setMinimumWidth(490)
            self.upNextLayout.addWidget(self.upNextLabelDescription, 1,0,3,1)
            self.remainingWorkoutHeader = QLabel("Coming up...")
            self.remainingWorkoutHeader.setMaximumHeight(40)
            self.remainingWorkoutHeader.setMinimumWidth(490)
            self.remainingWorkoutHeader.setStyleSheet(xlTextStyleSheet)
            self.upNextLayout.addWidget(self.remainingWorkoutHeader, 4,0)
            self.remainingWorkoutList = LongVLabels(True, AppState.ftp, AppState.ftpRanges)
            #a ramp test's blocks are added as they're ridden, so its list is always empty, not almost done
            self.remainingWorkoutList.ui.widget.showAlmostDone = not self.rampTest
            self.remainingWorkoutList.setMinimumWidth(490)
            self.remainingWorkoutList.setFixedHeight(560)
            # self.remainingWorkoutList.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Minimum)
            self.upNextLayout.addWidget(self.remainingWorkoutList, 5,0,21,1)
            #once the workout is complete each block's grade takes over the whole column (see show_workout_grades)
            self.workoutGradesHeader = QLabel("Workout Grades")
            self.workoutGradesHeader.setStyleSheet(xlTextStyleSheet)
            self.workoutGradesHeader.setMaximumHeight(40)
            self.workoutGradesHeader.hide()
            self.upNextLayout.addWidget(self.workoutGradesHeader, 0,0)
            self.workoutGradeList = GradeList()
            self.workoutGradeList.setMinimumWidth(490)
            self.workoutGradeList.hide()
            self.upNextLayout.addWidget(self.workoutGradeList, 1,0,25,1)
            self.hudLayout.addLayout(self.upNextLayout,0,0,20,6)

            self.currentBlockLayoutWidget = QWidget()
            self.currentBlockLayoutWidget.setFixedHeight(190)
            self.currentBlockLayoutWidget.setMinimumWidth(810)
            self.currentBlockLayout = QGridLayout()
            self.blockMultiplierLabel = QLabel()
            self.blockMultiplierLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.blockMultiplierLabel.setStyleSheet("font: 900 24pt 'Orbitron';")
            self.currentBlockLayout.addWidget(self.blockMultiplierLabel,0,0,6,2)
            self.blockInt3Label0 = QLabel()
            self.blockInt3Label0.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.currentBlockLayout.addWidget(self.blockInt3Label0, 0,2,2,14)
            self.blockInt3Label1 = QLabel()
            self.blockInt3Label1.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.currentBlockLayout.addWidget(self.blockInt3Label1, 2,2,2,14)
            self.blockInt3Label2 = QLabel()
            self.blockInt3Label2.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.currentBlockLayout.addWidget(self.blockInt3Label2, 4,2,2,14)
            self.blockInt2Label0 = QLabel()
            self.blockInt2Label0.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.currentBlockLayout.addWidget(self.blockInt2Label0, 0,2,3,14)
            self.blockInt2Label1 = QLabel()
            self.blockInt2Label1.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.currentBlockLayout.addWidget(self.blockInt2Label1, 3,2,3,14)
            self.blockNonIntLabel = QLabel("Press Start to begin workout")
            self.blockNonIntLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.blockNonIntLabel.setStyleSheet("font: 900 22pt 'Orbitron';")
            self.currentBlockLayout.addWidget(self.blockNonIntLabel, 0,0,6,16)
            self.blockMultiplierLabel.hide()
            self.blockInt2Label0.hide()
            self.blockInt2Label1.hide()
            self.blockInt3Label0.hide()
            self.blockInt3Label1.hide()
            self.blockInt3Label2.hide()
            self.currentBlockProgressBar = SmoothProgressBar()
            self.currentBlockProgressBar.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Minimum)
            self.currentBlockProgressBar.setRange(0,100)
            self.currentBlockProgressBar.setValue(0)
            self.currentBlockProgressBar.setTextVisible(False)
            self.currentBlockLayout.addWidget(self.currentBlockProgressBar, 6,0,1,16)
            self.timeBlockRemainingValue = QLabel()
            self.timeBlockRemainingValue.setMinimumHeight(26)
            self.timeBlockRemainingValue.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.currentBlockLayout.addWidget(self.timeBlockRemainingValue, 7,0,1,16)
            self.currentBlockLayoutWidget.setLayout(self.currentBlockLayout)
            self.hudLayout.addWidget(self.currentBlockLayoutWidget,0,6,4,16, Qt.AlignmentFlag.AlignTop)
        
        self.wattGaugeDisplay = new_power_gauge()
        #(colored for as many rings as fit wherever it ends up, see update_power_rings)
        self.wattGaugeDisplay.ui.widget.ringCountChanged.connect(self.update_power_rings)
        self.gaugeChooser() #the colored band, its colors at set % of FTP
        if self.cadenceMode:
            #in the cadence gauge's usual corner; the cadence gauge is the big one in the middle
            self.wattGaugeDisplay.setMaximumSize(220, 165)
            self.wattGaugeDisplay.setMinimumSize(200, 150)
            self.hudLayout.addWidget(self.wattGaugeDisplay,16,6,4,3, alignment=Qt.AlignmentFlag.AlignBottom)
            #small: a numbered tick every 100W, nothing between
            self.wattGaugeDisplay.ui.widget.scala_subdiv_count = 1
        elif AppState.workoutMode:
            self.hudLayout.addWidget(self.wattGaugeDisplay,4,7,16,15)
        else:
            self.hudLayout.addWidget(self.wattGaugeDisplay,0,4,20,24)
        self.update_power_rings()
        if AppState.shows_cadence():
            #chainring spins at the current cadence
            self.cadenceGaugeDisplay = Gauge(self, 'Chainring.svg', spin=True)
            self.cadenceGaugeDisplay.ui.widget.setNeedleColor(*QColor(AppState.gaugeRed).getRgb())
            self.cadenceGaugeDisplay.ui.widget.setMouseTracking(False)
            self.cadenceGaugeDisplay.ui.widget.outer_circle_bg = [[0, QColor(0,0,0,0)]]#[[0.25, QColor(0,0,0,0)], [0.18, QColor(255, 255, 255, 255)],]
            self.cadenceGaugeDisplay.ui.widget.set_scale_polygon_colors([[0, QColor(0,0,0,0)]]) #Transparent
            self.cadenceGaugeDisplay.ui.widget.bigScaleMarker = QColor(0,0,0,0) #Transparent
            self.cadenceGaugeDisplay.ui.widget.needle_center_bg = [[0, QColor("#FFFFFF")]] #Solid White
            self.cadenceGaugeDisplay.ui.widget.big_needle_center_size = 5 #change the center size
            self.cadenceGaugeDisplay.ui.widget.units = "RPM"
            self.cadenceGaugeDisplay.ui.widget.initial_scale_fontsize = 18
            self.cadenceGaugeDisplay.ui.widget.scale_fontsize = self.cadenceGaugeDisplay.ui.widget.initial_scale_fontsize
            self.cadenceGaugeDisplay.ui.widget.initial_value_fontsize = 56
            self.cadenceGaugeDisplay.ui.widget.value_fontsize = self.cadenceGaugeDisplay.ui.widget.initial_value_fontsize
            self.cadenceGaugeDisplay.ui.widget.setMaxValue(200)
            self.cadenceGaugeDisplay.ui.widget.setScalaCount(1)
            self.cadenceGaugeDisplay.ui.widget.scala_subdiv_count = 1
            self.cadenceGaugeDisplay.ui.widget.setEnableBigScaleGrid(False)
            self.cadenceGaugeDisplay.ui.widget.altUnitsDisplay = True
            self.cadenceGaugeDisplay.ui.widget.value_decimals = 1
            self.cadenceGaugeDisplay.ui.widget.fit_value_in_center = True
            if self.cadenceMode:
                #the cadence zones' colored band (like the power gauge's), numbered ticks every 10 RPM, and the
                #chainring shrunk to sit inside the band
                #0 to 140 RPM (the needle stops at 140, the number in the middle doesn't), a number at each 10 RPM tick
                self.cadenceGaugeDisplay.ui.widget.setMaxValue(140)
                self.cadenceGaugeDisplay.ui.widget.setScalaCount(14)
                self.cadenceGaugeDisplay.ui.widget.initial_scale_fontsize = 13 #(small enough for all 15)
                self.cadenceGaugeDisplay.ui.widget.scale_fontsize = 13
                self.cadenceGaugeDisplay.ui.widget.gauge_color_inner_radius_factor = 0.95 #half the usual band
                self.cadenceGaugeDisplay.ui.widget.setEnableBarGraph(False) #(the band ends at the needle, like power's)
                self.cadenceGaugeDisplay.ui.svg.scale = self.cadenceGaugeDisplay.ui.widget.gauge_color_inner_radius_factor
                self.cadenceGaugeDisplay.ui.widget.set_center_rings(("#ffffff", "#ffffff")) #(colored once there's a target)
                self.hudLayout.addWidget(self.cadenceGaugeDisplay,4,7,16,15)
            elif AppState.workoutMode:
                self.cadenceGaugeDisplay.setMaximumSize(220, 165)
                self.cadenceGaugeDisplay.setMinimumSize(200, 150)
                self.hudLayout.addWidget(self.cadenceGaugeDisplay,16,6,4,3, alignment=Qt.AlignmentFlag.AlignBottom)
            else:
                self.cadenceGaugeDisplay.setMaximumSize(400, 300)
                self.cadenceGaugeDisplay.setMinimumSize(400, 300)
                self.hudLayout.addWidget(self.cadenceGaugeDisplay,10,0,9,11, alignment=Qt.AlignmentFlag.AlignBottom)
        self.gaugeChooser() #(the cadence gauge's band, now it's there)
        #the gauge showing the Ride At target's arc
        self.targetGauge = self.cadenceGaugeDisplay if self.cadenceMode else self.wattGaugeDisplay
        self.rideAtLabel = QLabel()
        self.rideAtLabel.setStyleSheet(self.RIDE_AT_STYLE % self.RIDE_AT_POINTS)
        self.rideAtLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.rideAtLabel.setWordWrap(True) #wraps rather than run into the gauges either side (fit_workout_message)
        #an explicit minimum height (one line): otherwise the layout takes a word wrapped label's minimum
        #height at its narrowest possible width, so the screen would seem to need far more room than it does
        self.rideAtLabel.setMinimumHeight(self.rideAtLabel.fontMetrics().lineSpacing())
        self.rideAtLabel.setMinimumWidth(770)
        #timed workout messages from the .awo file, shown under the ride at label
        self.workoutMessageLabel = QLabel()
        self.workoutMessageLabel.setStyleSheet(self.MESSAGE_STYLE % self.MESSAGE_POINTS)
        self.workoutMessageLabel.setAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop)
        self.workoutMessageLabel.setWordWrap(True)
        #reserve two lines so the ride at label doesn't jump when a message appears or clears (a longer one can wrap to a third)
        self.workoutMessageLabel.setMinimumHeight(2*self.workoutMessageLabel.fontMetrics().lineSpacing())
        #stack both labels at the bottom of the power gauge's rows, over the empty lower part of the gauge
        #their widths (and the margins fit_workout_message gives them) mustn't widen the grid's columns
        for label in (self.rideAtLabel, self.workoutMessageLabel):
            label.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
            label._layoutOwnFit = True #(not AppLayout's text rule: fit_workout_message fits them)
        #fit_workout_message runs once the layout has settled after anything that moves or resizes these
        self.messageFitTimer = QTimer(self)
        self.messageFitTimer.setSingleShot(True)
        self.messageFitTimer.setInterval(0)
        self.messageFitTimer.timeout.connect(self.fit_workout_message)
        self.rideAtWidget = QWidget()
        self.rideAtWidget.setStyleSheet("background-color: rgba(0, 0, 0, 0);")
        rideAtLayout = QVBoxLayout(self.rideAtWidget)
        rideAtLayout.setContentsMargins(0,0,0,0)
        rideAtLayout.setSpacing(0)
        rideAtLayout.addStretch()
        rideAtLayout.addWidget(self.rideAtLabel)
        rideAtLayout.addWidget(self.workoutMessageLabel)
        self.hudLayout.addWidget(self.rideAtWidget,16,7,4,15)
        #the gauges either side can move without the whole HUD resizing
        self.rideAtWidget.installEventFilter(self)
        if AppState.antHRM:
            #pink resting heart, red enlarged heart on each beat at the current heart rate
            self.hrGaugeDisplay = Gauge(self, 'HeartSmaller.svg', beatSvg=('Heart.svg', "#FF8FAB", "#E8102A"))
            self.hrGaugeDisplay.ui.widget.setNeedleColor(*QColor(AppState.gaugeRed).getRgb())
            self.hrGaugeDisplay.ui.widget.setMouseTracking(False)
            self.hrGaugeDisplay.ui.widget.needle_center_bg = [[0, QColor("#FFFFFF")]] #Solid White
            self.hrGaugeDisplay.ui.widget.set_scale_polygon_colors([[0, QColor(0,0,0,0)]]) #Transparent
            self.hrGaugeDisplay.ui.widget.outer_circle_bg = [[0, QColor(0,0,0,0)]] #Transparent
            self.hrGaugeDisplay.ui.widget.big_needle_center_size = 20 #change the center size, lower is bigger, default 8
            self.hrGaugeDisplay.ui.widget.units = "BPM"
            self.hrGaugeDisplay.ui.widget.setMinValue(40)
            self.hrGaugeDisplay.ui.widget.setMaxValue(200)
            self.hrGaugeDisplay.ui.widget.setEnableBarGraph(False)
            self.hrGaugeDisplay.ui.widget.setScalaCount(2)
            self.hrGaugeDisplay.ui.widget.scala_subdiv_count = 5
            if AppState.workoutMode:
                self.hudLayout.addWidget(self.hrGaugeDisplay,15,21,6,4)
            else:
                self.hudLayout.addWidget(self.hrGaugeDisplay,11,23,9,9)


        self.progressGraphLayout = QGridLayout()
        self.progressGraph = WorkoutGraph()
        self.progressGraph.plotScreen.getViewBox().setMouseEnabled(x=False,y=False)
        self.progressGraph.powerVBox.setMouseEnabled(x=False,y=False)
        self.progressGraph.hrCadenceVBox.setMouseEnabled(x=False,y=False)
        #the graph just follows the workout: no mouse (clicks, drags, menu, wheel go to the HUD behind it) or keyboard
        self.progressGraph.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.progressGraph.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.progressGraph.viewport().setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.progressGraph.updateViews()
        self.progressGraph.plotScreen.vb.sigResized.connect(self.progressGraph.updateViews)
        self.progressGraph.setXRange(0,AppState.graphSecondsShown)
        self.progressGraph.setYRange(0,1000,padding=0)
        self.progressGraph.setMaximumHeight(200)
        self.progressGraph.plotScreen.setTitle(None)
        self.progressGraph.plotScreen.showAxis(axis='left', show=False)
        self.progressGraph.plotScreen.showAxis(axis='right', show=False)
        self.progressGraph.plotScreen.hideButtons()
        #the time ticks ("5m") at 0.8x the graph's font size, "Time (minutes)" at 16px (its usual is 18px)
        #(set before the theme first styles the graph, which scales from these)
        timeAxis = self.progressGraph.plotScreen.getAxis('bottom')
        tickFont = QFont(self.progressGraph.font())
        if tickFont.pointSizeF() > 0:
            tickFont.setPointSizeF(tickFont.pointSizeF()*0.8)
        else:
            tickFont.setPixelSize(max(1, round(tickFont.pixelSize()*0.8)))
        timeAxis.setTickFont(tickFont)
        self.progressGraph.plotScreen.setLabel("bottom", "Time (minutes)",
                                               **{**self.progressGraph.axisStyles, "font-size": "16px"})
        #the workout's target bars; Musical Cadence Mode's targets are cadences, drawn on the cadence axis
        targetVBox = self.progressGraph.hrCadenceVBox if self.cadenceMode else self.progressGraph.powerVBox
        self.workoutPowerPlot = SolidColoredColumns(x=[0], y=[],ranges=[],colors=[0])
        targetVBox.addItem(self.workoutPowerPlot)
        self.workoutPowerCompletedPlot = SolidColoredColumns(x=[0], y=[],ranges=[],colors=[0])
        targetVBox.addItem(self.workoutPowerCompletedPlot)
        self.progressGraph.powerVBox.addItem(pg.PlotCurveItem([0,0]))
        self.powerProducedPlot = self.progressGraph.powerVBox.addedItems[-1]
        if AppState.antHRM:
            self.progressGraph.hrCadenceVBox.addItem(pg.PlotCurveItem([0,0]))
            self.hrPlot = self.progressGraph.hrCadenceVBox.addedItems[-1]
        if AppState.shows_cadence():
            self.progressGraph.hrCadenceVBox.addItem(pg.PlotCurveItem([0,0]))
            self.cadencePlot = self.progressGraph.hrCadenceVBox.addedItems[-1]
        #fully transparent plot so the window's shaded background shows through
        self.progressGraph.setBackground(None)
        self.progressGraph.viewport().setAutoFillBackground(False)
        self.progressGraph.setStyleSheet("QGraphicsView { border: 0px; background: transparent; }")
        #subtle dark grid and axis labels so they read against the shaded background
        for axisName in ('bottom', 'left'):
            self.progressGraph.plotScreen.getAxis(axisName).setPen(pg.mkPen(QColor(0,0,0)))
            self.progressGraph.plotScreen.getAxis(axisName).setTextPen(pg.mkPen(QColor(20,20,40)))
        self.progressGraph.plotScreen.showGrid(x=True, y=True, alpha=0.25)
        #thin light blue line with caret ends at the current time splitting what has occurred (left) from what is coming (right);
        #above the workout bars, below the produced curves
        self.nowLine = pg.InfiniteLine(pos=0, angle=90, movable=False, pen=pg.mkPen(QColor("#bdd6de"), width=1.5))
        self.nowLine.addMarker('v', position=0.0, size=14)
        self.nowLine.addMarker('^', position=1.0, size=14)
        self.nowLine.setZValue(5)
        self.progressGraph.powerVBox.addItem(self.nowLine, ignoreBounds=True)
        #small solid diamonds on the latest reading of each sensor curve
        #values are looked up by AppState attribute name each update since those tuples get replaced on reset
        self.currentMarkers = [] #(scatter item, AppState saved values attribute name)
        #what each sensor curve was last drawn from, by AppState saved values attribute name (see plot_saved_vals)
        self.plottedKeys = {}
        markerSources = [(self.progressGraph.powerVBox, "savedPowerVals", FtpColors.SENSOR_COLORS["power"])]
        if AppState.antHRM:
            markerSources.append((self.progressGraph.hrCadenceVBox, "savedHRVals", FtpColors.SENSOR_COLORS["hr"]))
        if AppState.shows_cadence():
            markerSources.append((self.progressGraph.hrCadenceVBox, "savedCadenceVals",
                                  FtpColors.SENSOR_COLORS["cadence"]))
        for vbox, savedName, color in markerSources:
            #(outlined in its line's halo color, to show over the bars like the line does)
            marker = pg.ScatterPlotItem(symbol='d', size=11, brush=pg.mkBrush(color), pen=FtpColors.marker_outline(color))
            marker.setZValue(20)
            vbox.addItem(marker, ignoreBounds=True)
            self.currentMarkers.append((marker, savedName))
        self.progressGraphLayout.addWidget(self.progressGraph)
        self.hudLayout.addLayout(self.progressGraphLayout,20,0,4,30)

        #low battery warnings for the selected devices; keeps its space when hidden so nothing shifts mid-workout
        self.batteryWarningLabel = QLabel()
        self.batteryWarningLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.batteryWarningLabel.setWordWrap(True)
        #half the HUD's normal label font so the warnings leave room for the spiral
        self.batteryWarningLabel.setStyleSheet("font: 900 8pt 'Orbitron'; color:" + AppState.gaugeRed + ";")
        #not AppState.retainSize, that's Fixed and would cap the whole column at this label's width
        batteryPolicy = QSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        batteryPolicy.setRetainSizeWhenHidden(True)
        self.batteryWarningLabel.setSizePolicy(batteryPolicy)
        self.batteryWarningLabel.hide()

        if AppState.workoutMode:
            self.durationLayout = QGridLayout()
            #the workout and the block being ridden, drawn with the power and cadence ridden (see ClassPowerSpiral.py);
            #centered over the timer boxes below it, whatever shape its space is
            self.spiralWorkoutProgress = PowerSpiral()
            self.spiralWorkoutProgress.clear(AppState.workoutDurationSeconds)
            #take all spare room between the block info and the FTP slider
            self.spiralWorkoutProgress.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
            #each block's grade shows in the middle of the spiral as it ends
            self.gradeOverlay = GradeOverlay(self.spiralWorkoutProgress)
            self.durationLayout.addWidget(self.spiralWorkoutProgress, 0,0,10,1)
            self.durationLayout.addWidget(self.batteryWarningLabel,10,0,1,1)
            #rounded timer boxes; Qt draws square corners when the radius is more than half the height,
            #so each box is kept at least twice as tall as its radius
            timerBoxHeight = 34
            self.timerRadius = timerBoxHeight//2 - 2 #styles applied in workout_blink_default()/workout_blink_on()
            self.workoutTimeHeader = QLabel("Workout Time:")
            self.workoutTimeHeader.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.workoutTimeHeader.setMinimumHeight(timerBoxHeight)
            self.durationLayout.addWidget(self.workoutTimeHeader,11,0,1,1)
            self.workoutTime = QLabel("0s / " + MinimizedDuration(AppState.workoutDurationSeconds))
            self.workoutTime.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.workoutTime.setMinimumHeight(timerBoxHeight)
            self.durationLayout.addWidget(self.workoutTime,12,0,1,1)
            self.timeProgressBar = SmoothProgressBar()
            self.timeProgressBar.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Minimum)
            self.timeProgressBar.setRange(0,100*AppState.workoutDurationSeconds)
            self.timeProgressBar.setValue(0)
            self.timeProgressBar.setTextVisible(False)
            self.timeProgressBar.setFixedHeight(timerBoxHeight)
            self.durationLayout.addWidget(self.timeProgressBar, 13,0,1,1)
            self.remainingWorkoutTime = QLabel("Done in: " + MinimizedDuration(AppState.workoutDurationSeconds))
            self.remainingWorkoutTime.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.remainingWorkoutTime.setMinimumHeight(timerBoxHeight)
            self.durationLayout.addWidget(self.remainingWorkoutTime, 14,0,1,1)
            #down through row 14 so the timer boxes sit just above the heart rate gauge (the spiral takes the extra room)
            self.hudLayout.addLayout(self.durationLayout, 0,22,15,8)



        
        self.otherInfo = QGridLayout()
        #free mode has no Workout Time box, so its battery warning sits at the top of this group, above speed/distance
        infoRow = 0
        if not AppState.workoutMode:
            self.otherInfo.addWidget(self.batteryWarningLabel,0,0,1,3)
            infoRow = 1
        self.speedLabel = QLabel("Speed")
        self.speedLabel.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight)
        self.otherInfo.addWidget(self.speedLabel, infoRow,0,1,1)
        self.speedValueLabel = QLabel("0")
        self.speedValueLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.otherInfo.addWidget(self.speedValueLabel, infoRow,1,1,1)
        self.mphLabel = QLabel(Units.speed_unit())
        self.mphLabel.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft)
        self.otherInfo.addWidget(self.mphLabel, infoRow,2,1,1)
        self.distLabel = QLabel("Dist.")
        self.distLabel.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight)
        self.otherInfo.addWidget(self.distLabel, infoRow+1,0,1,1)
        self.distValueLabel = QLabel("0")
        self.distValueLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.otherInfo.addWidget(self.distValueLabel, infoRow+1,1,1,1)
        self.milesLabel = QLabel(Units.distance_unit())
        self.milesLabel.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft)
        self.otherInfo.addWidget(self.milesLabel, infoRow+1,2,1,1)
        #speed and distance only come from a real speed sensor (the power->speed fallback is calibrated from one too)
        for label in (self.speedLabel, self.speedValueLabel, self.mphLabel,
                      self.distLabel, self.distValueLabel, self.milesLabel):
            label.setVisible(AppState.has_speed_sensor())
        timerRow = infoRow + 2
        self.timerLabel = QLabel("Elapsed Time:")
        self.timerLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.otherInfo.addWidget(self.timerLabel,timerRow,0,1,3)
        self.timeElapsed = QLabel("0s")
        self.timeElapsed.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.otherInfo.addWidget(self.timeElapsed,timerRow+1,0,1,3)
        if AppState.workoutMode:
            self.hudLayout.addLayout(self.otherInfo, 16,25,4,5)
        else:
            self.hudLayout.addLayout(self.otherInfo, 2,25,4,5)

        #workout mode's FTP slider; free mode with an FE-C trainer gets the same slider setting the trainer's watts,
        #and Musical Cadence Mode (whose targets don't depend on FTP) the music's volume
        if AppState.workoutMode or self.trainerControl:
            self.difficultyLayout = QGridLayout()
            self.difficultyLabel = QLabel("Volume" if self.cadenceMode else "Difficulty")
            self.difficultyLabel.setStyleSheet("font: 900 14pt 'Orbitron';")
            self.difficultyLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.difficultyLayout.addWidget(self.difficultyLabel,0,0)
            self.difficultyPercentage = QLabel(self.difficulty_ftp_text(0) if AppState.workoutMode else "Watts:")
            #(difficulty_changed adds its color to this)
            self.difficultyTextStyleSheet = "font: 900 14pt 'Orbitron';"
            self.difficultyPercentage.setStyleSheet(self.difficultyTextStyleSheet)
            self.difficultyPercentage.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.difficultyPercentage.setMinimumWidth(120)
            self.difficultyLayout.addWidget(self.difficultyPercentage,1,0)
            sliderRow = 2
            if not AppState.workoutMode:
                #free mode target watts, follows the slider and can be typed in (see trainer_watts_edited)
                self.trainerWattsText = QLineEdit()
                self.trainerWattsText.setStyleSheet(self.difficultyTextStyleSheet)
                self.trainerWattsText.setAlignment(Qt.AlignmentFlag.AlignCenter)
                self.trainerWattsText.setMaxLength(len(str(TARGET_POWER_MAX_WATTS)))
                self.trainerWattsText.setMaximumWidth(120)
                self.trainerWattsText.editingFinished.connect(self.trainer_watts_edited)
                self.difficultyLayout.addWidget(self.trainerWattsText,2,0, alignment=Qt.AlignmentFlag.AlignHCenter)
                sliderRow = 3
            self.difficultySlider = QSlider()
            self.difficultySlider.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Minimum)
            if self.cadenceMode:
                self.difficultySlider.setRange(0,100)
                self.difficultySlider.setValue(self.musicVolumePercent)
            elif AppState.workoutMode:
                self.difficultySlider.setRange(-10,10)
                self.difficultySlider.setValue(0)
            else:
                maxWatts = min(AppState.maxWatts, TARGET_POWER_MAX_WATTS)
                self.difficultySlider.setRange(TARGET_POWER_MIN_WATTS, maxWatts)
                #free mode starts at half of FTP
                self.difficultySlider.setValue(min(round(int(AppState.ftp)*.5), maxWatts))
                #the wheel/Up/Down stop at FTP and the starting watts as well as every 5 W, so either can be got back to
                self.sliderStops = {int(AppState.ftp), self.difficultySlider.value()}
            self.difficultySlider.setSingleStep(1)
            if self.cadenceMode:
                self.difficultySlider.setPageStep(10)
                self.difficultySlider.valueChanged.connect(self.volume_changed)
            elif AppState.workoutMode:
                self.difficultySlider.valueChanged.connect(self.difficulty_changed)
            else:
                self.difficultySlider.valueChanged.connect(self.trainer_watts_changed)
            #no vertical margin on the handle: a negative one let it hang past the groove's ends, cut off at the top/bottom
            self.difficultySlider.setStyleSheet("""
                                                    QSlider::groove:vertical { background:#bdd6de; width:10px; position:absolute; left:20px; right:20px}
                                                    QSlider::handle:vertical { background:%s; width:40px; height:40px; margin:0px -40px; border-style:solid; border-color:#9fc5d1; border-width:1px; border-radius:15px}
                                                    QSlider::handle:vertical:disabled { background:transparent}
                                                """ % AppState.gaugeRed)
            #workout mode enables it on START, free mode has no START so it's live right away, as is the volume
            self.difficultySlider.setEnabled(not AppState.workoutMode or self.cadenceMode)
            if self.cadenceMode:
                self.volume_changed(self.difficultySlider.value())
                self.volumeSaveTimer.stop() #(nothing changed yet)
            self.difficultyLayout.addWidget(self.difficultySlider,sliderRow,0,15-sliderRow,1)
            self.hudLayout.addLayout(self.difficultyLayout, 0,30,15,2)
            if not AppState.workoutMode:
                self.trainer_watts_changed(self.difficultySlider.value())
            #the mouse wheel anywhere on the HUD (but the scrolling lists) and Up/Down move the slider, see nudge_slider;
            #the slider's own wheel handling (several steps a notch) is taken over
            self.sliderWheelRemainder = 0
            self.difficultySlider.installEventFilter(self)
            QShortcut(QKeySequence(Qt.Key.Key_Up), self, activated=lambda: self.nudge_slider(1))
            QShortcut(QKeySequence(Qt.Key.Key_Down), self, activated=lambda: self.nudge_slider(-1))
            KeyShortcuts.add(self.difficultySlider, label="↑ ↓")

        if AppState.workoutMode:
            self.pauseStartEndLayout = QGridLayout()
            self.pauseButton = QPushButton("PAUSE")
            self.pauseButton.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Minimum)
            self.pauseButton.setEnabled(False)
            self.pauseStartEndLayout.addWidget(self.pauseButton,0,0)
            self.startEndButton = QPushButton("START")
            self.startEndButton.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Minimum)
            self.startEndButton.clicked.connect(self.start_workout)
            self.pauseStartEndLayout.addWidget(self.startEndButton,1,0)
            self.hudLayout.addLayout(self.pauseStartEndLayout,15,30,9,2)
            self.startButtonBlink = True
            #Space starts the workout, then pauses/resumes it; Esc (MainWindow.escape_pressed) is END
            KeyShortcuts.add(self.startEndButton, Qt.Key.Key_Space, label="Spacebar", when=lambda: not self.startedBool)
            KeyShortcuts.add(self.pauseButton, Qt.Key.Key_Space, label="Spacebar", when=lambda: self.startedBool,
                             action=self.space_pause)
            KeyShortcuts.add(self.startEndButton, label="Esc", when=self.end_available)
        else:
            self.startEndButton = QPushButton("END")
            self.startEndButton.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Minimum)
            self.startEndButton.clicked.connect(self.confirm_end_workout)
            self.hudLayout.addWidget(self.startEndButton,20,30,4,2)
            KeyShortcuts.add(self.startEndButton, label="Esc")

        self.setLayout(self.hudLayout)

        #Start reading ANT+ data from text files
        self.antDevices = AntDevicesPoller(self)

        #Update Up Next and Remaining Workout Labels, take care of the workoutListRaw once Start is pressed
        if AppState.workoutMode:
            self.block_blink_default()
            self.workout_blink_default()
            # initialize activeWorkout at -1 so that init applies text to update labels and the first pass of next_workout_block() is at 0
            self.activeWorkout = -1
            self.update_upnext_remaining_labels()

        self.speedChanged.connect(self.update_speed_label)
        self.distanceChanged.connect(self.update_distance_label)

        self.parent.startExe.batteryChanged.connect(self.update_battery_warning)
        self.update_battery_warning()

        if self.cadenceMode:
            self.setup_music()

        #Start timer function
        #At this point elapsed time will begin progressing, start storing power, hr, speed and cadence
        #(both timers are children of the HUD, so they go with it when it's deleted; stop_timers() stops them
        #when the workout is left)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.process_events)
        self.timer.setInterval(100)
        self.timer.start()

        #~60fps redraw of just the two timer bars between the 100ms ticks so they glide rather than step
        if AppState.workoutMode:
            self.smoothBarTimer = QTimer(self)
            self.smoothBarTimer.setTimerType(Qt.TimerType.PreciseTimer)
            self.smoothBarTimer.timeout.connect(self.update_smooth_bars)
            self.smoothBarTimer.setInterval(16)
            self.smoothBarTimer.start()

        #redrawn when the power zone colors change; these show only those colors (LayoutViewer.py lists them
        #on its FTP Colors tab instead of with the other colors)
        FtpColors.watch(self, "refresh_ftp_colors")
        Units.watch(self, "units_changed")
        if hasattr(self, "upNextLabelDescription"):
            for widget in (self.blockNonIntLabel, self.blockInt2Label0, self.blockInt2Label1, self.blockInt3Label0,
                           self.blockInt3Label1, self.blockInt3Label2, self.upNextLabelDescription,
                           self.spiralWorkoutProgress):
                widget._ftpColored = True

        
    #the workout is over or being left (END to the synopsis, back to setup): nothing on the HUD moves any more
    def stop_timers(self):
        self.timer.stop()
        if hasattr(self, "smoothBarTimer"):
            self.smoothBarTimer.stop()

    ###HUD Layout functions
    def update_speed_label(self):
        self.speedValueLabel.setText(self.currentSpeed)

    def update_distance_label(self):
        self.distValueLabel.setText(self.currentDistance)

    def update_battery_warning(self):
        warnings = self.parent.startExe.low_battery_warnings()
        self.batteryWarningLabel.setText("\n".join(warnings))
        self.batteryWarningLabel.setVisible(bool(warnings))

    #speed in MPH and distance in miles (as the poller reads them), shown in the distance unit; reading: a new
    #sensor reading (the speed drops to 0 once there hasn't been one for a while, see zero_gauges_on_dropout)
    def set_speed(self, speedStr, reading=True):
        if reading:
            self.speedReadAt = AppState.secondsPassed
        self.speedMph = float(speedStr)
        self.currentSpeed = str(round(Units.speed(self.speedMph), 1))
        self.speedChanged.emit(self.currentSpeed)

    def set_distance(self, distanceStr):
        self.distanceMiles = float(distanceStr)
        self.currentDistance = str(round(Units.distance(self.distanceMiles), 2))
        self.distanceChanged.emit(self.currentDistance)

    #the ⚙ menu's Measurements changed: speed and distance again in the new unit
    def units_changed(self):
        self.mphLabel.setText(Units.speed_unit())
        self.milesLabel.setText(Units.distance_unit())
        self.progressGraph.set_speed_label()
        self.set_speed(self.speedMph, reading=False)
        self.set_distance(self.distanceMiles)

    ###Musical Cadence Mode's music (AppState.musicTracks, see MusicCadence.MusicPlayer); where it is in the ride is
    ###the workout's clock
    def setup_music(self):
        self.musicPlayer = None #made on START, playing until the ride is restarted or left
        self.musicSkipsLogged = 0

    def music_start(self):
        self.music_stop()
        self.musicPlayer = MusicCadence.MusicPlayer(AppState.musicTracks)
        self.musicPlayer.volume = self.musicVolume
        try:
            self.musicPlayer.start()
        except Exception as e:
            #no sound device: the ride goes on, timed by the clock instead
            ClassAntLogger.log_event("ERROR", f"Music: can't play ({type(e).__name__}: {e}), timing the ride by the clock")
            self.musicPlayer = None
            #(as a timed message, which the next tick would otherwise clear)
            AppState.workoutMessages.append((self.workoutSecondsPassed, self.workoutSecondsPassed + 15,
                                             "Music can't be played, see the log"))

    def music_seconds(self):
        """Workout time, from where the music is (None without music, see music_start)"""
        if self.musicPlayer is None:
            return None
        #songs that couldn't be played go to the log (the clock has moved on past them)
        for path, reason in self.musicPlayer.skipped[self.musicSkipsLogged:]:
            #(counted first: a song whose logging fails is still only tried once, not on every tick)
            self.musicSkipsLogged += 1
            ClassAntLogger.log_event("ERROR", f"Music: skipped {path}: {reason}")
        return self.musicPlayer.position()

    def music_pause(self, paused):
        if self.musicPlayer is not None:
            self.musicPlayer.paused = paused

    def music_stop(self):
        if getattr(self, "musicPlayer", None) is not None:
            self.musicPlayer.stop()
            self.musicPlayer = None
            self.musicSkipsLogged = 0

    def update_song_title(self):
        """The title bar: <app>: Musical Cadence Mode: <song> : <time into it>/<its length> while the music plays"""
        appTitle = getattr(self.parent, "APP_TITLE", "Awooo Workout")
        text = f"{appTitle}: {AppState.workout_display_name()}"
        seconds = self.music_seconds()
        if seconds is not None and AppState.musicTracks:
            _, title, start, duration = AppState.musicTracks[self.song_at(seconds)]
            into = min(max(seconds - start, 0.0), duration)
            text += f": {title} : {self.song_time(into)}/{self.song_time(duration)}"
        if text != getattr(self, "songTitle", None):
            self.songTitle = text
            self.parent.setWindowTitle("   " + text)
            if hasattr(self.parent, "title_bar"):
                self.parent.title_bar.set_title("   " + text)

    @staticmethod
    def song_at(seconds):
        """The index in AppState.musicTracks of the song playing at seconds into the music"""
        starts = [track[2] for track in AppState.musicTracks]
        return max(0, bisect.bisect_right(starts, seconds) - 1)

    def record_song(self):
        """Where in the ride each song was heard (AppState.musicPlayed), for the synopsis' song strip. A paused
        song's span runs on through the pause; a skipped one is never heard, so it has none"""
        seconds = self.music_seconds()
        if seconds is None or not AppState.musicTracks:
            return
        index = self.song_at(seconds)
        _, _, start, duration = AppState.musicTracks[index]
        #(the music is over)
        if seconds >= start + duration:
            return
        played = AppState.musicPlayed
        if played:
            #(up to now: the song before ends where this one starts, with no gap of a tick between them)
            played[-1][2] = AppState.secondsPassed
        if not played or played[-1][0] != index:
            played.append([index, AppState.secondsPassed, AppState.secondsPassed])

    @staticmethod
    def song_time(seconds):
        seconds = int(seconds)
        if seconds >= 3600:
            return f"{seconds//3600}:{seconds//60 % 60:02d}:{seconds % 60:02d}"
        return f"{seconds//60}:{seconds % 60:02d}"

    def update_center_rings(self, level=None):
        """The cadence gauge's nested circles: the band's color at the target cadence (so they match the band's tip
        when riding on target), brightened on the music's beat (or at level, given)"""
        if level is None:
            level = 0.0
            seconds = self.music_seconds() if self.startedBool and self.workoutUnpausedBool else None
            if seconds is not None:
                index = bisect.bisect_right(AppState.musicPulses, seconds) - 1
                if index >= 0:
                    level = max(0.0, 1 - (seconds - AppState.musicPulses[index])/BEAT_SECONDS)
        gauge = self.cadenceGaugeDisplay.ui.widget
        color = FtpColors.zone_color(self.currentPower)
        gauge.set_center_rings((color.darker(130), color), level)

    def update_power_rings(self):
        set_power_rings(self.wattGaugeDisplay)

    def workout_time_delta(self):
        """How far the workout moves on this tick: the music's progress in Musical Cadence Mode, else the time
        since the last tick"""
        musicSeconds = self.music_seconds() if self.cadenceMode else None
        if musicSeconds is None:
            return self.timeDelta
        return max(0.0, musicSeconds - self.workoutSecondsPassed)

    def hideEvent(self, event):
        #leaving the workout screen (END, back to setup) stops the music; minimizing the window doesn't
        if self.cadenceMode and not event.spontaneous():
            self.music_stop()
            if self.volumeSaveTimer.isActive():
                self.save_volume()
        super().hideEvent(event)

    def restart_workout(self):
        self.music_stop()
        if AppState.workoutMode:
            #the workout as built (a ramp test added blocks to it), its whole length on the workout timer bar again
            AppState.workoutList = list(self.builtWorkoutList)
            AppState.workoutDurationSeconds = self.builtWorkoutSeconds
            self.timeProgressBar.setRange(0,100*AppState.workoutDurationSeconds)
            #change end workout button back into a start workout button
            self.startEndButton.setText("START")
            try:
                self.startEndButton.clicked.disconnect(self.confirm_end_workout)
            except TypeError:
                pass
            self.startEndButton.clicked.connect(self.start_workout)
            #disable pause button functionality (and let it back up: pausing, the END dialog's included, held it down)
            self.pauseButton.setEnabled(False)
            self.pauseButton.setDown(False)
            self.pauseButton.clicked.disconnect(self.pause_recording)
            #START blinks again until it's pressed
            self.startButtonBlink = True
            #reset and disable the difficulty slider (the volume stays as it's set)
            if not self.cadenceMode:
                self.difficultySlider.setValue(0)
                self.difficultySlider.setEnabled(False)
            #timer and pause tracking booleans set
            self.startedBool = False
            self.workoutUnpausedBool = False
            #reset the workout initialization boolean
            self.newWorkoutStart = False
            self.workoutCompleteShown = False
            #reset workout settings
            self.activeWorkout = 0 #tracks which line of the workoutList + workoutListRaw is running
            self.activeDifficulty = 10 #tracks the difficulty setting set
            self.eventDuration = 0 #initialize the next event timer end point
            self.currentEventText = None #tuple containing information to set current event text
            self.currentEventSettings = None #tuple containing information for current ramp power or interval settings
            #reset timers
            #AppState.secondsPassed = 0 # don't initialize the main timer on reset!       
            self.workoutSecondsPassed = 0.0 #initialize the workout timer
            self.blockSecondsPassed = 0.0 #initialize the block workout timer
            self.eventTimerSeconds = 0.0 #initialize the next event timer
            #reset upcoming and remaining headers
            self.upNextLabelHeader.setText("Up Next:")
            self.remainingWorkoutHeader.setText("Coming up...")
            #reset current block text
            self.blockNonIntLabel.setText("Press Start to begin workout")
            self.blockNonIntLabel.show()
            self.blockMultiplierLabel.hide()
            self.blockInt2Label0.hide()
            self.blockInt2Label1.hide()
            self.blockInt3Label0.hide()
            self.blockInt3Label1.hide()
            self.blockInt3Label2.hide()
            #reset current block progress
            self.block_blink_default()
            self.currentBlockProgressBar.setValue(0)
            self.timeBlockRemainingValue.setText("")
            #reset workout progress
            self.workout_blink_default()
            self.spiralBlinkState = "default"
            self.timeProgressBar.setValue(0)
            self.workoutTime.setText(MinimizedDuration(self.workoutSecondsPassed) + " / " + MinimizedDuration(AppState.workoutDurationSeconds))
            self.remainingWorkoutTime.setText("Done in: " + MinimizedDuration(AppState.workoutDurationSeconds))
            #clear power requests
            self.updateRequestedPowerDisplays()
            self.rideAtLabel.setText("")
            self.workoutMessageLabel.setText("")
            #reset the up next and remaining workout elements
            self.activeWorkout = -1
            self.update_upnext_remaining_labels()
            #reset graph index
            self.pastPowerIndex = 0
            #reset spiral progress
            self.spiralWorkoutProgress.clear(AppState.workoutDurationSeconds)
            #clear the grades, the left column goes back to Up Next
            self.grader = None
            self.gradeKey = None
            self.lastGradeTarget = 0
            AppState.blockGrades = {}
            AppState.musicPlayed = []
            self.gradeOverlay.clear()
            self.show_workout_grades(False)
            #and the FTP test's effort
            self.reset_ftp_test()
            self.pauseButton.setText("PAUSE")
        #reset overall durations
        self.timeElapsed.setText("0s")


    def gaugeChooser(self):
        #each zone fading in where the needle points at its start, for any FTP
        self.wattGaugeDisplay.ui.widget.set_value_colors(FtpColors.zone_stops(FtpColors.power_ranges()))
        if self.cadenceMode and hasattr(self, "cadenceGaugeDisplay"): #(Musical Cadence Mode's own band)
            self.cadenceGaugeDisplay.ui.widget.set_value_colors(FtpColors.zone_stops(AppState.ftpRanges))

    #the target arc's two colors for a power, without the leading pound or alpha blending(transparency)
    def getFtpColor(self, power):
        return FtpColors.gauge_arc(power)

    def refresh_ftp_colors(self):
        """Redraws everything colored by power zone (the colors changed, see FtpColors.py)"""
        self.gaugeChooser()
        if self.targetGauge.ui.widget.arcCenterValue:
            c0, c1 = self.getFtpColor(self.currentPower)
            self.targetGauge.ui.widget.newArcColors = [[0, QColor("#88"+c0)],[.5, QColor("#88"+c1)]]
            self.targetGauge.ui.widget.update()
        if hasattr(self, "upNextLabelDescription"): #a workout's HUD (not free ride's)
            self.updateSpiralColors()
            labels = [self.upNextLabelDescription]
            if not getattr(self, "blinkBlock", False): #(a blinking block gets its colors back when it stops)
                labels += [self.blockNonIntLabel, self.blockInt2Label0, self.blockInt2Label1, self.blockInt3Label0,
                           self.blockInt3Label1, self.blockInt3Label2]
            for label in labels:
                label.setStyleSheet(self.getLabelColor(label.text()))
            for plot in (self.workoutPowerPlot, self.workoutPowerCompletedPlot):
                plot.recolor()
    
    def getLabelColor(self, txt):
        #a label's own sheet beats the HUD's QLabel:disabled rule, so it carries its own: an interval's
        #inactive blocks dim, their background halfway to grey
        css = self.labelColorCss(txt)
        background = re.search(r"background-color:[^;]*;", css)
        dimmed = re.sub(r"#[0-9a-fA-F]{6}", lambda m: self.dimColor(m.group(0)), background.group(0)) if background else ""
        return "QLabel{%s} QLabel:disabled{color:#6a6a6a; %s}" % (css, dimmed)

    def dimColor(self, hexColor, grey=QColor("#b4b5b8"), amount=0.5):
        color = QColor(hexColor)
        return QColor(round(color.red() + (grey.red() - color.red())*amount),
                      round(color.green() + (grey.green() - color.green())*amount),
                      round(color.blue() + (grey.blue() - color.blue())*amount)).name()

    def labelColorCss(self, txt):
        if txt in ["", "You're on the last block, way to go!", 
                   "Press Start to begin workout", 
                   "Workout Complete!\nCooldown, or press END for more options"]:
            return "background-color: #9fc5d1;"
        #(Up Next can be a block with the song it starts under it: colored as the block)
        if IsSongHeader(txt.split("\n")[0]):
            return SONG_HEADER_CSS
        targets = TargetsFromText(txt)
        if targets is None:
            #free ride
            return FtpColors.free_ride_css()
        return FtpColors.block_css(*targets)
    
    def updateRequestedPowerDisplays(self):
        #set the current power (cadence, in Musical Cadence Mode) arc
        self.targetGauge.ui.widget.arcCenterValue = int(self.currentPower)
        c0, c1 = self.getFtpColor(self.currentPower)
        self.targetGauge.ui.widget.newArcColors = [[0, QColor("#88"+c0)],[.5, QColor("#88"+c1)]]
        self.targetGauge.ui.widget.setEnableDrawNewArc()
        #set the ride at label
        if int(self.currentPower) == 0:
            self.rideAtLabel.setText("Free Ride")
        else:
            self.rideAtLabel.setText("Ride At: "+str(self.currentPower)+" "+AppState.target_unit())
        self.updateSpiralColors()

    def updateSpiralColors(self):
        #(the spiral colors itself from the power ridden: redrawn for new power zone colors)
        self.spiralWorkoutProgress.update()

    def spiral_block_time(self, sinceTick=0.0):
        """(seconds along, seconds long) of the whole block for the spiral: an interval's every rep of every
        sub-block"""
        if self.currentEventText is not None and self.currentEventText[0] == 3:
            blockCount = len(self.currentEventText) - 2 #descriptor, multiplier and block count of 2 or 3
            passedTimes = [self.int0PassedTime, self.int1PassedTime, self.int2PassedTime][:blockCount]
            totalDurations = [self.int0TotalDuration, self.int1TotalDuration, self.int2TotalDuration][:blockCount]
            return sum(passedTimes) + sinceTick, sum(totalDurations)
        return self.blockSecondsPassed + sinceTick, self.blockDuration

    def record_spiral_sample(self):
        """The power and cadence being ridden (each a short average) into the spiral's ring and spiral"""
        power = self.recent_average(AppState.savedPowerVals)
        cadence = None #(no cadence to show: the spiral's lines are drawn at the lanes' outer edges)
        if AppState.shows_cadence():
            cadence = self.recent_average(AppState.savedCadenceVals, self.SPIRAL_CADENCE_AVERAGE_SECONDS)
            #the cadence sensor gone quiet: the outer edge while the power's still coming in, else not pedaling
            if cadence is None and power is None:
                cadence = 0
        power = 0 if power is None else power
        self.spiralWorkoutProgress.add_sample(self.workoutSecondsPassed, self.spiral_block_time()[0], power, cadence)

    def recent_average(self, saved, seconds=SPIRAL_AVERAGE_SECONDS):
        """The average of the readings in the last seconds (saved is one of AppState's ([times], [values])), or None
        if there's been none for WorkoutGrading.DROPOUT_SECONDS"""
        times, values = saved
        now = AppState.secondsPassed
        if WorkoutGrading.latest_power(times, values, now) is None:
            return None
        total, count = 0.0, 0
        for i in range(len(times) - 1, -1, -1):
            if now - times[i] > seconds:
                break
            total += values[i]
            count += 1
        return total/count if count else values[-1]


    def difficulty_changed(self, val):
        ##2% difficulty changes
        self.difficultyPercentage.setText(self.difficulty_ftp_text(val))
        #(the label's font stays, only its color changes)
        color = "green" if val == 0 else "red" if val > 0 else "blue"
        self.difficultyPercentage.setStyleSheet(f"{self.difficultyTextStyleSheet} color: {color};")
        self.activeDifficulty = val + 10
        self.difficultyChangedBool = True

    def volume_changed(self, val):
        #Musical Cadence Mode's Volume slider, 0-100%; squared so each step sounds about as big as the next
        self.difficultyPercentage.setText(f"{val}%")
        self.musicVolume = (val/100)**2
        if self.musicPlayer is not None:
            self.musicPlayer.volume = self.musicVolume
        self.musicVolumePercent = val
        self.volumeSaveTimer.start()

    def save_volume(self):
        self.volumeSaveTimer.stop()
        SavedSettings.update(musicVolume=self.musicVolumePercent)

    def nudge_slider(self, steps):
        """The mouse wheel/Up/Down: moves the slider steps of 2% FTP, 5% volume or (free mode) 5 watts, landing on
        a whole step or (free mode) one of sliderStops; nothing while the slider's disabled"""
        slider = getattr(self, "difficultySlider", None)
        if not steps or slider is None or not slider.isEnabled() or not self.isVisible():
            return
        step = 5 if (self.cadenceMode or not AppState.workoutMode) else 1
        stops = getattr(self, "sliderStops", ())
        value = slider.value()
        for _ in range(abs(steps)):
            if steps > 0:
                nextValue = min([(value//step + 1)*step] + [stop for stop in stops if stop > value])
            else:
                nextValue = max([-((-value)//step + 1)*step] + [stop for stop in stops if stop < value])
            value = max(slider.minimum(), min(nextValue, slider.maximum()))
        slider.setValue(value)

    def slider_wheel(self, event):
        #one step per wheel notch (120), touchpads send smaller steps that accumulate
        self.sliderWheelRemainder += event.angleDelta().y()
        steps = int(self.sliderWheelRemainder/120)
        if steps:
            self.sliderWheelRemainder -= steps*120
            self.nudge_slider(steps)
        event.accept()

    def wheelEvent(self, event):
        #reaches here from anywhere on the HUD that doesn't scroll itself (the block and grade lists do)
        if hasattr(self, "difficultySlider"):
            self.slider_wheel(event)
        else:
            super().wheelEvent(event)

    def difficulty_ftp_text(self, val):
        #the FTP at this difficulty (2% steps); a ramp FTP test can be ridden with no FTP set
        if AppState.ftp == "0":
            return "FTP: --"
        return "FTP: " + str(int(AppState.ftp) + int(int(AppState.ftp)*val*.02))

    def trainer_watts_changed(self, val):
        #free mode Watts slider, sets the FE-C trainer's target power directly
        self.trainerWattsText.setText(str(val))
        self.currentPower = val
        self.updateRequestedPowerDisplays()
        self.parent.startExe.set_trainer_target_power(val)

    def trainer_watts_edited(self):
        #typed watts move the slider (which sets the trainer); anything not a whole number in range just goes back
        text = self.trainerWattsText.text().strip()
        if text.isdigit() and self.difficultySlider.minimum() <= int(text) <= self.difficultySlider.maximum():
            self.difficultySlider.setValue(int(text))
        self.trainerWattsText.setText(str(self.difficultySlider.value()))

    def update_trainer_target(self):
        #workout mode: the trainer follows the requested (Ride At) power while the workout runs,
        #except it's released whenever that's above the Power Drive Cutoff, and drops to 0 while paused
        #or once it's complete
        if not self.trainerControl or not self.trainerCommanded or self.cadenceMode:
            return
        if self.startedBool and self.workoutUnpausedBool:
            if AppState.powerDriveCutoff and self.currentPower > AppState.powerDriveCutoff:
                self.parent.startExe.release_trainer_power()
            else:
                self.parent.startExe.set_trainer_target_power(self.currentPower)
        else:
            self.parent.startExe.set_trainer_target_power(0)


    def start_workout(self):
        #change start workout button into end workout button
        self.startEndButton.setText("END")
        self.startEndButton.clicked.disconnect(self.start_workout)
        self.startEndButton.clicked.connect(self.confirm_end_workout)

        #set pause button functionality
        self.pauseButton.setEnabled(True)
        self.pauseButton.clicked.connect(self.pause_recording)

        #enable the difficulty slider (FTP tests don't get one: the test sets the power; Musical Cadence Mode's volume
        #slider is already live)
        self.difficultySlider.setEnabled(self.cadenceMode or not self.ftpTest)

        #timer and pause tracking booleans set
        self.startedBool = True
        self.startedAt = time.monotonic()
        self.workoutUnpausedBool = True

        #set the workout initialization boolean
        self.newWorkoutStart = True

        #turn off the blink button
        self.startButtonBlink = False

        #the trainer follows the workout from here on
        self.trainerCommanded = True

        #Musical Cadence Mode: an FE-C trainer is left free (no target power), and the music starts
        if self.cadenceMode:
            if self.parent.startExe.trainer_selected():
                self.parent.startExe.release_trainer_power()
            self.music_start()

    def process_events(self):
        #initialize the power request times
        if len(AppState.workoutPowerRequestsSeconds) == 0:
            AppState.workoutPowerRequestsSeconds.append(AppState.secondsPassed)
            AppState.workoutPowerRequestsSysTime.append(datetime.datetime.now())
        # increase elapsed time
        newCurrentTime = time.monotonic()
        self.timeDelta = newCurrentTime - self.lastTimestamp
        AppState.secondsPassed += self.timeDelta
        self.lastTimestamp = newCurrentTime
        self.timeElapsed.setText(MinimizedDuration(AppState.secondsPassed))
        #add the graph
        self.hud_graph_data_update()
        self.update_power_rings()
        if AppState.workoutMode:
            self.process_workout_events()
            self.update_trainer_target()
        if self.cadenceMode:
            self.update_song_title()
            self.record_song()

        # with open("deltaLog.txt", "a") as f:
        #     f.write(str(self.timeDelta)+"\n")

    def update_smooth_bars(self):
        if self.cadenceMode:
            self.update_center_rings()
        #only while the timer is actually running; paused, pre-start and finished bars hold their last set value
        if not (self.workoutUnpausedBool and self.startedBool) or self.newWorkoutStart:
            return
        #time since the last 100ms tick, capped at a tick-and-a-half so a stalled tick can't run the bars ahead
        sinceTick = min(time.monotonic() - self.lastTimestamp, 0.15)
        musicSeconds = self.music_seconds() if self.cadenceMode else None
        if musicSeconds is not None:
            #how far the music has got past the last tick
            sinceTick = min(max(0.0, musicSeconds - self.workoutSecondsPassed), 0.15)
        if AppState.workoutDurationSeconds > 0:
            self.timeProgressBar.setFraction((self.workoutSecondsPassed + sinceTick)/AppState.workoutDurationSeconds)
        if self.blockDuration > 0:
            self.currentBlockProgressBar.setFraction((self.blockSecondsPassed + sinceTick)/self.blockDuration)
            if AppState.workoutDurationSeconds > 0:
                self.update_spiral_progress(sinceTick)
        self.progress_blink_check(sinceTick)

    #sinceTick is how far past the last 100ms tick to draw, so update_smooth_bars() can move the spiral between ticks
    def update_spiral_progress(self, sinceTick=0.0):
        if self.currentEventText is None:
            return
        self.spiralWorkoutProgress.set_progress(self.workoutSecondsPassed + sinceTick, AppState.workoutDurationSeconds,
                                                *self.spiral_block_time(sinceTick))

    def process_workout_events(self):
        #a paused ramp test's countdown on the pause button
        self.update_ramp_pause_countdown()
        # blink the start button if not running
        if self.startButtonBlink:
            self.startButtonBlinkCounter += self.timeDelta
            if self.startButtonBlinkCounter > .3:
                self.startButtonBlinkCounter = 0
                self.startButtonBlinkTgl = not self.startButtonBlinkTgl
        #(only restyled when the look changes: each stylesheet goes through the theme and re-polishes the button)
        startButtonStyle = self.startButtonBlinkOnStyle if self.startButtonBlink and self.startButtonBlinkTgl \
            else self.startButtonBlinkOffStyle
        if startButtonStyle is not self.startButtonStyle:
            self.startButtonStyle = startButtonStyle
            self.startEndButton.setStyleSheet(startButtonStyle)
        # track label changes
        upcomingRemainingUpdated = False
        clusterDisplayUpdated = False
        #Handle end of workout at the end of the duration of the final event (once: nothing changes after it)
        if self.workout_complete() and not self.workoutCompleteShown:
            self.workoutCompleteShown = True
            #Hide any interval blocks that were open, show single block
            self.blockMultiplierLabel.hide()
            self.blockInt2Label0.hide()
            self.blockInt2Label1.hide()
            self.blockInt3Label0.hide()
            self.blockInt3Label1.hide()
            self.blockInt3Label2.hide()
            self.blockNonIntLabel.show()
            #Set blinking objects to default states
            self.workout_blink_default()
            self.block_blink_default()
            #Disable Pause button
            self.pauseButton.setEnabled(False)
            #Workout done sound
            SoundEffects.play("complete")
            #grade the last block, then every block's grade takes over the left column
            if not self.workoutGraded:
                finishedGrade = self.finish_grading()
                if finishedGrade is not None:
                    self.gradeOverlay.show_grade(finishedGrade)
                if self.ftpTest:
                    #nothing graded; the spiral keeps the effort's average
                    self.workoutGraded = True
                    AppState.ftpTestResult = self.ftp_test_result(completed=True)
                else:
                    self.show_workout_grades(True)
            #set to paused (first: it shows "Paused" in Ride At, cleared below)
            self.pause_recording()
            #Workout done announcements
            self.blockNonIntLabel.setAlignment(Qt.AlignmentFlag.AlignCenter) #Center the end announcement
            self.blockNonIntLabel.setText("Workout Complete!\nCooldown, or press END for more options")
            self.blockNonIntLabel.setStyleSheet(self.getLabelColor(self.blockNonIntLabel.text()))
            self.workoutTime.setText("Done")
            self.remainingWorkoutTime.setText("")
            self.timeBlockRemainingValue.setText("Workout Complete")
            self.rideAtLabel.setText("")
            self.upNextLabelHeader.setText("")
            self.remainingWorkoutHeader.setText("")
            self.upNextLabelDescription.setText("")
            self.upNextLabelDescription.setStyleSheet("") #drop the light blue box, nothing is up next
            self.remainingWorkoutList.ui.widget.setFullText("DONE")
            #reset and disable difficulty slider (the volume stays as it's set)
            if not self.cadenceMode:
                self.difficultySlider.setValue(0)
                self.difficultySlider.setEnabled(False)
            #end the progress bars
            self.currentBlockProgressBar.setRange(0,1)
            self.currentBlockProgressBar.setValue(1)
            self.timeProgressBar.setRange(0,1)
            self.timeProgressBar.setValue(1)
            self.spiralWorkoutProgress.set_progress(AppState.workoutDurationSeconds, AppState.workoutDurationSeconds,
                                                    *self.spiral_block_time())
            #Workout is no longer started
            self.startedBool = False
        #handle workout power = 0 while paused
        if not self.workoutUnpausedBool:
            #set requested power to zero while paused
            self.store_workout_power_time(0)
        #handle workout events if unpaused and started
        if self.workoutUnpausedBool and self.startedBool:
            #increment timers and timer related events if we're not on the first workout
            if not self.newWorkoutStart:
                #(the music's progress in Musical Cadence Mode, so the targets can't drift from the music)
                self.workoutDelta = self.workout_time_delta()
                self.blockSecondsPassed += self.workoutDelta
                self.workoutSecondsPassed += self.workoutDelta
                self.eventTimerSeconds += self.workoutDelta
                #append current workout power to the graph
                self.store_workout_power_time(self.currentPower)
                #grade this tick against the power being asked for
                self.grade_tick()
                self.ftp_test_tick()
                #spiral duration handling 
                if self.currentEventText[0] == 3:
                    #spiral time progress update for interval, only update time for the current interval block
                    if self.currentIntervalBlockIndex == 0:
                        self.int0PassedTime += self.workoutDelta
                    elif self.currentIntervalBlockIndex == 1:
                        self.int1PassedTime += self.workoutDelta
                    else:
                        self.int2PassedTime += self.workoutDelta
                self.update_spiral_progress()
                self.record_spiral_sample()
            #increase progress bars and text
            self.timeProgressBar.setValue(int(self.workoutSecondsPassed*100))
            self.currentBlockProgressBar.setValue(int(self.blockSecondsPassed*100))
            self.timeBlockRemainingValue.setText(f"Block time remaining: {MinimizedDuration(int(self.blockDuration - self.blockSecondsPassed))}")
            self.workoutTime.setText(MinimizedDuration(self.workoutSecondsPassed) + " / " + MinimizedDuration(AppState.workoutDurationSeconds))
            self.remainingWorkoutTime.setText("Done in: " + MinimizedDuration(AppState.workoutDurationSeconds-self.workoutSecondsPassed))
            #check for an expired event timer or if this workout needs initializing
            if self.newWorkoutStart or \
                    self.eventTimerSeconds > self.eventDuration or \
                    self.blockSecondsPassed > self.blockDuration:
                #if the event timer expired, get the portion of a second that passed since then
                if self.eventTimerSeconds > self.eventDuration:
                    self.eventOverage = self.eventTimerSeconds - self.eventDuration
                    if self.eventOverage < 0:
                        self.eventOverage = 0.0
                #if the block timer expired, get the portion of a second that passed since then
                if self.blockSecondsPassed > self.blockDuration:
                    self.blockOverage = self.blockSecondsPassed - self.blockDuration
                    if self.blockOverage < 0:
                        self.blockOverage = 0.0
                #check for workout end
                if self.activeWorkout + 1 == len(AppState.workoutList):
                    pass
                else:
                    #next event detected, reset the event notifiers
                    self.eventTimerSeconds = self.eventOverage
                    self.eventDuration = 0
                    # increment activeWorkout
                    self.activeWorkout = self.activeWorkout + 1
                    # get next workout line
                    currentBlock = AppState.workoutList[self.activeWorkout]
                    if currentBlock[0] in [0,1,2,3]: #new workout block detected
                        #grade the block that just ended; the new one is graded from here (an interval from its first rep)
                        finishedGrade = self.finish_grading()
                        gradedIndex, gradedBlock = self.activeWorkout, currentBlock
                        #initialize block level settings
                        self.blockDuration = 0.0
                        # hide interval workout blocks and show standard block by default
                        self.blockMultiplierLabel.hide()
                        self.blockInt2Label0.hide()
                        self.blockInt2Label1.hide()
                        self.blockInt3Label0.hide()
                        self.blockInt3Label1.hide()
                        self.blockInt3Label2.hide()
                        self.blockNonIntLabel.show()
                        #skip timer reinitialization for the first workout
                        if not self.newWorkoutStart:
                            self.blockSecondsPassed = self.blockOverage
                        self.currentPower = 0
                        # free ride (0,duration,position)
                        if currentBlock[0] == 0:
                            self.blockDuration = currentBlock[1]
                            self.eventDuration = currentBlock[1]
                            self.blockNonIntLabel.setText(ProcessWorkoutLine(currentBlock,10)[0])
                            self.blockNonIntLabel.setStyleSheet(self.getLabelColor(ProcessWorkoutLine(currentBlock,10)[0]))
                            self.currentEventText = currentBlock
                            self.currentEventSettings = currentBlock
                        # steady state (1,duration,(power),position)
                        elif currentBlock[0] == 1:
                            self.blockDuration = currentBlock[1]
                            self.eventDuration = currentBlock[1]
                            self.currentPower = currentBlock[2][self.activeDifficulty]
                            self.blockNonIntLabel.setText(ProcessWorkoutLine(currentBlock,self.activeDifficulty)[0])
                            self.blockNonIntLabel.setStyleSheet(self.getLabelColor(ProcessWorkoutLine(currentBlock,self.activeDifficulty)[0]))
                            self.currentEventText = currentBlock
                            self.currentEventSettings = currentBlock
                        # ramp (2,duration,(start, stop),position)
                        elif currentBlock[0] == 2:
                            self.blockDuration = currentBlock[1]
                            self.blockNonIntLabel.setText(ProcessWorkoutLine(currentBlock,self.activeDifficulty)[0])
                            self.blockNonIntLabel.setStyleSheet(self.getLabelColor(ProcessWorkoutLine(currentBlock,self.activeDifficulty)[0]))
                            self.currentEventText = currentBlock
                            # next workout list element will have will have power and duration start info
                            # ramp power (4,durationEvent,(power))
                            self.activeWorkout = self.activeWorkout + 1
                            currentBlock = AppState.workoutList[self.activeWorkout]
                            self.currentEventSettings = currentBlock
                            self.eventDuration = currentBlock[1]
                            self.currentPower = currentBlock[2][self.activeDifficulty]
                        elif currentBlock[0] == 3:
                            #hide non-interval current workout info
                            self.blockNonIntLabel.hide()
                            #initialize spiral time interval values
                            self.int0TotalDuration = 1 #durations stored for each interval block x multiplier as we get to them for spiral duration, 1 for division safety
                            self.int1TotalDuration = 1
                            self.int2TotalDuration = 1
                            self.int0PassedTime = 0 #passed time for each interval block x multiplier needs to be stored too for spiral duration
                            self.int1PassedTime = 0
                            self.int2PassedTime = 0
                            #store the interval text info
                            self.currentEventText = currentBlock
                            #get current text info for the interval
                            intervalText = ProcessWorkoutLine(currentBlock, self.activeDifficulty)
                            #set the multiplier
                            self.blockMultiplierLabel.setText(f"{intervalText[0]}")
                            #set the current interval block index
                            self.currentIntervalBlockIndex = 0
                            #get spiral durations
                            self.int0TotalDuration = currentBlock[1] * currentBlock[2][1]
                            self.int1TotalDuration = currentBlock[1] * currentBlock[3][1]
                            #handle current views
                            self.blockMultiplierLabel.show()
                            if len(currentBlock) == 4: #descriptor, multiplier and block count of 2
                                self.blockInt2Label0.setText(intervalText[1])
                                self.blockInt2Label0.setStyleSheet(self.getLabelColor(intervalText[1]))
                                self.blockInt2Label0.show()
                                self.blockInt2Label0.setEnabled(True)
                                self.blockInt2Label1.setText(intervalText[2])
                                self.blockInt2Label1.setStyleSheet(self.getLabelColor(intervalText[2]))
                                self.blockInt2Label1.show()
                                self.blockInt2Label1.setEnabled(False)
                            if len(currentBlock) == 5: #descriptor, multiplier and block count of 3
                                self.blockInt3Label0.setText(intervalText[1])
                                self.blockInt3Label0.setStyleSheet(self.getLabelColor(intervalText[1]))
                                self.blockInt3Label0.show()
                                self.blockInt3Label0.setEnabled(True)
                                self.blockInt3Label1.setText(intervalText[2])
                                self.blockInt3Label1.setStyleSheet(self.getLabelColor(intervalText[2]))
                                self.blockInt3Label1.show()
                                self.blockInt3Label1.setEnabled(False)
                                self.blockInt3Label2.setText(intervalText[3])
                                self.blockInt3Label2.setStyleSheet(self.getLabelColor(intervalText[3]))
                                self.blockInt3Label2.show()
                                self.blockInt3Label2.setEnabled(False)
                                #get remaining spiral duration
                                self.int2TotalDuration = currentBlock[1] * currentBlock[4][1]
                            # next workout list element will have will have power and duration start info, block ramp will have another jump
                            # interval block (5,multiplier,intervalIndex,(intervalBlock))
                            self.activeWorkout = self.activeWorkout + 1
                            currentBlock = AppState.workoutList[self.activeWorkout]
                            self.blockDuration = currentBlock[3][1]
                            if currentBlock[3][0] == 0: #free
                                self.eventDuration = currentBlock[3][1]
                                self.currentPower = 0
                            elif currentBlock[3][0] == 1: #steady
                                self.eventDuration = currentBlock[3][1]
                                self.currentPower = currentBlock[3][2][self.activeDifficulty]
                            else: #ramp
                                self.activeWorkout = self.activeWorkout + 1
                                currentBlock = AppState.workoutList[self.activeWorkout]
                                self.eventDuration = currentBlock[1]
                                self.currentPower = currentBlock[2][self.activeDifficulty]
                            self.currentEventSettings = currentBlock
                            #set default blink state for the interval
                            self.block_blink_default()
                        # end interval handling ; elif currentBlockRaw[0] == "3":
                        #the new block's spiral (an interval's: all its reps)
                        self.spiralWorkoutProgress.set_block(self.spiral_block_time()[1])

                        if gradedBlock[0] == 3:
                            self.gradeInterval = (gradedIndex, gradedBlock[1])
                            self.begin_grading((gradedIndex, 0, 0), gradedBlock[2])
                        else:
                            self.begin_grading((gradedIndex, 0, 0), gradedBlock)
                        if finishedGrade is not None:
                            self.gradeOverlay.show_grade(finishedGrade, self.blockDuration)

                        #update remaining steps for a new block
                        self.update_upnext_remaining_labels()
                        # update remaining block time tied to progress bar for current block complete
                        self.timeBlockRemainingValue.setText(f"Block time remaining: {MinimizedDuration(int(self.blockDuration))}")
                        self.currentBlockProgressBar.setRange(0,int(self.blockDuration*100))
                        self.currentBlockProgressBar.setValue(0)
                        clusterDisplayUpdated = True
                        upcomingRemainingUpdated = True
                    elif currentBlock[0] == 4: #new ramp power detected (4,durationEvent,(power))
                        self.currentEventSettings = currentBlock
                        self.eventDuration = currentBlock[1]
                        self.currentPower = currentBlock[2][self.activeDifficulty]
                    elif currentBlock[0] == 5: #new interval block detected (5,multiplier,intervalIndex,(intervalBlock))
                        #grade the interval block that just ended; each rep of each interval block is graded on its own
                        finishedGrade = self.finish_grading()
                        intervalIndex, intervalReps = self.gradeInterval
                        gradedKey, gradedBlock = (intervalIndex, currentBlock[2], intervalReps - currentBlock[1]), currentBlock[3]
                        #set the multiplier
                        self.blockMultiplierLabel.setText(f"{currentBlock[1]}x")
                        #set the current interval block index
                        self.currentIntervalBlockIndex = currentBlock[2]
                        #reset the block timer seconds
                        self.blockSecondsPassed = self.blockOverage
                        #handle current views
                        if len(self.currentEventText) == 4: #descriptor, multiplier and block count of 2
                            if self.currentIntervalBlockIndex == 0:
                                self.blockInt2Label0.setEnabled(True)
                                self.blockInt2Label1.setEnabled(False)
                            else:
                                self.blockInt2Label0.setEnabled(False)
                                self.blockInt2Label1.setEnabled(True)
                        if len(self.currentEventText) == 5: #descriptor, multiplier and block count of 3
                            if self.currentIntervalBlockIndex == 0:
                                self.blockInt3Label0.setEnabled(True)
                                self.blockInt3Label1.setEnabled(False)
                                self.blockInt3Label2.setEnabled(False)
                            elif self.currentIntervalBlockIndex == 1:
                                self.blockInt3Label0.setEnabled(False)
                                self.blockInt3Label1.setEnabled(True)
                                self.blockInt3Label2.setEnabled(False)
                            else:
                                self.blockInt3Label0.setEnabled(False)
                                self.blockInt3Label1.setEnabled(False)
                                self.blockInt3Label2.setEnabled(True)
                        # next workout list element will have will have power and duration start info, block ramp will have another jump
                        # interval block (5,multiplier,intervalIndex,(intervalBlock))
                        self.blockDuration = currentBlock[3][1]
                        #check if interval type
                        if currentBlock[3][0] == 0: #set power to 0 if interval block is free ride
                            self.eventDuration = currentBlock[3][1]
                            self.currentPower = 0
                        elif currentBlock[3][0] == 1:
                            self.eventDuration = currentBlock[3][1]
                            self.currentPower = currentBlock[3][2][self.activeDifficulty]
                        else:
                            #ramp found, move to the next event to get power and set up for event times
                            self.activeWorkout = self.activeWorkout + 1
                            currentBlock = AppState.workoutList[self.activeWorkout]
                            self.currentPower = currentBlock[2][self.activeDifficulty]
                            self.eventDuration = currentBlock[1]
                        self.currentEventSettings = currentBlock
                        # update remaining block time tied to progress bar for current block complete
                        self.timeBlockRemainingValue.setText(f"Block time remaining: {MinimizedDuration(int(self.blockDuration))}")
                        self.currentBlockProgressBar.setRange(0,int(self.blockDuration*100))
                        self.currentBlockProgressBar.setValue(0)
                        self.block_blink_default()
                        self.begin_grading(gradedKey, gradedBlock)
                        if finishedGrade is not None:
                            self.gradeOverlay.show_grade(finishedGrade, self.blockDuration)
                        #end new interval block
                    #set the current power arc now that self.current power has been set
                    self.updateRequestedPowerDisplays()
                    self.newWorkoutStart = False
            #end new event, check for a blink state change now that we're guaranteed to have currentEventText set
            self.progress_blink_check()
            self.show_effort_average()
        #end if Unpaused and workout hasn't ended

        #show or clear the timed workout message for the current workout time
        self.update_workout_message()

        #change difficulty if workout isn't complete and affected values haven't already changed
        if self.difficultyChangedBool and self.startedBool:
            if not upcomingRemainingUpdated:
                self.update_upnext_remaining_labels()
            if not clusterDisplayUpdated:
                #skip free ride, no current power or cluster changes for it
                #steady
                if self.currentEventText[0] == 1:
                    self.currentPower = self.currentEventText[2][self.activeDifficulty]
                    self.updateRequestedPowerDisplays()
                    self.blockNonIntLabel.setText(ProcessWorkoutLine(self.currentEventText,self.activeDifficulty)[0])
                    self.blockNonIntLabel.setStyleSheet(self.getLabelColor(ProcessWorkoutLine(self.currentEventText,self.activeDifficulty)[0]))
                elif self.currentEventText[0] == 2:
                    self.currentPower = self.currentEventSettings[2][self.activeDifficulty]
                    self.updateRequestedPowerDisplays()
                    self.blockNonIntLabel.setText(ProcessWorkoutLine(self.currentEventText,self.activeDifficulty)[0])
                    self.blockNonIntLabel.setStyleSheet(self.getLabelColor(ProcessWorkoutLine(self.currentEventText,self.activeDifficulty)[0]))
                elif self.currentEventText[0] == 3:
                    if self.currentEventSettings[0] == 5: #free or steady
                        self.currentPower = 0 #default for free
                        if self.currentEventSettings[3][0] == 1: #ensure this is a steady block
                            self.currentPower = self.currentEventSettings[3][2][self.activeDifficulty]
                    else: #if event settings are not 5, they must be 4 here, ramp event
                        self.currentPower = self.currentEventSettings[2][self.activeDifficulty]
                    self.updateRequestedPowerDisplays()
                    #and the current block interval changes
                    intervalText = ProcessWorkoutLine(self.currentEventText,self.activeDifficulty)
                    if len(self.currentEventText) == 4: #descriptor, multiplier and block count of 2
                        self.blockInt2Label0.setText(intervalText[1])
                        self.blockInt2Label0.setStyleSheet(self.getLabelColor(intervalText[1]))
                        self.blockInt2Label1.setText(intervalText[2])
                        self.blockInt2Label1.setStyleSheet(self.getLabelColor(intervalText[2]))
                    if len(self.currentEventText) == 5: #descriptor, multiplier and block count of 3
                        self.blockInt3Label0.setText(intervalText[1])
                        self.blockInt3Label0.setStyleSheet(self.getLabelColor(intervalText[1]))
                        self.blockInt3Label1.setText(intervalText[2])
                        self.blockInt3Label1.setStyleSheet(self.getLabelColor(intervalText[2]))
                        self.blockInt3Label2.setText(intervalText[3])
                        self.blockInt3Label2.setStyleSheet(self.getLabelColor(intervalText[3]))
            self.difficultyChangedBool = False #signifies that the difficulty change is complete
        #end difficulty handling

    def update_workout_message(self):
        #workout time only advances while unpaused, so a paused message stays up until the ride resumes
        text = ""
        if self.startedBool:
            latestStart = -1
            for msgStart, msgEnd, msgText in AppState.workoutMessages:
                #when messages overlap, the one that started most recently wins
                if msgStart <= self.workoutSecondsPassed < msgEnd and msgStart >= latestStart:
                    latestStart = msgStart
                    text = msgText
            #a ramp test's not pedaling warning comes first
            if self.ftpTestMessage:
                text = self.ftpTestMessage
        if self.workoutMessageLabel.text() != text:
            self.set_workout_message(text)

    def set_workout_message(self, text):
        self.workoutMessageLabel.setText(text)
        self.fit_workout_message()
        #again once the new text has been laid out
        self.messageFitTimer.start()

    def fit_workout_message(self):
        """Keeps Ride At and the message under it centered on the power gauge and clear of the cadence
        and heart rate gauges either side (whichever are showing): their text wraps to stay between
        them, and only if it still needs more lines than it has does the font shrink"""
        self.fit_between_gauges(self.rideAtLabel, self.RIDE_AT_STYLE, self.RIDE_AT_POINTS, self.RIDE_AT_LINES,
                                self.RIDE_AT_LONGEST)
        self.fit_between_gauges(self.workoutMessageLabel, self.MESSAGE_STYLE, self.MESSAGE_POINTS,
                                self.MESSAGE_LINES, self.workoutMessageLabel.text())

    def fit_between_gauges(self, label, style, points, lines, text):
        labelRect = QRect(label.mapTo(self, QPoint(0, 0)), label.size())
        #centered on the power gauge, else on the label's own space
        center = labelRect.center().x()
        #(the cadence gauge is in the middle in Musical Cadence Mode, the power gauge beside it)
        middle = getattr(self, "targetGauge", None) or getattr(self, "wattGaugeDisplay", None)
        if middle is not None and middle.isVisible():
            center = QRect(middle.mapTo(self, QPoint(0, 0)), middle.size()).center().x()
        left, right = 0, self.width()
        sides = [g for g in (getattr(self, "wattGaugeDisplay", None), getattr(self, "cadenceGaugeDisplay", None),
                             getattr(self, "hrGaugeDisplay", None)) if g is not middle]
        for gauge in sides:
            if gauge is None or not gauge.isVisible():
                continue
            rect = QRect(gauge.mapTo(self, QPoint(0, 0)), gauge.size())
            if rect.bottom() < labelRect.top() or rect.top() > labelRect.bottom():
                continue #not beside the label
            if rect.center().x() < center:
                left = max(left, rect.right())
            else:
                right = min(right, rect.left())
        #the same room either side of the center, so the text stays centered on it
        half = max(20, min(center - (left + self.MESSAGE_GAP), (right - self.MESSAGE_GAP) - center,
                           center - labelRect.left(), labelRect.right() - center))
        marginLeft = max(0, center - half - labelRect.left())
        marginRight = max(0, labelRect.right() - (center + half))
        label.setContentsMargins(marginLeft, 0, marginRight, 0)
        width = max(40, label.width() - marginLeft - marginRight)
        label.setStyleSheet(style % points)
        while text and points > 6:
            metrics = label.fontMetrics()
            needed = metrics.boundingRect(QRect(0, 0, width, 100000), Qt.TextFlag.TextWordWrap, text)
            if needed.height() <= lines*metrics.lineSpacing() and needed.width() <= width:
                break
            points -= 1
            label.setStyleSheet(style % points)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        #once the layout has placed everything at the new size
        self.messageFitTimer.start()

    def showEvent(self, event):
        super().showEvent(event)
        #built (and fitted) while hidden, before the layout gave everything its real place
        self.messageFitTimer.start()

    def eventFilter(self, obj, event):
        if obj is self.rideAtWidget and event.type() in (QEvent.Type.Resize, QEvent.Type.Move, QEvent.Type.Show):
            self.messageFitTimer.start()
        #the slider: the wheel moves it a step a notch (see slider_wheel)
        if event.type() == QEvent.Type.Wheel and obj is getattr(self, "difficultySlider", None):
            self.slider_wheel(event)
            return True
        return super().eventFilter(obj, event)

    def store_workout_power_time(self, power):
        powerChanged = True
        if len(AppState.workoutPowerRequestsWatts) > 0:
            if AppState.workoutPowerRequestsWatts[-1] == power:
                # power didn't change from the last reading, just update the timestamps
                powerChanged = False
                AppState.workoutPowerRequestsSeconds[-1] = AppState.secondsPassed
                AppState.workoutPowerRequestsSysTime[-1] = datetime.datetime.now()

        if powerChanged:
            AppState.workoutPowerRequestsSeconds.append(AppState.secondsPassed)
            AppState.workoutPowerRequestsWatts.append(power)
            AppState.workoutPowerRequestsSysTime.append(datetime.datetime.now())

    ###Block grading (see WorkoutGrading.py)
    def begin_grading(self, key, block):
        """Starts grading a block (or one rep of an interval block): block is its (type, duration, ...) tuple"""
        if self.ftpTest: #FTP tests aren't graded
            return
        #Musical Cadence Mode grades cadence against the target cadence, with the block's own target standing in for
        #FTP (an intensity of 1: the grader's usual tolerances at threshold)
        ftp = AppState.ftp
        if self.cadenceMode:
            ftp = block[2][self.activeDifficulty] if block[0] == 1 else 1
        self.grader = WorkoutGrading.BlockGrader(block[1], self.lastGradeTarget, ftp, block[0] == 0)
        self.gradeKey = key
        self.gradeLabel = ProcessNonIntervalWorkoutLineText(block, self.activeDifficulty)[0]
        self.gradeStart = AppState.secondsPassed
        self.gradeTopTarget = 0

    def grade_tick(self):
        #ticks only come while the workout runs, so paused time is never graded
        if self.grader is None:
            return
        actualVals = AppState.savedCadenceVals if self.cadenceMode else AppState.savedPowerVals
        actual = WorkoutGrading.latest_power(*actualVals, AppState.secondsPassed)
        self.grader.add(self.blockSecondsPassed, self.workoutDelta, self.currentPower, actual)
        self.lastGradeTarget = self.currentPower
        self.gradeTopTarget = max(self.gradeTopTarget, self.currentPower)

    def finish_grading(self, endedEarly=False):
        """Records the running block's grade in AppState.blockGrades; its letter, or None if there was no block
        or nothing in it could be graded (the power sensor was out the whole time). endedEarly: the workout
        was ended part way through the block, which is graded up to now"""
        if self.grader is None:
            return None
        result = self.grader.finish(self.blockSecondsPassed if endedEarly else None)
        result.update({"label": self.gradeLabel, "start": self.gradeStart, "top": self.gradeTopTarget,
                       "end": AppState.secondsPassed})
        AppState.blockGrades[self.gradeKey] = result
        self.grader = None
        return None if result["pct"] is None else WorkoutGrading.letter(result["pct"])

    ###FTP test (see FtpTest.py)
    def reset_ftp_test(self):
        #an effort test's average power, a ramp test's best 60s average
        self.effortAverage = FtpTest.BestRollingAverage() if self.rampTest else FtpTest.EffortAverage()
        self.notPedalingSeconds = 0.0 #ramp test: how long the rider has gone without pedaling
        self.ftpTestMessage = "" #ramp test: the not pedaling warning, shown in place of the timed messages
        self.rampPausedAt = None #ramp test: when the ramp was paused (time.monotonic()), None while it isn't
        AppState.ftpTestResult = None

    def workout_complete(self):
        #the end of the final block has been reached
        return self.activeWorkout+1 == len(AppState.workoutList) and self.blockSecondsPassed >= self.blockDuration

    def update_ramp_pause_countdown(self):
        """A paused ramp's pause button counts down the seconds left before unpausing starts the ramp over"""
        if self.rampPausedAt is None:
            return
        remaining = math.ceil(FtpTest.RAMP_PAUSE_SECONDS - (time.monotonic() - self.rampPausedAt))
        text = f"PAUSE\n{remaining}s" if remaining > 0 else "PAUSE\nRESET"
        if getattr(self.pauseButton, "_themeOrigText", self.pauseButton.text()) != text:
            self.pauseButton.setText(text)

    def restart_ramp(self):
        """The ramp starts over from its first block, as if the warmup had just ended (on the next tick); the best
        60s ridden so far is kept, and the workout clock keeps going"""
        AppState.workoutList = list(self.builtWorkoutList)
        self.activeWorkout = self.effortStartIndex - 1
        self.blockSecondsPassed = self.eventTimerSeconds = 0.0
        self.blockOverage = self.eventOverage = 0.0
        self.blockDuration = FtpTest.RAMP_BLOCK_SECONDS
        AppState.workoutDurationSeconds = self.workoutSecondsPassed + FtpTest.RAMP_BLOCK_SECONDS
        self.timeProgressBar.setRange(0,int(100*AppState.workoutDurationSeconds))
        #(no 60s spans the pause and the restart)
        self.effortAverage.start_new_window()
        AppState.ftpTestResult = self.ftp_test_result(completed=False)
        #the next tick moves into the first block like the start of a workout (see process_workout_events)
        self.newWorkoutStart = True

    def in_ftp_test_effort(self):
        #from the end of the warmup on: an effort test's free ride, every block of a ramp test
        return self.ftpTest is not None and self.activeWorkout >= self.effortStartIndex

    def ftp_test_result(self, completed):
        """AppState.ftpTestResult: the power the test measured, and whether it counts. A ramp test counts however it
        ended (stopping pedaling, or END) once it has a best 60s"""
        average = self.effortAverage.average()
        return {"average": average, "completed": average is not None if self.rampTest else completed}

    def ftp_test_tick(self):
        #ticks only come while the workout runs, so paused time doesn't count toward the average
        if not self.in_ftp_test_effort():
            return
        power = WorkoutGrading.latest_power(*AppState.savedPowerVals, AppState.secondsPassed)
        self.effortAverage.add(self.timeDelta, power)
        if self.rampTest:
            self.ramp_tick(power)
        AppState.ftpTestResult = self.ftp_test_result(completed=False)

    def ramp_tick(self, power):
        #stopping pedaling for long enough ends the test
        pedaling = power is not None and power >= FtpTest.NOT_PEDALING_WATTS
        self.notPedalingSeconds = 0.0 if pedaling else self.notPedalingSeconds + self.timeDelta
        self.ftpTestMessage = FtpTest.NOT_PEDALING_MESSAGE if self.notPedalingSeconds >= FtpTest.NOT_PEDALING_WARN_SECONDS else ""
        if self.notPedalingSeconds >= FtpTest.NOT_PEDALING_END_SECONDS:
            self.end_ramp()
            return
        #the next block is added RAMP_ADD_BEFORE_SECONDS before the one being ridden ends
        if self.activeWorkout == len(AppState.workoutList) - 1 and \
                self.blockDuration - self.blockSecondsPassed <= FtpTest.RAMP_ADD_BEFORE_SECONDS:
            nextBlock = FtpTest.ramp_block(AppState.ftpTestMode, len(AppState.workoutList) - self.effortStartIndex)
            AppState.workoutList.append(nextBlock)
            AppState.workoutDurationSeconds += nextBlock[1]
            self.timeProgressBar.setRange(0,int(100*AppState.workoutDurationSeconds))
            self.update_upnext_remaining_labels()

    def end_ramp(self):
        """The ramp test is over: the block being ridden ends now (the next tick sees the workout complete) and the
        one queued after it is dropped"""
        del AppState.workoutList[self.activeWorkout+1:]
        self.blockDuration = self.blockSecondsPassed
        AppState.workoutDurationSeconds = self.workoutSecondsPassed
        self.ftpTestMessage = ""

    def show_effort_average(self):
        """The spiral's middle through the effort: its average power so far (for the 20m test, 95% of it: the FTP it
        works out to), or for a ramp test 75% of its best 60s"""
        if not self.in_ftp_test_effort():
            return
        average = self.effortAverage.average()
        value = "--" if average is None else str(FtpTest.ftp_from_average(AppState.ftpTestMode, average))
        self.gradeOverlay.show_value(self.ftpTest["averageTitle"], value)

    def end_grading(self):
        """The workout was ended before it was complete (END, Exit to Synopsis): the block being ridden is graded
        up to now; the blocks never reached aren't graded at all"""
        self.finish_grading(endedEarly=True)

    def show_workout_grades(self, show):
        """The left column: each block's grade once the workout is complete (show), else Up Next/Coming up"""
        self.workoutGraded = show
        for widget in (self.upNextLabelHeader, self.upNextLabelDescription, self.remainingWorkoutHeader,
                       self.remainingWorkoutList):
            widget.setVisible(not show)
        self.workoutGradesHeader.setVisible(show)
        self.workoutGradeList.setVisible(show)
        if show:
            self.workoutGradeList.refresh()

    def update_upnext_remaining_labels(self):
        #collect the workout strings at the current difficulty, skip ramp power updates and interval block updates
        workoutList = []
        for index, workoutElement in enumerate(AppState.workoutList[self.activeWorkout+1:], self.activeWorkout+1):
            #Musical Cadence Mode: a line for each song as it starts
            if self.cadenceMode and index in AppState.musicSongStarts:
                workoutList.append(AppState.musicSongStarts[index])
            if workoutElement[0] in [0,1,2,3]:
                returnedList = ProcessWorkoutLine(workoutElement, self.activeDifficulty)
                if workoutElement[0] in [0,1,2]:
                    workoutList.extend(returnedList)
                elif workoutElement[0] == 3:
                    #interval text handling, keep it as one list item by appending instead of extending
                    workoutList.append(returnedList)
        #a ramp test's next block isn't added until just before this one ends; up next is what it will be
        if not workoutList and self.rampTest and self.notPedalingSeconds < FtpTest.NOT_PEDALING_END_SECONDS:
            nextBlock = FtpTest.ramp_block(AppState.ftpTestMode, len(AppState.workoutList) - self.effortStartIndex)
            workoutList.append(ProcessWorkoutLine(nextBlock, self.activeDifficulty)[0])
        #first check if active workout is out of bounds
        if len(workoutList) <= 0:
            self.upNextLabelDescription.setText("You're on the last block, way to go!")
            self.upNextLabelDescription.setStyleSheet(self.getLabelColor("You're on the last block, way to go!"))
            self.remainingWorkoutHeader.setText("") #the list shows "Almost done!", nothing is coming up
        else:
            #more workout remains, 
            #update up next
            upNextText = ""
            if isinstance(workoutList[0], str) and IsSongHeader(workoutList[0]) and len(workoutList) > 1:
                #a new song next: its first block, with the song under it
                upNextText = workoutList[1] + "\n" + workoutList[0]
                workoutList.pop(0)
            elif isinstance(workoutList[0], list):
                #interval handling
                upNextText = workoutList[0][0] + " " + "\n      ".join(workoutList[0][1:]) #add the remaining interval blocks, newline separated
            else:
                upNextText = workoutList[0]
            self.upNextLabelDescription.setText(upNextText)
            self.upNextLabelDescription.setStyleSheet(self.getLabelColor(upNextText))
            # remove the up next workout from the list
            workoutList.pop(0)
            # update coming up
            comingUpText = ""
            for workoutElement in workoutList:
                if isinstance(workoutElement, list):
                    #interval handling
                    comingUpText = comingUpText + workoutElement[0]  + " " + "\n      ".join(workoutElement[1:]) + "\n" #add the remaining interval blocks, newline separated
                else:
                    comingUpText = comingUpText + workoutElement + "\n"
            #an empty list shows "Almost done!", so there's nothing "Coming up..."
            self.remainingWorkoutHeader.setText("Coming up..." if comingUpText else "")
            self.remainingWorkoutList.ui.widget.setFullText(comingUpText)
        #nothing to do if no workout blocks remain

    #The workout timer blinks through the workout's last BLINK_WINDOW_SECONDS, and the block timer
    #through each block's. The on/off state comes straight from the time remaining (on for the first
    #half period, off for the next, ...), so the rhythm stays even no matter when this is called.
    #update_smooth_bars() calls it every frame, sinceTick being how far past the last 100ms tick it is,
    #so each switch lands on time instead of snapping to the next tick. Blocks/workouts shorter than
    #BLINK_MIN_DURATION_SECONDS don't blink.
    def progress_blink_check(self, sinceTick=0.0):
        woSecondsRemaining = AppState.workoutDurationSeconds - (self.workoutSecondsPassed + sinceTick)
        blSecondsRemaining = self.blockDuration - (self.blockSecondsPassed + sinceTick)
        self.workoutEnding = woSecondsRemaining < self.BLINK_WINDOW_SECONDS
        workoutBlinkOn = AppState.workoutDurationSeconds >= self.BLINK_MIN_DURATION_SECONDS and self.blink_phase_on(woSecondsRemaining)
        blockBlinkOn = self.blockDuration >= self.BLINK_MIN_DURATION_SECONDS and self.blink_phase_on(blSecondsRemaining)
        if workoutBlinkOn != self.blinkWorkout:
            if workoutBlinkOn:
                self.workout_blink_on()
            else:
                self.workout_blink_default()
        if blockBlinkOn != self.blinkBlock:
            if blockBlinkOn:
                self.block_blink_on()
            else:
                self.block_blink_default()
        #one spiral state for both blinks, the workout blink wins while the workout is ending
        #(two separate updates used to fight over it and flicker the spiral)
        if workoutBlinkOn:
            spiralState = "workout"
        elif blockBlinkOn and not self.workoutEnding:
            spiralState = "block"
        else:
            spiralState = "default"
        if spiralState != self.spiralBlinkState:
            self.spiralBlinkState = spiralState
            self.spiralWorkoutProgress.set_blink(spiralState)

    def blink_phase_on(self, secondsRemaining):
        if not 0 < secondsRemaining < self.BLINK_WINDOW_SECONDS:
            return False
        return int(secondsRemaining / self.BLINK_HALF_PERIOD_SECONDS) % 2 == 1

    def workout_blink_default(self):
        self.blinkWorkout = False
        self.set_workout_timer_colors("#1f15e1", "#bdd6de", "#9fc5d1")

    def workout_blink_on(self):
        self.blinkWorkout = True
        self.set_workout_timer_colors("#1f15e1", "#dadbde", "#dadbde")

    #the workout timer labels are rounded, so every recolor has to carry the radius too; the bar stays square
    def set_workout_timer_colors(self, chunkColor, barColor, labelColor):
        self.timeProgressBar.setBarColors(chunkColor, barColor)
        labelStyle = "background-color:%s; border-radius: %dpx;" % (labelColor, self.timerRadius)
        self.workoutTimeHeader.setStyleSheet(labelStyle)
        self.workoutTime.setStyleSheet(labelStyle)
        self.remainingWorkoutTime.setStyleSheet(labelStyle)

    def block_blink_default(self):
        self.blinkBlock = False
        self.currentBlockProgressBar.setBarColors("#1f15e1", "#bdd6de")
        self.timeBlockRemainingValue.setStyleSheet("background-color:#9fc5d1")
        self.blockMultiplierLabel.setStyleSheet("background-color:#9fc5d1")
        self.blockNonIntLabel.setStyleSheet(self.getLabelColor(self.blockNonIntLabel.text()))
        self.blockInt2Label0.setStyleSheet(self.getLabelColor(self.blockInt2Label0.text()))
        self.blockInt2Label1.setStyleSheet(self.getLabelColor(self.blockInt2Label1.text()))
        self.blockInt3Label0.setStyleSheet(self.getLabelColor(self.blockInt3Label0.text()))
        self.blockInt3Label1.setStyleSheet(self.getLabelColor(self.blockInt3Label1.text()))
        self.blockInt3Label2.setStyleSheet(self.getLabelColor(self.blockInt3Label2.text()))
        

    def block_blink_on(self):
        self.blinkBlock = True
        self.currentBlockProgressBar.setBarColors("#1f15e1", "#dadbde")
        self.blockNonIntLabel.setStyleSheet("background-color:#dadbde")
        self.timeBlockRemainingValue.setStyleSheet("background-color:#dadbde")
        if self.currentIntervalBlockIndex == 0:
            self.blockInt2Label0.setStyleSheet("background-color:#dadbde")
            self.blockInt3Label0.setStyleSheet("background-color:#dadbde")
        elif self.currentIntervalBlockIndex == 1:
            self.blockInt2Label1.setStyleSheet("background-color:#dadbde")
            self.blockInt3Label1.setStyleSheet("background-color:#dadbde")
            if len(self.currentEventText) == 4 and self.blockMultiplierLabel.text() == "1x":
                self.blockMultiplierLabel.setStyleSheet("background-color:#dadbde")
        elif self.currentIntervalBlockIndex == 2:
            self.blockInt3Label2.setStyleSheet("background-color:#dadbde")
            if self.blockMultiplierLabel.text() == "1x":
                self.blockMultiplierLabel.setStyleSheet("background-color:#dadbde")

    def hud_graph_data_update(self):
        #collect current workout data 4-12 minutes depending on how much time has already passed
        if AppState.workoutMode:
            self.get_workout_graph_data()
            #adjust workout completed data start point to fit into the window before the breakpoint
            if AppState.secondsPassed > AppState.graphSecondsShown-AppState.graphBreakpoint:
                #find the number of blocks to go back to show as much of the past workout as possible
                while self.pastPowerIndex + 1 < len(AppState.workoutPowerRequestsSeconds):
                    if AppState.secondsPassed - AppState.workoutPowerRequestsSeconds[self.pastPowerIndex+1] > \
                            AppState.graphSecondsShown-AppState.graphBreakpoint:
                        self.pastPowerIndex += 1
                    else:
                        break

        #Keep the x-axis window on one continuous formula for the whole
        #workout instead of only calling setXRange() once secondsPassed
        #crosses the breakpoint. Before this, the range was only set once
        #in HUDWindow.__init__() and otherwise left alone - so the first
        #explicit setXRange() call, right as the data reached the edge of
        #the screen, was a switch from "untouched/autoranged" to
        #"force-set every tick", which is what produced the jarring snap.
        #Calling the same formula (clamped to 0 before the breakpoint) on
        #every tick from the start makes that transition continuous - the
        #legitimate axis changes (new max value, an old max scrolling out
        #of view) are untouched, since those come from the Y-axis logic
        #below and the data itself, not from this X-axis formula.
        diff = max(0, AppState.secondsPassed - (AppState.graphSecondsShown-AppState.graphBreakpoint))
        self.progressGraph.setXRange(diff, diff+AppState.graphSecondsShown)
        self.nowLine.setValue(AppState.secondsPassed)

        #adjust y axes to account for max visible data
        #(the workout's targets are on the cadence axis in Musical Cadence Mode)
        maxTarget = GetMaxOrZero([GetMaxOrZero(self.powerGraphList),
                                  GetMaxOrZero(AppState.workoutPowerRequestsWatts[self.pastPowerIndex:])])
        maxP2 = GetMaxOrZero(AppState.savedPowerVals[1])
        powerMax = GetMaxOrZero([maxP2] if self.cadenceMode else [maxTarget,maxP2])
        if powerMax == 0:
            powerMax = 150
        self.progressGraph.powerVBox.setYRange(0,powerMax+100,padding=0)
        maxSavedHr = GetMaxOrZero(AppState.savedHRVals[1])
        maxCadence = GetMaxOrZero(AppState.savedCadenceVals[1])
        hrCadenceMax = GetMaxOrZero([maxSavedHr, maxCadence] + ([maxTarget] if self.cadenceMode else []))
        if hrCadenceMax == 0:
            hrCadenceMax = 190
        self.progressGraph.hrCadenceVBox.setYRange(0,hrCadenceMax+20,padding=0)
        if AppState.workoutMode:
            #add workout graph data bar graphs, until workout started these will shift to immediately after timePassed, grey nearly transparent
            if not self.workoutUnpausedBool: #pre workout start or while paused
                self.workoutPowerPlot.setData(x=self.timeGraphList,y=self.powerGraphList,ranges=AppState.ftpRanges,colors=AppState.graphWorkoutBrushLight)
            else:
                self.workoutPowerPlot.setData(x=self.timeGraphList,y=self.powerGraphList,ranges=AppState.ftpRanges,colors=AppState.graphWorkoutBrushMid)
            self.workoutPowerCompletedPlot.setData(x=AppState.workoutPowerRequestsSeconds[self.pastPowerIndex:],
                                                    y=AppState.workoutPowerRequestsWatts[self.pastPowerIndex:],
                                                    ranges=AppState.ftpRanges,colors=AppState.graphWorkoutBrushDark)
        #update produced values (power (blue), hr (red), cadence (yellow)) plots based on MainWindow lists;
        #brief sensor dropouts draw straight across, long ones (still ongoing included) show as zero
        series = [("savedPowerVals", self.powerProducedPlot, "power")]
        if AppState.antHRM:
            series.append(("savedHRVals", self.hrPlot, "hr"))
        if AppState.shows_cadence():
            series.append(("savedCadenceVals", self.cadencePlot, "cadence"))
        for savedName, plot, sensor in series:
            self.plot_saved_vals(savedName, plot, FtpColors.SENSOR_COLORS[sensor])
        self.zero_gauges_on_dropout()
        #(the graph's extra views follow its size through sigResized, see __init__)

    def plot_saved_vals(self, savedName, plot, color):
        """One sensor's curve, and its current reading diamond at the newest point. Only redone when there's
        something new to draw: a reading (about 4 a second) or, through a dropout, the zero line growing"""
        times, values = getattr(AppState, savedName)
        dropoutEnd = AppState.secondsPassed if len(times) and \
            AppState.secondsPassed - times[-1] > DROPOUT_ZERO_SECONDS else None
        key = (id(times), len(times), times[0] if len(times) else None, times[-1] if len(times) else None, dropoutEnd)
        if self.plottedKeys.get(savedName) == key:
            return
        self.plottedKeys[savedName] = key
        xs, ys = WithDropoutsAsZero(times, values, endTime=AppState.secondsPassed)
        pen, halo = FtpColors.sensor_pens(color) #(a halo under the line: it shows over any bar or background)
        plot.setData(xs, ys, pen=pen, shadowPen=halo)
        for marker, markerName in self.currentMarkers:
            if markerName == savedName:
                if len(xs):
                    marker.setData(x=[xs[-1]], y=[ys[-1]])
                else:
                    marker.clear()

    def zero_gauges_on_dropout(self):
        #a sensor silent for longer than DROPOUT_ZERO_SECONDS reads 0 instead of freezing on its last value;
        #AntDevicesPoller sets the real value again with the sensor's next reading
        gauges = [(self.wattGaugeDisplay, AppState.savedPowerVals)]
        if AppState.antHRM:
            gauges.append((self.hrGaugeDisplay, AppState.savedHRVals))
        if AppState.shows_cadence():
            gauges.append((self.cadenceGaugeDisplay, AppState.savedCadenceVals))
        for gauge, savedVals in gauges:
            #(the reading in the middle, which shows the 0: the needle stops at the gauge's minimum, e.g. 40 on the HR gauge)
            if len(savedVals[0]) and AppState.secondsPassed - savedVals[0][-1] > DROPOUT_ZERO_SECONDS \
                    and gauge.ui.widget.reading > 0:
                gauge.ui.widget.setValue(0)
        #the speed too: a stopped wheel sends no new revolutions, so the poller never reports the 0 itself
        if self.speedMph > 0 and AppState.secondsPassed - self.speedReadAt > DROPOUT_ZERO_SECONDS:
            self.set_speed("0", reading=False)

    def get_workout_graph_data(self):
        #initialize workout plot coordinates
        self.timeGraphList.clear()
        self.powerGraphList.clear()
        #collect time (x axis) and power (y axis) data from the workout for graphSecondsShown - elapsed time until only the upcoming graphBreakpoint seconds are shown
        collectionPeriod = AppState.graphSecondsShown - AppState.secondsPassed
        if collectionPeriod < AppState.graphBreakpoint:
            collectionPeriod = AppState.graphBreakpoint
        #get the current workout event index, change it to 0 if it is -1 (before start is pressed)
        workoutIndex = self.activeWorkout
        if self.activeWorkout == -1:
            workoutIndex = 0
        firstEvent = True
        for event in AppState.workoutList[workoutIndex:]:
            if event[0] in [0,1,4,5]: #skip interval headers
                if event[0] == 0: #free ride block
                    duration = event[1]
                    power = 0
                elif event[0] in [1,4]: #steady block or ramp details
                    duration = event[1]
                    power = event[2][self.activeDifficulty]
                else: #5, interval internal block
                    if event[3][0] == 0: #free ride interval block
                        duration = event[3][1]
                        power = 0
                    elif event[3][0] == 1: #steady interval block
                        duration = event[3][1]
                        power = event[3][2][self.activeDifficulty]
                    else: #need to skip ramp blocks, their events are already handled
                        continue 
                #modify the duration to account for the time that has already passed within the event block on the current event block
                durationRemaining = duration
                if firstEvent:
                    durationRemaining = duration - self.eventTimerSeconds
                    firstEvent = False
                #check if the remaining event duration exceeds the collection period
                if durationRemaining > collectionPeriod:
                    durationRemaining = collectionPeriod
                #lower the collection period by the duration
                collectionPeriod -= durationRemaining
                #this goes from the active workout so subtract event time passed from duration on initial block
                if len(self.timeGraphList) == 0:
                    #first duration, elapsed time is always the initial starting point
                    self.timeGraphList.append(AppState.secondsPassed)
                    self.timeGraphList.append(AppState.secondsPassed + durationRemaining)
                else:
                    #not the first duration, start at the previous end time
                    self.timeGraphList.append(self.timeGraphList[-1] + durationRemaining)
                #append the power
                self.powerGraphList.append(power)
                #exit the loop if we've filled the collection period
                if collectionPeriod <= 0:
                    break

    def pause_recording(self):
        #only does anything if started flag is set
        if self.startedBool:
            #toggle
            self.workoutUnpausedBool = not self.workoutUnpausedBool
            #set as pressed or not basd on pause state
            if self.workoutUnpausedBool:
                self.pauseButton.setDown(False)
                self.music_pause(False)
                self.updateRequestedPowerDisplays()
                #grading stopped with the workout clock (no ticks while paused); getting back up to the target
                #isn't graded either
                if self.grader is not None:
                    self.grader.resume(self.blockSecondsPassed, self.currentPower)
                #a ramp test's not pedaling time starts over (the rider wasn't pedaling while paused)
                self.notPedalingSeconds = 0.0
                self.ftpTestMessage = ""
                #a ramp paused too long starts over from its first block
                if self.rampPausedAt is not None:
                    expired = time.monotonic() - self.rampPausedAt >= FtpTest.RAMP_PAUSE_SECONDS
                    self.rampPausedAt = None
                    self.pauseButton.setText("PAUSE")
                    if expired:
                        self.restart_ramp()

            else:
                self.pauseButton.setDown(True)
                #pausing in a ramp test's ramp counts down on the pause button (see update_ramp_pause_countdown); the
                #pause_recording() a completed workout makes doesn't
                if self.rampTest and self.in_ftp_test_effort() and not self.workout_complete():
                    self.rampPausedAt = time.monotonic()
                    self.update_ramp_pause_countdown()
                self.rideAtLabel.setText("Paused")
                self.music_pause(True)
                #set the current power arc
                self.targetGauge.ui.widget.arcCenterValue = 0
                self.targetGauge.ui.widget.setEnableDrawNewArc()

    def space_pause(self):
        #(not straight after START: a held or doubled Space that started the workout doesn't pause it too)
        if time.monotonic() - self.startedAt >= SPACE_PAUSE_GUARD_SECONDS:
            self.pauseButton.click()

    def end_available(self):
        #the START/END button reads END: a free ride, or a workout once it's started (finished or not). Its text as
        #set: shown as a symbol (AppLayout.set_symbol) the button itself has none
        return getattr(self.startEndButton, "_themeOrigText", self.startEndButton.text()) == "END"

    #End Workout handler
    def confirm_end_workout(self):
        self.w = ConfirmEndWorkoutWindow(self)
        self.w.show()