import datetime
import html
import os

from PyQt6.QtCore import QEvent, Qt, QTimer
from PyQt6.QtGui import QColor, QKeySequence
from PyQt6.QtWidgets import (
    QFileDialog,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSizePolicy,
    QSpacerItem,
    QVBoxLayout,
    QWidget,
)

from ClassAnalogGauge import *
from ClassLongVerticalLabelSeries import *
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *

from AppState import AppState
import FtpColors
from ClassCalibratePowerWindow import CalibratePowerWindow
from ClassConnectDevicesWindow import ConnectDevicesWindow
from ClassSelectDevicesWindow import SelectDevicesWindow
from ClassMusicSelectWindow import MusicSelectWindow
from ClassWorkoutSelectWindow import WorkoutSelectWindow
from ClassAntDevices import (read_selected_devices, ROLE_POWER, ROLE_HRM, ROLE_SPEED_CADENCE,
                             ROLE_CADENCE, ROLE_SPEED, TARGET_POWER_MAX_WATTS)
from WorkoutHelpers import MinimizedDuration, ProcessWorkoutLine, is_float, ReadSelectedDeviceLines, WriteSelectedDeviceLines, ClearSelectedDeviceLines

import ClassAntLogger
import FtpTest
import KeyShortcuts
import MusicCadence
import SavedSettings
import Units
import ZwoConvert

#lowest Max Watts the FE-C setting accepts (the top of the free mode Watts slider)
MIN_MAX_WATTS = 200
#lowest Power Drive Cutoff the FE-C setting accepts, and its default as a fraction of FTP
MIN_POWER_DRIVE_CUTOFF = 50
POWER_DRIVE_CUTOFF_FTP = 1.2
#longest block a workout file may have (an FTP test's effort can be longer, see build_ftp_test)
MAX_BLOCK_SECONDS = 1800
#a block's power is an FTP multiplier (0.05 to 3), or explicit watts written with a W after them (e.g. 250W)
MIN_FTP_MULTIPLIER, MAX_FTP_MULTIPLIER = 0.05, 3
MIN_EXPLICIT_WATTS, MAX_EXPLICIT_WATTS = 1, 2000
#each difficulty step moves the targets this fraction of FTP (or of an explicit power), -10 to +10 steps
DIFFICULTY_STEP = 0.02



  

#a stylesheet set only when it's a change: every stylesheet goes through the theme and re-polishes the widget (see
#AppTheme.install), and setup_blink() sets the same ones again every 300ms
def set_style(widget, sheet):
    if getattr(widget, "_themeOrigSheet", None) != sheet:
        widget.setStyleSheet(sheet)


class WorkoutSetupWindow(QWidget):
    def __init__(self, parent):
        super(WorkoutSetupWindow, self).__init__()

        self.parent = parent

        #Title
        self.setWindowTitle("Awooo Workout Setup")

        #Fitness Metrics
        self.wheelCSet = False
        self.ftpSet = False
        self.settleCheckPending = False #auto_select_and_connect() has a recheck queued for when the scan settles
        self.maxHrSet = False

        #Track when scanning
        self.scanBool = True

        #Found devices
        self.deviceDetectedRawList = []
        self.deviceSelectedRawList = []
        self.deviceDetectedTextList = []
        self.deviceSelectedTextList = []

        #Blink booleans
        self.powerButtonBlink = False
        self.powerButtonBlinkTgl = False
        self.hrButtonBlink = False
        self.hrButtonBlinkTgl = False
        self.spdCadButtonBlink = False
        self.spdCadButtonBlinkTgl = False
        self.connectButtonBlink = False
        self.connectButtonBlinkTgl = False
        self.wheelCTextBlink = False
        self.wheelCTextBlinkTgl = False
        self.wheelCButtonBlink = False
        self.wheelCButtonBlinkTgl = False
        self.maxHrTextBlink = False
        self.maxHrTextBlinkTgl = False
        self.maxHrButtonBlink = False
        self.maxHrButtonBlinkTgl = False
        self.ftpTextBlink = False
        self.ftpTextBlinkTgl = False
        self.ftpButtonBlink = False
        self.ftpButtonBlinkTgl = False
        self.workoutSelectBlink = False
        self.workoutSelectBlinkTgl = False
        self.startButtonBlink = False
        self.startButtonBlinkTgl = False
        self.disconnectButtonBlink = False
        self.disconnectButtonBlinkTgl = False

        #Text styles (laid out for 1920x1200; other screen sizes are scaled, see AppLayout.py)
        xxlTextStyleSheet = "font: 900 34pt 'Orbitron';"
        xlTextStyleSheet = "font: 900 24pt 'Orbitron';"
        lgTextStyleSheet = "font: 900 18pt 'Orbitron';"
        mdTextStyleSheet = "font: 900 16pt 'Orbitron';"
        smTextStyleSheet = "font: 900 10pt 'Orbitron';"
        xlBtnTextStyleSheet = "font: 24pt 'Segoe UI Black';"
        smBtnTextStyleSheet = "font: 10pt 'Segoe UI Black';"

        #Blink stylesheet text (buttons keep Segoe UI Black; everything else is Orbitron)
        self.btnBlinkOn = "QPushButton {color: black; background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 #1CB5E0, stop: 1 #dadbde); \
                                border-style: solid; border-width: 2px; border-color: white; border-radius: 15px; padding: 4px;" + xlBtnTextStyleSheet + \
                                "}QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #1f15e1);} \
                                QPushButton:disabled {color: grey;background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #4a4952, stop: 1 #716f7d);}"
        self.btnBlinkOff = "QPushButton {color: black; background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 #1CB5E0, stop: 1 #dadbde); \
                                border-style: outset; border-width: 2px; border-color: black; border-radius: 15px; padding: 4px;" + xlBtnTextStyleSheet + \
                                "}QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #1f15e1);} \
                                QPushButton:disabled {color: grey;background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #4a4952, stop: 1 #716f7d);}"
        self.tinyBtnBlinkOn = "QPushButton {color: black; background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 #1CB5E0, stop: 1 #dadbde); \
                                border-style: solid; border-width: 2px; border-color: white; border-radius: 15px; padding: 4px;" + smBtnTextStyleSheet + \
                                "}QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #1f15e1);}"
        self.tinyBtnBlinkOff = "QPushButton {color: black; background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 #1CB5E0, stop: 1 #dadbde); \
                                border-style: outset; border-width: 2px; border-color: black; border-radius: 15px; padding: 4px;" + smBtnTextStyleSheet + \
                                "}QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #1f15e1);}"
        self.txtBlinkOn = "QLineEdit{border-style:solid; border-color:white; border-width: 2px; \
                                background-color: qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #bdd6de stop:1 #9fc5d1);" + \
                                mdTextStyleSheet + "}"
        self.txtBlinkOff = "QLineEdit{border-style:solid; border-color:black; border-width: 2px; \
                                background-color: qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #bdd6de stop:1 #9fc5d1);" + \
                                mdTextStyleSheet + "}"
        
        #longest block allowed in the workout being read
        self.maxBlockSeconds = MAX_BLOCK_SECONDS
        #reading a workout file for the workout picker's preview (see preview_workout), not loading it
        self.previewing = False
        self.previewError = None

        #Clear the workout list
        AppState.workoutList = []
        AppState.workoutFilename = ""
        AppState.workoutFilenameShort = ""
        AppState.workoutTitle = ""
        AppState.workoutMessages = []



        #Widgets
        self.introductoryLayout = QGridLayout()
        self.initializationWidget = QWidget()
        self.initializationWidget.setMaximumWidth(800) #1920x1200
        self.initializationLayout = QGridLayout()
        self.requiredHeader = QLabel("  Required Power Device:  ")
        self.requiredHeader.setMaximumHeight(60)
        self.requiredHeader.setStyleSheet(xlTextStyleSheet)
        self.initializationLayout.addWidget(self.requiredHeader, 0,0,1,2, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.powerButton = QPushButton("Power Device")
        self.powerButton.clicked.connect(self.select_power_device)
        self.powerButton.setStyleSheet(self.btnBlinkOff)
        self.powerButton.setEnabled(False)
        self.initializationLayout.addWidget(self.powerButton, 1,0,1,2)
        self.selectedPowerLabel = QLabel()
        self.selectedPowerLabel.setMaximumHeight(30)
        self.selectedPowerLabel.setMinimumWidth(782)
        self.selectedPowerLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.selectedPowerLabel.setStyleSheet("background-color:#9fc5d1;")
        self.selectedPowerLabel.setSizePolicy(AppState.retainSize)
        self.selectedPowerLabel.hide()
        #an FE-C trainer as the power device gets a Max Watts box beneath it, the top of the free mode Watts slider;
        #in workout mode the same box is the Power Drive Cutoff instead (see update_max_watts_mode)
        self.selectedPowerLayout = QGridLayout()
        self.selectedPowerLayout.addWidget(self.selectedPowerLabel, 0,0,1,2)
        #label and box side by side, centered under the selected power device label
        self.maxWattsLayout = QHBoxLayout()
        self.maxWattsLayout.addStretch()
        self.maxWattsLabel = QLabel("Max Watts:")
        self.maxWattsLabel.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight)
        self.maxWattsLabel.setSizePolicy(AppState.retainSize)
        self.maxWattsLabel.hide()
        self.maxWattsLayout.addWidget(self.maxWattsLabel)
        self.textMaxWatts = QLineEdit(str(AppState.maxWatts))
        self.textMaxWatts.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.textMaxWatts.setStyleSheet(lgTextStyleSheet)
        self.textMaxWatts.setMaxLength(4)
        self.textMaxWatts.setFixedSize(100,40)
        self.textMaxWatts.editingFinished.connect(self.text_max_watts_validator)
        self.textMaxWatts.setSizePolicy(AppState.retainSize)
        self.textMaxWatts.hide()
        self.maxWattsLayout.addWidget(self.textMaxWatts)
        self.maxWattsLayout.addStretch()
        self.selectedPowerLayout.addLayout(self.maxWattsLayout, 1,0,1,2)
        self.initializationLayout.addLayout(self.selectedPowerLayout, 2,0,1,2)
        #"HR" rather than "Heart Rate" so it fits this column on one line
        self.optionalHeader = QLabel("Optional HR/Cadence/Speed Device(s):")
        self.optionalHeader.setMaximumHeight(60)
        self.optionalHeader.setStyleSheet(xlTextStyleSheet)
        self.initializationLayout.addWidget(self.optionalHeader, 3,0,1,2, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.hrmButton = QPushButton("Heart Rate Monitor")
        self.hrmButton.clicked.connect(self.select_hrm_device)
        self.hrmButton.setStyleSheet(self.btnBlinkOff)
        self.hrmButton.setEnabled(False)
        self.initializationLayout.addWidget(self.hrmButton, 4,0)
        self.speedCadenceButton = QPushButton("Speed/Cadence Device")
        self.speedCadenceButton.clicked.connect(self.select_sc_device)
        self.speedCadenceButton.setStyleSheet(self.btnBlinkOff)
        self.speedCadenceButton.setEnabled(False)
        self.initializationLayout.addWidget(self.speedCadenceButton, 4,1)
        self.selectedHrmLabel = QLabel()
        self.selectedHrmLabel.setMinimumSize(386,30)
        self.selectedHrmLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.selectedHrmLabel.setStyleSheet("background-color:#9fc5d1;")
        self.selectedHrmLabel.setSizePolicy(AppState.retainSize)
        self.selectedHrmLabel.hide()
        self.initializationLayout.addWidget(self.selectedHrmLabel, 5,0)
        self.speedCadenceLayout = QGridLayout()
        self.selectedSpeedCadenceLabel = QLabel()
        self.selectedSpeedCadenceLabel.setMinimumSize(386,30)
        self.selectedSpeedCadenceLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.selectedSpeedCadenceLabel.setStyleSheet("background-color:#9fc5d1;")
        self.selectedSpeedCadenceLabel.setSizePolicy(AppState.retainSize)
        self.selectedSpeedCadenceLabel.hide()
        self.speedCadenceLayout.addWidget(self.selectedSpeedCadenceLabel, 0,0,1,2)
        self.selectedCadenceLabel = QLabel()
        self.selectedCadenceLabel.setMinimumSize(190,30)
        self.selectedCadenceLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.selectedCadenceLabel.setStyleSheet("background-color:#9fc5d1;")
        self.selectedCadenceLabel.setSizePolicy(AppState.retainSize)
        self.selectedCadenceLabel.hide()
        self.speedCadenceLayout.addWidget(self.selectedCadenceLabel, 0,0,1,1)
        self.selectedSpeedLabel = QLabel()
        self.selectedSpeedLabel.setMinimumSize(190,30)
        self.selectedSpeedLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.selectedSpeedLabel.setStyleSheet("background-color:#9fc5d1;")
        self.selectedSpeedLabel.setSizePolicy(AppState.retainSize)
        self.selectedSpeedLabel.hide()
        self.speedCadenceLayout.addWidget(self.selectedSpeedLabel, 0,1,1,1)
        self.initializationLayout.addLayout(self.speedCadenceLayout, 5,1)
        self.initStatusDisplay = QLabel("Scanning for ANT+ devices,\nhang on for a bit...")
        ##Longest error message, use for testing
        #self.initStatusDisplay = QLabel("Failed connect to speed/cadence device\nensure that it is active when connecting,\nor select a different speed/cadence device,\nor uncheck speed/cadence devices altogether.")
        self.initStatusDisplay.setStyleSheet(lgTextStyleSheet + "margin-top: 30px")#"background-color:#9fc5d1;")
        self.initStatusDisplay.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter)
        #rich text so low battery warnings can be red beneath the plain status lines
        self.initStatusDisplay.setTextFormat(Qt.TextFormat.RichText)
        self.initStatusLines = ["Scanning for ANT+ devices,", "hang on for a bit..."]
        self.batteryWarnings = []
        self.workoutFileError = None #summary lines of why the last workout file was rejected, see reject_workout()
        self.render_init_status()
        self.initializationLayout.addWidget(self.initStatusDisplay, 6,0,3,2, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.connectButton = QPushButton("Connect Selected Devices")
        self.connectButton.clicked.connect(self.connect_selected_devices)
        self.connectButton.setMaximumWidth(866)
        self.connectButton.setStyleSheet(self.btnBlinkOff)
        self.connectButton.setEnabled(False)
        self.initializationLayout.addWidget(self.connectButton, 11,0,1,2)
        self.restartScanButton = QPushButton("Disconnect and Rescan")
        self.restartScanButton.clicked.connect(self.disconnect_and_rescan)
        self.restartScanButton.setMaximumWidth(430)
        self.restartScanButton.setStyleSheet(self.btnBlinkOff)
        self.restartScanButton.setEnabled(False)
        self.initializationLayout.addWidget(self.restartScanButton, 12,0,1,1)
        self.calibratePowerButton = QPushButton("Calibrate Power")
        self.calibratePowerButton.clicked.connect(self.calibrate_power)
        self.calibratePowerButton.setMaximumWidth(430)
        self.calibratePowerButton.setStyleSheet(self.btnBlinkOff)
        self.calibratePowerButton.setEnabled(False)
        self.initializationLayout.addWidget(self.calibratePowerButton, 12,1,1,1)
        #push the Connect/Rescan/Calibrate buttons to the bottom of this column; the column's
        #height is pinned to the center column's (see eventFilter) so their bottoms line up
        self.initializationLayout.addItem(QSpacerItem(0, 0, QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding), 9,0,2,2)
        self.initializationWidget.setLayout(self.initializationLayout)
        self.introductoryLayout.addWidget(self.initializationWidget, 0,0,14,1, Qt.AlignmentFlag.AlignTop)


        self.centerInitWidget = QWidget()
        self.setFtpMaxHrLayout = QGridLayout()
        #Wheel circumference, FTP, Max HR and (optional) weight: a header over a box, its current value and a Set button,
        #kept short so the mode buttons below fit
        metricHeaderHeight = 34
        metricRowHeight = 40
        self.wheelCHeader = QLabel("Wheel Circumference")
        self.wheelCHeader.setFixedHeight(metricHeaderHeight)
        self.wheelCHeader.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.wheelCHeader.setStyleSheet(mdTextStyleSheet + "background-color:#9fc5d1;")
        self.setFtpMaxHrLayout.addWidget(self.wheelCHeader,0,0,1,3)
        self.textWheelC = QLineEdit("0")
        self.textWheelC.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.textWheelC.setStyleSheet(mdTextStyleSheet)
        self.textWheelC.setMaxLength(5)
        self.textWheelC.setFixedSize(100,metricRowHeight)
        self.textWheelC.editingFinished.connect(self.text_wheelC_validator)
        self.setFtpMaxHrLayout.addWidget(self.textWheelC,1,0, alignment=Qt.AlignmentFlag.AlignCenter)
        self.displayWheelC = QLabel("0m")
        self.displayWheelC.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.displayWheelC.setStyleSheet(mdTextStyleSheet)
        self.displayWheelC.setMaximumSize(180,metricRowHeight)
        self.setFtpMaxHrLayout.addWidget(self.displayWheelC,1,1)
        self.setWheelC = QPushButton("Set Wheel\ncirc. (m)")
        self.setWheelC.clicked.connect(self.set_wheel_c)
        self.setWheelC.setStyleSheet(self.tinyBtnBlinkOff)
        self.setWheelC.setFixedSize(100,metricRowHeight)
        self.setFtpMaxHrLayout.addWidget(self.setWheelC,1,2, alignment=Qt.AlignmentFlag.AlignCenter)

        self.ftpDescriptionHeader = QLabel("FTP")
        self.ftpDescriptionHeader.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.ftpDescriptionHeader.setFixedHeight(metricHeaderHeight)
        self.ftpDescriptionHeader.setStyleSheet(mdTextStyleSheet + "background-color:#9fc5d1;")
        self.setFtpMaxHrLayout.addWidget(self.ftpDescriptionHeader,2,0,1,3)
        self.textFtp = QLineEdit("0")
        self.textFtp.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.textFtp.setStyleSheet(mdTextStyleSheet)
        self.textFtp.setMaxLength(3)
        self.textFtp.setFixedSize(100,metricRowHeight)
        self.textFtp.editingFinished.connect(self.text_ftp_validator)
        self.setFtpMaxHrLayout.addWidget(self.textFtp,3,0, alignment=Qt.AlignmentFlag.AlignCenter)
        self.displayFtp = QLabel("0 Watts")
        self.displayFtp.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.displayFtp.setStyleSheet(mdTextStyleSheet)
        self.displayFtp.setMaximumSize(180,metricRowHeight)
        self.setFtpMaxHrLayout.addWidget(self.displayFtp,3,1)
        self.setFtp = QPushButton("Set\nnew FTP")
        self.setFtp.clicked.connect(self.set_ftp)
        self.setFtp.setStyleSheet(self.tinyBtnBlinkOff)
        self.setFtp.setFixedSize(100,metricRowHeight)
        self.setFtpMaxHrLayout.addWidget(self.setFtp,3,2, alignment=Qt.AlignmentFlag.AlignCenter)

        self.maxHrDescriptionHeader = QLabel("Max HR")
        self.maxHrDescriptionHeader.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.maxHrDescriptionHeader.setFixedHeight(metricHeaderHeight)
        self.maxHrDescriptionHeader.setStyleSheet(mdTextStyleSheet + "background-color:#9fc5d1;")
        self.setFtpMaxHrLayout.addWidget(self.maxHrDescriptionHeader,4,0,1,3)
        self.textMaxHr = QLineEdit("0")
        self.textMaxHr.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.textMaxHr.setStyleSheet(mdTextStyleSheet)
        self.textMaxHr.setMaxLength(3)
        self.textMaxHr.setFixedSize(100,metricRowHeight)
        self.textMaxHr.editingFinished.connect(self.text_max_hr_validator)
        self.setFtpMaxHrLayout.addWidget(self.textMaxHr,5,0, alignment=Qt.AlignmentFlag.AlignCenter)
        self.displayMaxHr = QLabel("0 BPM")
        self.displayMaxHr.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.displayMaxHr.setStyleSheet(mdTextStyleSheet)
        self.displayMaxHr.setMaximumSize(180,metricRowHeight)
        self.setFtpMaxHrLayout.addWidget(self.displayMaxHr,5,1)
        self.setMaxHr = QPushButton("Set new\nMax HR")
        self.setMaxHr.clicked.connect(self.set_max_hr)
        self.setMaxHr.setStyleSheet(self.tinyBtnBlinkOff)
        self.setMaxHr.setFixedSize(100,metricRowHeight)
        self.setFtpMaxHrLayout.addWidget(self.setMaxHr,5,2, alignment=Qt.AlignmentFlag.AlignCenter)

        #optional (0 is fine): with it the synopsis shows power to weight; in lbs or kg (see Units.py, the ⚙ menu's
        #Measurements), converted when that changes
        self.weightHeader = QLabel("Weight (optional)")
        self.weightHeader.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.weightHeader.setFixedHeight(metricHeaderHeight)
        self.weightHeader.setStyleSheet(mdTextStyleSheet + "background-color:#9fc5d1;")
        self.setFtpMaxHrLayout.addWidget(self.weightHeader,6,0,1,3)
        self.textWeight = QLineEdit("0")
        self.textWeight.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.textWeight.setStyleSheet(self.txtBlinkOff)
        self.textWeight.setMaxLength(5)
        self.textWeight.setFixedSize(100,metricRowHeight)
        self.textWeight.editingFinished.connect(self.text_weight_validator)
        self.setFtpMaxHrLayout.addWidget(self.textWeight,7,0, alignment=Qt.AlignmentFlag.AlignCenter)
        self.displayWeight = QLabel("0 lbs")
        self.displayWeight.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.displayWeight.setStyleSheet(mdTextStyleSheet)
        self.displayWeight.setMaximumSize(180,metricRowHeight)
        self.setFtpMaxHrLayout.addWidget(self.displayWeight,7,1)
        self.setWeight = QPushButton("Set new\nWeight")
        self.setWeight.clicked.connect(self.set_weight)
        self.setWeight.setStyleSheet(self.tinyBtnBlinkOff)
        self.setWeight.setFixedSize(100,metricRowHeight)
        self.setFtpMaxHrLayout.addWidget(self.setWeight,7,2, alignment=Qt.AlignmentFlag.AlignCenter)

        #the selected mode is the disabled button (clicking it again does nothing): bright blue, thick black border.
        #the other one stays enabled and looks like a normal raised button that lights up on hover.
        #(the text's navy is its own color, not the window gradient's #28275e, so a theme can recolor them apart)
        modeBtnStyle = """QPushButton {color: #28285e; border-style: outset; border-width: 2px; border-color: #808080; padding: 4px;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #dadbde, stop: 1 #9fc5d1);
                                border-radius: 15px; font: bold 34px 'Segoe UI Black';}
                            QPushButton:hover {border-color: black;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #dadbde, stop: 1 #1CB5E0);}
                            QPushButton:pressed {border-style: inset;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #dadbde);}
                            QPushButton:disabled {color: black; border-style: solid; border-width: 2px; border-color: black;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #dadbde);}"""
        #one button per mode, top to bottom: free mode, the FTP tests (see FtpTest.py), musical cadence mode
        #(see MusicCadence.py), workout mode
        self.freeModeButton = QPushButton("Free Mode")
        self.ftp20mButton = QPushButton("20m FTP Test")
        self.ftp1hButton = QPushButton("1h FTP Test")
        self.ftpRamp10Button = QPushButton("10W delta FTP Ramp")
        self.ftpRamp20Button = QPushButton("20W delta FTP Ramp")
        self.cadenceModeButton = QPushButton("Musical Cadence Mode")
        self.workoutModeButton = QPushButton("Workout Mode")
        self.modeButtons = ((self.freeModeButton, "free"), (self.ftp20mButton, "20m"), (self.ftp1hButton, "1h"),
                            (self.ftpRamp10Button, "ramp10"), (self.ftpRamp20Button, "ramp20"),
                            (self.cadenceModeButton, "cadence"), (self.workoutModeButton, "workout"))
        for row, (button, mode) in enumerate(self.modeButtons, 9):
            button.mode = mode
            button.pressedState = mode == "workout"
            button.setEnabled(not button.pressedState)
            button.setStyleSheet(modeBtnStyle)
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            button.clicked.connect(self.mode_btn_state)
            self.setFtpMaxHrLayout.addWidget(button,row,0,1,3)

        self.workoutFileButton = QPushButton("Select a workout File")
        self.workoutFileButton.setSizePolicy(AppState.retainSize)
        self.workoutFileButton.setMaximumWidth(520)
        self.workoutFileButton.clicked.connect(self.open_workout_dialog)
        self.setFtpMaxHrLayout.addWidget(self.workoutFileButton,16,0,1,3)
        self.workoutFileSelected = QLabel("Workout not set")
        self.workoutFileSelected.setSizePolicy(AppState.retainSize)
        self.workoutFileSelected.setMaximumWidth(520)
        self.workoutFileSelected.setMinimumHeight(60)
        self.workoutFileSelected.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.workoutFileSelected.setWordWrap(True) #workout titles can be long and contain spaces
        self.workoutFileSelected.setStyleSheet(mdTextStyleSheet + "background-color:#9fc5d1;")
        self.setFtpMaxHrLayout.addWidget(self.workoutFileSelected,17,0,1,3)
        self.workoutInfo = QLabel()
        self.workoutInfo.setMaximumWidth(520)
        #(half the height it had before the Musical Cadence Mode button, so the column stays the same height)
        self.workoutInfo.setMinimumHeight(60)
        self.workoutInfo.setSizePolicy(AppState.retainSize)
        self.workoutInfo.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.workoutInfo.setStyleSheet(mdTextStyleSheet + "background-color:#9fc5d1;") # assumes (1920,1200)
        self.setFtpMaxHrLayout.addWidget(self.workoutInfo,18,0,1,3)
        self.centerInitWidget.setLayout(self.setFtpMaxHrLayout)
        self.centerInitWidget.setFixedWidth(522)
        self.introductoryLayout.addWidget(self.centerInitWidget, 0,1,14,1, Qt.AlignmentFlag.AlignTop)
        self.centerInitWidget.installEventFilter(self)

        self.introStartButton = QPushButton("START")
        self.introStartButton.setEnabled(False)
        self.introStartButton.clicked.connect(self.show_hud_window)
        self.introStartButton.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Minimum)
        self.introductoryLayout.addWidget(self.introStartButton, 15,0,1,2)

        self.workoutDisplayLayout = QVBoxLayout()
        # self.workoutDisplayLayout.setContentsMargins(20,0,0,0)
        self.workoutDisplayLabel = LongVLabels(True, AppState.ftp, AppState.ftpRanges)
        self.workoutDisplayLabel.setFixedHeight(900)
        self.workoutDisplayLayout.addWidget(self.workoutDisplayLabel)
        self.introductoryLayout.addLayout(self.workoutDisplayLayout, 0,2,14,1, Qt.AlignmentFlag.AlignHCenter)
        self.workoutDisplayLabel.hide()

        self.setLayout(self.introductoryLayout)

        #keys for the buttons (Ctrl shows them, see KeyShortcuts.py)
        for button, key in ((self.powerButton, Qt.Key.Key_P), (self.hrmButton, Qt.Key.Key_H),
                            (self.speedCadenceButton, Qt.Key.Key_C), (self.connectButton, Qt.Key.Key_N),
                            (self.restartScanButton, Qt.Key.Key_R), (self.calibratePowerButton, Qt.Key.Key_L),
                            (self.setWheelC, Qt.Key.Key_W), (self.setFtp, Qt.Key.Key_F),
                            (self.setMaxHr, Qt.Key.Key_M), (self.setWeight, Qt.Key.Key_G),
                            (self.workoutFileButton, Qt.Key.Key_O), (self.introStartButton, Qt.Key.Key_S)):
            KeyShortcuts.add(button, key, label=QKeySequence(key).toString())
        #the modes, top to bottom
        for number, (button, _) in enumerate(self.modeButtons, 1):
            KeyShortcuts.add(button, Qt.Key(Qt.Key.Key_F1 + number - 1), label=f"F{number}")
        #Tab goes around just the text boxes, and Enter in one presses its Set button (Max Watts has none, Enter or
        #leaving it applies it)
        KeyShortcuts.tab_cycle((self.textMaxWatts, self.textWheelC, self.textFtp, self.textMaxHr, self.textWeight),
                               self.textWheelC)
        for field, button in ((self.textWheelC, self.setWheelC), (self.textFtp, self.setFtp),
                              (self.textMaxHr, self.setMaxHr), (self.textWeight, self.setWeight)):
            KeyShortcuts.add(button, KeyShortcuts.ENTER, field=field)

        # Read saved Wheel Circumference, FTP and Max HR (savedSettingsANT.json), anything missing or out of range
        # has to be entered; each is checked on its own so one bad value doesn't throw away the others
        settings = SavedSettings.load()
        wheelC = settings.get("wheelCircumference")
        if isinstance(wheelC, (int, float)) and .5 <= wheelC < 3:
            AppState.wheelCircumference = str(wheelC)
        ftp = settings.get("ftp")
        if isinstance(ftp, int) and 20 <= ftp < 450:
            AppState.ftp = str(ftp)
        #the power zones, even with no FTP set (a ramp FTP test can be ridden without one)
        self.get_ftp_ranges()
        maxHr = settings.get("maxHr")
        if isinstance(maxHr, int) and 80 <= maxHr < 250:
            AppState.maxHr = str(maxHr)
        maxWatts = settings.get("maxWatts")
        if isinstance(maxWatts, int) and MIN_MAX_WATTS <= maxWatts <= TARGET_POWER_MAX_WATTS:
            AppState.maxWatts = maxWatts
        #nothing (valid) saved yet starts at 120% of FTP
        powerDriveCutoff = settings.get("powerDriveCutoff")
        if isinstance(powerDriveCutoff, int) and MIN_POWER_DRIVE_CUTOFF <= powerDriveCutoff <= TARGET_POWER_MAX_WATTS:
            AppState.powerDriveCutoff = powerDriveCutoff
        else:
            self.default_power_drive_cutoff()
        self.update_max_watts_mode()
        self.displayWheelC.setText(AppState.wheelCircumference + "m")
        self.displayFtp.setText(AppState.ftp + " Watts")
        self.displayMaxHr.setText(AppState.maxHr + " BPM")
        #the rider's weight and the units it's shown in (see Units.py); shown again whenever they change
        Units.load()
        self.show_weight()
        Units.watch(self, "show_weight")

        # This screen used to find out about every new device, a
        # completed connect, or a disconnect purely by re-reading
        # foundDevicesANT.txt/connectANT.txt/readyANT.txt on this timer's
        # fixed 300ms tick. AntWorkoutBridge (self.parent.startExe) now
        # emits a signal the instant each of those actually happens, so
        # connecting the same init_ant_scan_connect() handler to those
        # signals makes the screen react immediately instead of up to
        # 300ms late. The timer itself stays running, just slowed way
        # down (2s) - a deliberate defensive fallback in case a signal is
        # ever missed (e.g. connected mid-emission during construction),
        # not the primary trigger any more.
        bridge = self.parent.startExe
        bridge.deviceDiscovered.connect(lambda deviceType, deviceNumber: self.init_ant_scan_connect())
        bridge.devicesConnected.connect(self.init_ant_scan_connect)
        bridge.devicesDisconnected.connect(self.init_ant_scan_connect)
        bridge.statusMessage.connect(lambda message: self.init_ant_scan_connect())
        bridge.errorOccurred.connect(lambda message: self.init_ant_scan_connect())
        bridge.batteryChanged.connect(self.update_battery_warnings)
        self.update_battery_warnings()

        # Set by stop() while the HUD/synopsis is up, cleared by resume() on the way back
        self.stopReadingInit = False

        # Fallback refresh of the setup screen state
        self.timer = QTimer()
        self.timer.timeout.connect(self.init_ant_scan_connect)
        self.timer.setInterval(2000)
        self.timer.start()

        # setup_blink() just toggles each *BlinkTgl flag and re-applies a
        # stylesheet - cheap, and it's what actually makes a flagged
        # button/text field visibly blink rather than just changing
        # color once. It used to piggyback on the old 300ms
        # init_ant_scan_connect() tick for its cadence; now that that
        # timer only fires every 2s (init_ant_scan_connect's real work is
        # event-driven - see above), blinking needs its own fast,
        # dedicated timer so buttons/fields still blink at roughly their
        # original rate instead of stuttering once every 2 seconds.
        self.blinkTimer = QTimer()
        self.blinkTimer.timeout.connect(self.setup_blink)
        self.blinkTimer.setInterval(300)
        self.blinkTimer.start()

        

    #------------------------------
    #Workout Setup Window Functions
    #------------------------------
    def init_ant_scan_connect(self):
        #The bridge's signals (status messages, connects, discoveries) keep
        #firing during a workout; the setup screen must not react to them -
        #in particular, the AppState.ant* device flags the HUD was built
        #from have to stay put until the workout is over
        if self.stopReadingInit:
            return
        #Blink setting booleans
        self.powerButtonBlink = False
        self.hrButtonBlink = False
        self.spdCadButtonBlink = False
        self.connectButtonBlink = False
        self.wheelCTextBlink = False
        self.wheelCButtonBlink = False
        self.maxHrTextBlink = False
        self.maxHrButtonBlink = False
        self.ftpTextBlink = False
        self.ftpButtonBlink = False
        self.workoutSelectBlink = False
        self.startButtonBlink = False
        self.disconnectButtonBlink = False
        #Strings for device labels
        self.statusString = ""
        #Booleans for what devices are found and selected
        self.pwrSelected = False
        self.hrmSelected = False
        self.spdCadSelected = False
        self.spdSelected = False
        self.cadSelected = False
        self.pwrFound = False
        self.hrmFound = False
        self.spdCadFound = False
        self.spdFound = False
        self.cadFound = False
        #Get the bridge's current state
        bridge = self.parent.startExe
        error = bridge.take_error()
        if error:
            self.set_init_status(error.strip())
        if bridge.is_connect_requested():
            self.set_init_status("Connecting....")
            self.powerButton.setEnabled(False)
            self.hrmButton.setEnabled(False)
            self.speedCadenceButton.setEnabled(False)
            self.connectButton.setEnabled(False)
            self.scanBool = False
        self.get_found_devices()
            
        #handle initial selected devices, changes will be handled in check_for_selected_scanned
        self.get_selected_devices()

        #Auto-select the unambiguous case (exactly one candidate found for
        #a slot) and, once every slot the workout needs is resolved, go
        #straight to connecting - see auto_select_and_connect() for the
        #reasoning. This refreshes self.*Selected/deviceSelectedRawList
        #in place when it acts, so everything below sees the result
        #immediately rather than a tick later.
        self.auto_select_and_connect()

        #hide labels for devices that have been deselected + blink handling
        if not self.hrmSelected:
            self.selectedHrmLabel.setText("")
            self.selectedHrmLabel.hide()
            if not self.hrmFound:
                self.hrmButton.setEnabled(False)
        if not self.pwrSelected:
            self.selectedPowerLabel.setText("")
            self.selectedPowerLabel.hide()
            self.show_max_watts(False)
            if not self.pwrFound:
                self.powerButton.setEnabled(False)
        if not self.spdCadSelected:
            self.selectedSpeedCadenceLabel.setText("")
            self.selectedSpeedCadenceLabel.hide()
        if not self.cadSelected:
            self.selectedCadenceLabel.setText("")
            self.selectedCadenceLabel.hide()
        if not self.spdSelected:
            self.selectedSpeedLabel.setText("")
            self.selectedSpeedLabel.hide()

        if not self.spdCadSelected and not self.spdSelected and not self.cadSelected and \
                not self.spdCadFound and not self.spdFound and not self.cadFound:
            self.speedCadenceButton.setEnabled(False)

        #set up the status text, and check for button blink
        selectDetectMismatch = False
        selectedDevStrList = []
        unselectedScannedDevStrList = []
        #check if devices are selected and found
        for sDev in self.deviceSelectedTextList:
            if sDev in self.deviceDetectedTextList:
                selectedDevStrList.append(sDev +" - Selected/Found")
            else:
                selectedDevStrList.append(sDev +" - Selected")
                selectDetectMismatch = True
        #check if found devices can still be selected
        for fDev in self.deviceDetectedTextList:
            #only add to the list if the device wasn't in the selected list
            if fDev not in self.deviceSelectedTextList:
                #only add to the list if the device type isn't already filled by another selected device
                typeId = fDev.split()
                if typeId[0] in ["Power", "Stationary"] and not self.pwrSelected:
                    self.powerButtonBlink = True
                    unselectedScannedDevStrList.append(fDev)
                elif typeId[0] == "Heart" and not self.hrmSelected:
                    self.hrButtonBlink = True
                    unselectedScannedDevStrList.append(fDev)
                elif typeId[0] == "Speed/Cadence" and not (self.spdCadSelected or self.cadSelected or self.spdSelected):
                    self.spdCadButtonBlink = True
                    unselectedScannedDevStrList.append(fDev)
                elif typeId[0] == "Cadence" and not (self.spdCadSelected or self.cadSelected):
                    self.spdCadButtonBlink = True
                    unselectedScannedDevStrList.append(fDev)
                elif typeId[0] == "Speed" and not (self.spdCadSelected or self.spdSelected):
                    self.spdCadButtonBlink = True
                    unselectedScannedDevStrList.append(fDev)

        #only display the status text if we're still scanning
        if self.scanBool:
            #prepare the info string
            if selectDetectMismatch:
                self.statusString += "Not all selected devices have been detected yet."
            if len(selectedDevStrList) > 1:
                self.statusString += "\n"
                self.statusString += "\n".join(selectedDevStrList)
            elif len(selectedDevStrList) == 1:
                self.statusString += "\n"
                self.statusString += selectedDevStrList[0]
            if len(unselectedScannedDevStrList) > 1:
                self.statusString += "\n\nUnselected Scanned Devices:\n"
                self.statusString += "\n".join(unselectedScannedDevStrList)
            elif len(unselectedScannedDevStrList) == 1:
                self.statusString += "\n\nUnselected Scanned Device:\n"
                self.statusString += unselectedScannedDevStrList[0]
            #end set up status text
            #no usable stick yet: say so (and that plugging one in is enough) rather than claim to be scanning
            waitingForStick = bridge.waiting_for_stick()
            if waitingForStick:
                self.set_init_status(waitingForStick)
            else:
                self.set_init_status(self.statusString.strip() or "Scanning for ANT+ devices,\nhang on for a bit...")
            #check if the connect button is ready to be pressed, blinked
            self.check_for_selected_scanned()

        if bridge.is_ready():
            self.set_init_status("Connected Succesfully")
            #one last check for which devices are connecting
            self.get_found_devices()
            self.get_selected_devices()
            #set booleans for which devices are connected - rebuilt from the
            #bridge's actual connection every time, so a previous workout's
            #devices (the flags used to only ever be set, never cleared,
            #outside Disconnect and Rescan) can't leak into this one
            connectedRoles = bridge.selected_roles()
            AppState.antHRM = ROLE_HRM in connectedRoles
            AppState.antSpeedCadence = ROLE_SPEED_CADENCE in connectedRoles
            AppState.antCadence = ROLE_CADENCE in connectedRoles
            AppState.antSpeed = ROLE_SPEED in connectedRoles
            #calibration needs a connected power device to talk to
        self.calibratePowerButton.setEnabled(
            bridge.is_ready() and bridge.selected_device(ROLE_POWER) is not None)
        if bridge.is_ready() and bridge.is_connect_requested():
            self.restartScanButton.setEnabled(True)
            if self.hrButtonBlink or self.spdCadButtonBlink:
                self.disconnectButtonBlink = True  
        if self.displayWheelC.text() == "0m":
            self.wheelCTextBlink = True
            self.wheelCButtonBlink = True
        if self.displayMaxHr.text() == "0 BPM":
            self.maxHrTextBlink = True
            self.maxHrButtonBlink = True
        if self.displayFtp.text() == "0 Watts" and not self.ftp_optional():
            self.ftpTextBlink = True
            self.ftpButtonBlink = True
        if self.workout_needs_choosing():
            self.workoutSelectBlink = True
        
        self.check_enable_start_button()
        self.setup_blink()

    def setup_blink(self):
        #power button
        self.powerButtonBlinkTgl = not self.powerButtonBlinkTgl
        if self.powerButtonBlink and self.powerButtonBlinkTgl: 
            set_style(self.powerButton, self.btnBlinkOn)
        else: #default setting
            set_style(self.powerButton, self.btnBlinkOff)
        #hr button
        self.hrButtonBlinkTgl = not self.hrButtonBlinkTgl
        if self.hrButtonBlink and self.hrButtonBlinkTgl: 
            set_style(self.hrmButton, self.btnBlinkOn)
        else: #default setting
            set_style(self.hrmButton, self.btnBlinkOff)
        #speed cadence button
        self.spdCadButtonBlinkTgl = not self.spdCadButtonBlinkTgl
        if self.spdCadButtonBlink and self.spdCadButtonBlinkTgl: 
            set_style(self.speedCadenceButton, self.btnBlinkOn)
        else: #default setting
            set_style(self.speedCadenceButton, self.btnBlinkOff)
        #connect button
        self.connectButtonBlinkTgl = not self.connectButtonBlinkTgl
        if self.connectButtonBlink and self.connectButtonBlinkTgl: 
            set_style(self.connectButton, self.btnBlinkOn)
        else: #default setting
            set_style(self.connectButton, self.btnBlinkOff)
        #wheel circumference text
        self.wheelCTextBlinkTgl = not self.wheelCTextBlinkTgl
        if self.wheelCTextBlink and self.wheelCTextBlinkTgl: 
            set_style(self.textWheelC, self.txtBlinkOn)
        else: #default setting
            set_style(self.textWheelC, self.txtBlinkOff)
        #wheel circumference button
        self.wheelCButtonBlinkTgl = not self.wheelCButtonBlinkTgl
        if self.wheelCButtonBlink and self.wheelCButtonBlinkTgl: 
            set_style(self.setWheelC, self.tinyBtnBlinkOn)
        else: #default setting
            set_style(self.setWheelC, self.tinyBtnBlinkOff)
        #max hr text
        self.maxHrTextBlinkTgl = not self.maxHrTextBlinkTgl
        if self.maxHrTextBlink and self.maxHrTextBlinkTgl: 
            set_style(self.textMaxHr, self.txtBlinkOn)
        else: #default setting
            set_style(self.textMaxHr, self.txtBlinkOff)
        #max hr button
        self.maxHrButtonBlinkTgl = not self.maxHrButtonBlinkTgl
        if self.maxHrButtonBlink and self.maxHrButtonBlinkTgl: 
            set_style(self.setMaxHr, self.tinyBtnBlinkOn)
        else: #default setting
            set_style(self.setMaxHr, self.tinyBtnBlinkOff)
        #ftp text
        self.ftpTextBlinkTgl = not self.ftpTextBlinkTgl
        if self.ftpTextBlink and self.ftpTextBlinkTgl: 
            set_style(self.textFtp, self.txtBlinkOn)
        else: #default setting
            set_style(self.textFtp, self.txtBlinkOff)
        #ftp button
        self.ftpButtonBlinkTgl = not self.ftpButtonBlinkTgl
        if self.ftpButtonBlink and self.ftpButtonBlinkTgl: 
            set_style(self.setFtp, self.tinyBtnBlinkOn)
        else: #default setting
            set_style(self.setFtp, self.tinyBtnBlinkOff)
        #workout select button
        self.workoutSelectBlinkTgl = not self.workoutSelectBlinkTgl
        if self.workoutSelectBlink and self.workoutSelectBlinkTgl: 
            set_style(self.workoutFileButton, self.btnBlinkOn)
        else: #default setting
            set_style(self.workoutFileButton, self.btnBlinkOff)
        #start button
        self.startButtonBlinkTgl = not self.startButtonBlinkTgl
        if self.startButtonBlink and self.startButtonBlinkTgl: 
            set_style(self.introStartButton, self.btnBlinkOn)
        else: #default setting
            set_style(self.introStartButton, self.btnBlinkOff)
        #disconnect and restart scan button
        self.disconnectButtonBlinkTgl = not self.disconnectButtonBlinkTgl
        if self.disconnectButtonBlink and self.disconnectButtonBlinkTgl: 
            set_style(self.restartScanButton, self.btnBlinkOn)
        else: #default setting
            set_style(self.restartScanButton, self.btnBlinkOff)

    def get_found_devices(self):
        self.deviceDetectedRawList.clear()
        self.deviceDetectedTextList.clear()
        self.deviceDetectedRawList.extend(self.parent.startExe.found_devices())
        self.deviceDetectedTextList = self.make_text_devices(self.deviceDetectedRawList, False)

    def get_selected_devices(self):
        self.deviceSelectedRawList.clear()
        self.deviceSelectedTextList.clear()
        self.deviceSelectedRawList.extend(ReadSelectedDeviceLines())
        self.deviceSelectedTextList = self.make_text_devices(self.deviceSelectedRawList, True)

    def make_text_devices(self, deviceList, selectedListBool):
        deviceTextList = []
        for deviceLine in deviceList:
            devTypeId = deviceLine.split()
            if len(devTypeId) == 2:
                match devTypeId[0]:
                    case "120": # ANT+ HRM
                        txt = "Heart Rate Monitor: " + devTypeId[1]
                        deviceTextList.append(txt)
                        if selectedListBool:
                            self.hrmSelected = True
                            self.selectedHrmLabel.setText(txt)
                            self.selectedHrmLabel.show()
                            if self.scanBool:
                                self.hrmButton.setEnabled(True)
                        else: 
                            self.hrmFound = True
                            if self.scanBool:
                                self.hrmButton.setEnabled(True)
                    case "11": # ANT+ Power Meter
                        txt = "Power Meter: " + devTypeId[1]
                        deviceTextList.append(txt)
                        if selectedListBool:
                            self.pwrSelected = True
                            self.selectedPowerLabel.setText(txt)
                            self.selectedPowerLabel.show()
                            self.show_max_watts(False)
                            if self.scanBool:
                                self.powerButton.setEnabled(True)
                        else:
                            self.pwrFound = True
                            if self.scanBool:
                                self.powerButton.setEnabled(True)
                    case "17": # ANT+ Fitness Equipment
                        txt = "Stationary Trainer: " + devTypeId[1]
                        deviceTextList.append(txt)
                        if selectedListBool:
                            self.pwrSelected = True
                            self.selectedPowerLabel.setText(txt)
                            self.selectedPowerLabel.show()
                            self.show_max_watts(True)
                            if self.scanBool:
                                self.powerButton.setEnabled(True)
                        else:
                            self.pwrFound = True
                            if self.scanBool:
                                self.powerButton.setEnabled(True)
                    case "121": # ANT+ Speed/Cadence
                        txt = "Speed/Cadence Meter: " + devTypeId[1]
                        deviceTextList.append(txt)
                        if selectedListBool: 
                            self.selectedSpeedCadenceLabel.setText(txt)
                            self.selectedSpeedCadenceLabel.show()
                            self.spdCadSelected = True
                            if self.scanBool:
                                self.speedCadenceButton.setEnabled(True)
                        else: 
                            self.spdCadFound = True
                            if self.scanBool:
                                self.speedCadenceButton.setEnabled(True)
                    case "122": # ANT+ Cadence
                        txt = "Cadence Meter: " + devTypeId[1]
                        deviceTextList.append(txt)
                        if selectedListBool: 
                            self.selectedCadenceLabel.setText(txt)
                            self.selectedCadenceLabel.show()
                            self.cadSelected = True
                            if self.scanBool:
                                self.speedCadenceButton.setEnabled(True)
                        else: 
                            self.cadFound = True
                            if self.scanBool:
                                self.speedCadenceButton.setEnabled(True)
                    case "123": # ANT+ Speed"
                        txt = "Speed Meter: " + devTypeId[1]
                        deviceTextList.append(txt)
                        if selectedListBool: 
                            self.selectedSpeedLabel.setText(txt)
                            self.selectedSpeedLabel.show()
                            self.spdSelected = True
                            if self.scanBool:
                                self.speedCadenceButton.setEnabled(True)
                        else: 
                            self.spdFound = True
                            if self.scanBool:
                                self.speedCadenceButton.setEnabled(True)
        return deviceTextList

    def auto_select_and_connect(self):
        #A workout only ever needs one HRM, one power source (a power
        #meter or a fitness equipment/FE-C trainer share the same slot -
        #see select_power_device()) and either one combined speed/cadence
        #sensor or a discrete cadence+speed pair. Once scanning turns up
        #exactly one candidate for a slot, there's nothing to decide, so
        #auto-select it rather than making the person click through the
        #picker for the common case. Genuine ambiguity (more than one
        #candidate for a slot) is deliberately left alone here - the
        #found-but-unselected blink logic further down in
        #init_ant_scan_connect() picks that up, and the person resolves it
        #manually via select_power_device()/select_hrm_device()/
        #select_sc_device() (whose own SelectDevicesWindow now blinks the
        #genuine candidates too).
        if not self.scanBool:
            return

        #Give the scan time to hear every device in range first, so a slot with two candidates is left
        #for the person to choose instead of going to whichever was heard first; check again once it has
        settleRemaining = self.parent.startExe.scan_settle_remaining()
        if settleRemaining > 0:
            if not self.settleCheckPending:
                self.settleCheckPending = True
                QTimer.singleShot(int(settleRemaining*1000) + 50, self.settle_check)
            return

        powerCandidates =[d for d in self.deviceDetectedRawList if d.split()[0] in ("11", "17")]
        hrmCandidates = [d for d in self.deviceDetectedRawList if d.split()[0] == "120"]
        combinedCandidates = [d for d in self.deviceDetectedRawList if d.split()[0] == "121"]
        cadenceCandidates = [d for d in self.deviceDetectedRawList if d.split()[0] == "122"]
        speedCandidates = [d for d in self.deviceDetectedRawList if d.split()[0] == "123"]

        toSelect = []

        if not self.pwrSelected and len(powerCandidates) == 1:
            toSelect.append(powerCandidates[0])

        if not self.hrmSelected and len(hrmCandidates) == 1:
            toSelect.append(hrmCandidates[0])

        if not (self.spdCadSelected or self.cadSelected or self.spdSelected):
            if len(combinedCandidates) == 1 and not cadenceCandidates and not speedCandidates:
                toSelect.append(combinedCandidates[0])
            elif len(cadenceCandidates) == 1 and len(speedCandidates) == 1 and not combinedCandidates:
                toSelect.append(cadenceCandidates[0])
                toSelect.append(speedCandidates[0])

        if toSelect:
            #rewritten whole rather than appended, see WriteSelectedDeviceLines()
            WriteSelectedDeviceLines(ReadSelectedDeviceLines() + toSelect)
            for line in toSelect:
                ClassAntLogger.log_event("SETUP", f"Auto-selected device: {line}")
            #Refresh self.*Selected/deviceSelectedRawList/labels in place
            #so the rest of this same tick sees the new selection instead
            #of waiting another 300ms timer cycle.
            self.get_selected_devices()

        #Auto-connect: once every slot the workout needs resolves to
        #exactly one selected device that's actually been detected -
        #whether that selection just happened above or the person made it
        #manually through the picker - go straight to connecting rather
        #than waiting for the "Connect to Selected Devices" click and its
        #confirmation popup. disconnect_and_rescan() (the "Disconnect and
        #Rescan" button) is the escape hatch: it clears the selection file
        #so the person can fall back to picking devices by hand without
        #this immediately re-selecting/re-connecting them.
        if self.parent.startExe.is_connect_requested():
            return

        spdCadResolved = self.spdCadSelected or (self.cadSelected and self.spdSelected)
        if not (self.pwrSelected and self.hrmSelected and spdCadResolved):
            return

        for sDevLine in self.deviceSelectedRawList:
            if sDevLine not in self.deviceDetectedRawList:
                return  #a selected device hasn't actually shown up yet

        ClassAntLogger.log_event(
            "SETUP",
            "Auto-connecting - all required slots resolved: "
            + ", ".join(self.deviceSelectedRawList),
        )
        self.parent.startExe.request_connect(read_selected_devices())

    def settle_check(self):
        self.settleCheckPending = False
        self.init_ant_scan_connect()

    #Check if connect button should be available
    def check_for_selected_scanned(self):
        self.get_found_devices()
        self.get_selected_devices()
        foundPowerDevice = False
        for sDevLine in self.deviceSelectedRawList:
            #Only a power device is needed to connect
            if sDevLine.split()[0] in ["11","17"]:
                #Check if the device was scanned
                if sDevLine in self.deviceDetectedRawList:
                    foundPowerDevice = True
        if foundPowerDevice:
            self.connectButton.setEnabled(True)
            #also blink connect button if all other device selection buttons aren't blinking
            if not self.powerButtonBlink and not self.hrButtonBlink and not self.spdCadButtonBlink:
                self.connectButtonBlink = True
        else:
            self.connectButton.setEnabled(False)
            
    def stop(self):
        self.stopReadingInit = True
        self.timer.stop()
        self.blinkTimer.stop()

    def resume(self):
        self.stopReadingInit = False
        self.timer.start()
        self.blinkTimer.start()

    def set_init_status(self, text):
        #"ANT Initialized" sits above whatever the connection status is once the stick is up
        lines = []
        if self.parent.startExe.is_initialized():
            lines.append("ANT Initialized")
        if text:
            lines.extend(text.split("\n"))
        self.initStatusLines = lines
        self.render_init_status()

    def update_battery_warnings(self):
        self.batteryWarnings = self.parent.startExe.low_battery_warnings()
        self.render_init_status()

    def render_init_status(self):
        lines = [html.escape(line) for line in self.initStatusLines]
        #unset settings, a rejected workout file, then low batteries, below the ANT status, all in red
        settingsWarnings = []
        if not self.wheelCSet:
            settingsWarnings.append("Wheel Circ. must be set!")
        if not self.ftpSet and not self.ftp_optional():
            settingsWarnings.append("FTP must be set!")
        if not self.maxHrSet:
            settingsWarnings.append("Max HR must be set!")
        warnings = settingsWarnings + (self.workoutFileError or []) + self.batteryWarnings
        lines += ['<span style="color:' + AppState.gaugeRed + ';">' + html.escape(warning) + '</span>'
                  for warning in warnings]
        self.initStatusDisplay.setText("<br>".join(lines))

    def reject_workout(self, filename, reason, lnNum=None, line=None):
        #full details (where, and the offending line) in the workout info box, plus a two line summary
        #under the ANT status: <file>, then Error, line <#>: "<reason>"
        if self.previewing:
            #(the workout picker's preview, see preview_workout: just why, for its own display)
            self.previewError = f"Line {lnNum}: {reason}" if lnNum is not None else reason
            return
        location = f"file;line\n{filename};{lnNum}" if lnNum is not None else f"file\n{filename}"
        details = f"Invalid workout: {location}\n{reason}"
        if line is not None:
            details += f"\n{line}"
        self.workoutInfo.setText(details)
        summary = " ".join(reason.split())
        where = f"Error, line {lnNum}" if lnNum is not None else "Error"
        self.workoutFileError = [filename, f'{where}: "{summary}"']
        self.render_init_status()

    def calibrate_power(self):
        self.w = CalibratePowerWindow(bridge=self.parent.startExe)
        self.w.show()

    def select_power_device(self):
        deviceDetectedList = []
        for deviceLine in self.deviceDetectedRawList:
            devTypeId = deviceLine.split()
            if devTypeId[0] in ["11", "17"]:
                deviceDetectedList.append(deviceLine)
        for deviceLine in self.deviceSelectedRawList:
            devTypeId = deviceLine.split()
            if devTypeId[0] in ["11", "17"]:
                deviceDetectedList.append(deviceLine)
        self.device_selector_window(deviceDetectedList, "power")
    
    def select_hrm_device(self):
        deviceDetectedList = []
        for deviceLine in self.deviceDetectedRawList:
            devTypeId = deviceLine.split()
            if devTypeId[0] == "120":
                deviceDetectedList.append(deviceLine)
        for deviceLine in self.deviceSelectedRawList:
            devTypeId = deviceLine.split()
            if devTypeId[0] == "120":
                deviceDetectedList.append(deviceLine)
        self.device_selector_window(deviceDetectedList, "hrm")

    def select_sc_device(self):
        deviceDetectedList = []
        for deviceLine in self.deviceDetectedRawList:
            devTypeId = deviceLine.split()
            if devTypeId[0] in ["121", "122", "123"]:
                deviceDetectedList.append(deviceLine)
        for deviceLine in self.deviceSelectedRawList:
            devTypeId = deviceLine.split()
            if devTypeId[0] in ["121", "122", "123"]:
                deviceDetectedList.append(deviceLine)
        self.device_selector_window(deviceDetectedList, "sc")

    def connect_selected_devices(self):
        self.w = ConnectDevicesWindow(bridge=self.parent.startExe)
        self.w.move(20,20)
        self.w.show()


    def disconnect_and_rescan(self):
        ClassAntLogger.log_event("SETUP", "User requested Disconnect and Rescan")
        AppState.antHRM = False
        AppState.antSpeedCadence = False
        AppState.antCadence = False
        AppState.antSpeed = False
        self.connectButton.setEnabled(False)
        self.restartScanButton.setEnabled(False)
        self.calibratePowerButton.setEnabled(False)
        self.parent.startExe.request_disconnect()
        #Clear any automatic (or prior manual) selections too, so the next
        #scan starts from a clean slate and auto_select_and_connect() -
        #and, if the person prefers, their own manual picks - can build a
        #fresh selection rather than instantly reconnecting to what was
        #just disconnected.
        ClearSelectedDeviceLines()
        self.scanBool = True

    def device_selector_window(self,devList,devTypeStr):
        self.w = SelectDevicesWindow(parent=None,devList=devList,devListType=devTypeStr,
                                     foundDevices=self.parent.startExe.found_devices())
        self.w.show()

    #function to get ftp ranges
    def get_ftp_ranges(self):
        #ftpRanges is a MainWindow class attribute (shared across calls,
        #not reset per-instance), so without clearing it first, changing
        #the FTP value and calling this again just appends 4 more entries
        #on top of the old ones - e.g. change FTP once before starting a
        #workout and this list has 8 entries instead of 4, which is what
        #produced "Ranges count must be one less than colors count--got 8
        #and 5" (4 stale + 4 fresh ranges against the fixed 5-color list).
        #(with no FTP set, the zones are worked out from FtpColors.UNSET_FTP_WATTS)
        AppState.ftpRanges[:] = FtpColors.power_ranges()

        #the graph's bar colors per zone: paused / upcoming / past (see FtpColors.py)
        FtpColors.update_graph_brushes()

    #function to check for valid wheel circumference
    def text_wheelC_validator(self):
        if is_float(self.textWheelC.text()):
            if .5 <= float(self.textWheelC.text())<3:
                return
        self.textWheelC.setText(AppState.wheelCircumference)

    #function to check for valid FTP setting
    def text_ftp_validator(self):
        if self.textFtp.text().isdigit():
            if 20<=int(self.textFtp.text())<450:
                return
        self.textFtp.setText(AppState.ftp)

    #function to check for valid Max HR setting
    def text_max_hr_validator(self):
        if self.textMaxHr.text().isdigit():
            if 80<=int(self.textMaxHr.text())<250:
                return
        self.textMaxHr.setText(AppState.maxHr)

    #function to check for valid weight: 0 (not set) or a reasonable weight in the weight unit
    def text_weight_validator(self):
        if is_float(self.textWeight.text()):
            weight = float(self.textWeight.text())
            low, high = Units.weight_range()
            if weight == 0 or low <= weight <= high:
                return
        self.textWeight.setText(Units.format_weight(Units.current.riderWeightKg))

    #FE-C Max Watts (free mode) or Power Drive Cutoff (workout mode) takes effect as soon as it's entered;
    #anything invalid goes back to the last value
    def text_max_watts_validator(self):
        text = self.textMaxWatts.text()
        if AppState.workoutMode:
            if text.isdigit() and MIN_POWER_DRIVE_CUTOFF <= int(text) <= TARGET_POWER_MAX_WATTS:
                AppState.powerDriveCutoff = int(text)
                SavedSettings.update(powerDriveCutoff=AppState.powerDriveCutoff)
        elif text.isdigit() and MIN_MAX_WATTS <= int(text) <= TARGET_POWER_MAX_WATTS:
            AppState.maxWatts = int(text)
            SavedSettings.update(maxWatts=AppState.maxWatts)
        self.update_max_watts_mode()

    #the FE-C box is Max Watts in free mode and Power Drive Cutoff in workout mode
    def update_max_watts_mode(self):
        if AppState.workoutMode:
            self.maxWattsLabel.setText("Power Drive Cutoff:")
            self.textMaxWatts.setText(str(AppState.powerDriveCutoff))
        else:
            self.maxWattsLabel.setText("Max Watts:")
            self.textMaxWatts.setText(str(AppState.maxWatts))

    #Power Drive Cutoff back to 120% of the current FTP (save=True when it's a new FTP, so the saved one follows)
    def default_power_drive_cutoff(self, save=False):
        if AppState.ftp != "0":
            AppState.powerDriveCutoff = min(round(int(AppState.ftp)*POWER_DRIVE_CUTOFF_FTP), TARGET_POWER_MAX_WATTS)
            if save:
                SavedSettings.update(powerDriveCutoff=AppState.powerDriveCutoff)

    #shows the Max Watts box only while the selected power device is an FE-C trainer
    def show_max_watts(self, show):
        self.maxWattsLabel.setVisible(show)
        self.textMaxWatts.setVisible(show)

    #function to set the Wheel Circumference to the selected value
    def set_wheel_c(self):
        #(checked here too: pressing Set, by click or key, leaves the box focused so it hasn't been checked yet)
        self.text_wheelC_validator()
        if self.textWheelC.text() != "0":
            self.displayWheelC.setText(self.textWheelC.text() + "m")
            AppState.wheelCircumference = self.textWheelC.text()
            SavedSettings.update(wheelCircumference=float(AppState.wheelCircumference))
            self.reload_workout()

    #function to set the FTP to the selected value
    def set_ftp(self):
        self.text_ftp_validator()
        if self.textFtp.text() != "0":
            self.displayFtp.setText(self.textFtp.text() + " Watts")
            AppState.ftp = self.textFtp.text()
            self.get_ftp_ranges()
            SavedSettings.update(ftp=int(AppState.ftp))
            self.default_power_drive_cutoff(save=True)
            self.update_max_watts_mode()
            self.reload_workout()

    #function to set the Max HR to the selected value
    def set_max_hr(self):
        self.text_max_hr_validator()
        if self.textMaxHr.text() != "0":
            self.displayMaxHr.setText(self.textMaxHr.text() + " BPM")
            AppState.maxHr = self.textMaxHr.text()
            SavedSettings.update(maxHr=int(AppState.maxHr))
            self.reload_workout()

    #function to set the weight (0 clears it), entered in the weight unit and kept in kg
    def set_weight(self):
        self.text_weight_validator()
        Units.set_rider_weight_kg(Units.weight_to_kg(float(self.textWeight.text())))

    #the weight in the current unit (it's converted when the unit changes)
    def show_weight(self):
        self.displayWeight.setText(Units.weight_text())
        self.textWeight.setText(Units.format_weight(Units.current.riderWeightKg))

    #an FTP test's new FTP, from its synopsis (Apply FTP): set the same as typing it in and pressing Set new FTP
    def apply_ftp(self, ftp):
        self.textFtp.setText(str(ftp))
        self.set_ftp()

    #a workout's max heart rate, from its synopsis (Set Max HR): set the same as typing it in and pressing Set new Max HR
    def apply_max_hr(self, maxHr):
        self.textMaxHr.setText(str(maxHr))
        self.set_max_hr()

    #the loaded workout again after a setting it was built from changed: the FTP test, else the workout file if there is one
    def reload_workout(self):
        if AppState.cadenceMode:
            #the cadence zones again (a new FTP just put the power zones back), nothing else depends on the settings
            self.apply_cadence_ranges()
        elif AppState.ftpTestMode:
            self.build_ftp_test()
        elif AppState.workoutFilename != "":
            self.process_workout_file(AppState.workoutFilename)

    #radio button handling
    def mode_btn_state(self):
        mode = self.sender().mode
        for button, buttonMode in self.modeButtons:
            button.pressedState = buttonMode == mode
            button.setEnabled(not button.pressedState)
        #the FTP tests are workouts too, built here rather than read from a file
        AppState.workoutMode = mode != "free"
        AppState.ftpTestMode = mode if mode in FtpTest.TESTS else None
        wasCadenceMode = AppState.cadenceMode
        AppState.cadenceMode = mode == "cadence"
        #with no FTP set, the zones' colors depend on the mode (FtpColors.zone_ftp); leaving Musical Cadence
        #Mode, its cadence zones go back to power zones
        if AppState.ftp == "0" or wasCadenceMode:
            self.get_ftp_ranges()
        self.update_max_watts_mode()
        self.workoutFileButton.setText("Select Music" if AppState.cadenceMode else "Select a workout File")
        if AppState.cadenceMode:
            self.workoutFileButton.show()
            self.workoutFileSelected.show()
            self.workoutInfo.show()
            self.build_music_workout()
        elif AppState.ftpTestMode:
            self.workoutFileButton.hide()
            self.workoutFileSelected.show()
            self.workoutInfo.show()
            self.build_ftp_test()
        elif AppState.workoutMode:
            self.workoutFileButton.show()
            self.workoutFileSelected.show()
            self.workoutInfo.show()
            #(back from an FTP test, the workout file's workout replaces the test's)
            if AppState.workoutFilename != "" and AppState.ftp != "0":
                self.process_workout_file(AppState.workoutFilename)
                self.workoutDisplayLabel.show()
            else:
                self.clear_workout()
                self.workoutInfo.setText("")
                self.workoutDisplayLabel.ui.widget.setFullText("")
                self.workoutDisplayLabel.hide()
            self.workoutFileSelected.setText(" "+AppState.workout_display_name()+" " if AppState.workoutFilename != ""
                                             else "Workout not set")
        else:
            self.workoutFileButton.hide()
            self.workoutFileSelected.hide()
            self.workoutInfo.hide()
            self.workoutDisplayLabel.hide()
        #Re-evaluate mode dependent UI now, rather than waiting for the next
        #init_ant_scan_connect() tick (up to 2s) to notice the mode changed
        self.workoutSelectBlink = self.workout_needs_choosing()
        self.check_enable_start_button()
        self.setup_blink()

    #a ramp FTP test can be ridden with no FTP set (its warmup is a free ride then, see FtpTest.workout_lines), and
    #so can Musical Cadence Mode (its targets are cadences)
    def ftp_optional(self):
        return (bool(AppState.ftpTestMode) and FtpTest.is_ramp(AppState.ftpTestMode)) or \
            (AppState.workoutMode and AppState.cadenceMode)

    #a workout mode that still needs its workout file (or music) picked
    def workout_needs_choosing(self):
        if not AppState.workoutMode or AppState.ftpTestMode:
            return False
        if AppState.cadenceMode:
            return not AppState.workoutList
        return AppState.workoutFilenameShort == ""

    #the workout to ride is ready: a workout file's, an FTP test's or Musical Cadence Mode's
    def workout_ready(self):
        if AppState.ftpTestMode or AppState.cadenceMode:
            return bool(AppState.workoutList)
        return AppState.workoutFilename != ""

    #the selected FTP test (see FtpTest.py) as the workout: its blocks read like a workout file's, plus its messages
    def build_ftp_test(self):
        test = FtpTest.TESTS[AppState.ftpTestMode]
        if AppState.ftp == "0" and not self.ftp_optional(): #built once the FTP is set (set_ftp)
            self.clear_workout()
            self.workoutInfo.setText("Set your FTP first")
            self.workoutDisplayLabel.ui.widget.setFullText("")
            self.workoutDisplayLabel.hide()
            self.workoutFileSelected.setText(" "+test["name"]+" ")
            return
        #an effort test's effort is a single free ride block, which can be longer than a workout file's blocks
        self.maxBlockSeconds = max(MAX_BLOCK_SECONDS, test.get("effortSeconds", 0))
        try:
            built = self.process_workout_lines(FtpTest.workout_lines(AppState.ftpTestMode, AppState.ftp != "0"), test["name"])
        finally:
            self.maxBlockSeconds = MAX_BLOCK_SECONDS
        if not built:
            return
        if FtpTest.is_ramp(AppState.ftpTestMode):
            #the ramp's first block; the HUD adds the rest as they're ridden
            effortStart = AppState.workoutDurationSeconds
            firstBlock = FtpTest.ramp_block(AppState.ftpTestMode, 0)
            AppState.workoutList.append(firstBlock)
            AppState.workoutDurationSeconds += firstBlock[1]
            self.workoutInfo.setText("Warmup + ramp until you stop pedaling")
            #a "more..." block under the first step: the ramp keeps going until the rider stops
            self.show_workout_text(more=True)
        else:
            effortStart = AppState.workoutDurationSeconds - test["effortSeconds"]
        AppState.workoutMessages.extend(FtpTest.messages(AppState.ftpTestMode, effortStart, AppState.ftp))
        self.workoutFileSelected.setText(" "+test["name"]+" ")
        self.workoutDisplayLabel.show()

    def open_workout_dialog(self):
        if AppState.cadenceMode:
            self.open_music_dialog()
            return
        dialog = WorkoutSelectWindow(self)
        fname = (dialog.selected_path() if dialog.exec() else "",)
        if fname[0] != '':
            if self.process_workout_file(fname[0]):
                self.workoutDisplayLabel.show()
                AppState.workoutFilename = fname[0]
                AppState.workoutFilenameShort = fname[0].split("/")[-1]
                self.workoutFileSelected.setText(" "+AppState.workout_display_name()+" ")
            else:
                AppState.workoutFilename = ""
                AppState.workoutFilenameShort = ""
                AppState.workoutTitle = ""
                AppState.workoutMessages = []
                self.workoutFileSelected.setText("")
                self.workoutDisplayLabel.ui.widget.setFullText("")
            #check enable start button on failure or success
            self.workoutSelectBlink = AppState.workoutFilenameShort == ""
            self.check_enable_start_button()
            self.setup_blink()

    ###Musical Cadence Mode (see MusicCadence.py)
    def open_music_dialog(self):
        dialog = MusicSelectWindow(self)
        if dialog.exec():
            self.musicSelection = dialog.selection()
            self.build_music_workout()
            self.workoutSelectBlink = self.workout_needs_choosing()
            self.check_enable_start_button()
            self.setup_blink()

    def saved_music_selection(self):
        """Last time's checked songs, if every one of them is still analyzed in the cache (the ride can then be
        built right away, without the song picker); None otherwise"""
        cache = MusicCadence.load_cache()
        songs, analyses = [], {}
        try:
            for path, title, checked in MusicCadence.load_selection():
                if not checked:
                    continue
                analysis = MusicCadence.cached(cache, path) if os.path.isfile(path) else None
                if analysis is None:
                    return None
                songs.append((path, title))
                analyses[path] = analysis
        finally:
            #(a song looked up for the first time has its file read for its CRC: kept, so that's only ever once)
            MusicCadence.save_new_crcs(cache)
        if not songs:
            return None
        settings = SavedSettings.load()
        return {"songs": songs, "analyses": analyses,
                "preferred": int(settings.get("musicPreferredRpm", MusicCadence.DEFAULT_PREFERRED_RPM)),
                "minSegment": int(settings.get("musicMinSegment", MusicCadence.DEFAULT_MIN_SEGMENT_SECONDS))}

    def build_music_workout(self):
        """The music's cadence profile as the workout: a steady block per cadence (the RPM where a workout file's
        blocks have watts, the same at every difficulty), free ride where there's no clear beat, and each song's
        title as a timed message as it starts"""
        self.clear_workout()
        AppState.musicTracks = []
        AppState.musicSongStarts = {}
        AppState.musicPulses = []
        if getattr(self, "musicSelection", None) is None:
            self.musicSelection = self.saved_music_selection()
        selection = self.musicSelection
        if selection is None:
            self.workoutFileSelected.setText("Music not picked")
            self.workoutInfo.setText("")
            self.workoutDisplayLabel.ui.widget.setFullText("")
            self.workoutDisplayLabel.hide()
            return
        blocks, tracks, songStarts, pulses = MusicCadence.build_workout(selection["songs"], selection["analyses"],
                                                                        selection["preferred"], selection["minSegment"])
        for duration, rpm in blocks:
            AppState.workoutList.append((0, duration, 0) if rpm == 0 else (1, duration, (rpm,)*21, 0))
        AppState.workoutDurationSeconds = sum(duration for duration, _ in blocks)
        AppState.musicTracks = tracks
        AppState.musicSongStarts = {index: MusicCadence.song_header(tracks[track]) for index, track in songStarts.items()}
        AppState.musicPulses = pulses
        for _, title, start, duration in tracks:
            messageStart = int(start)
            messageEnd = min(messageStart + 8, AppState.workoutDurationSeconds)
            if messageEnd > messageStart:
                AppState.workoutMessages.append((messageStart, messageEnd, "Now playing: " + title))
        songCount = len(tracks)
        #(the title bar shows the song being played after it, see HUDWindow.update_song_title)
        AppState.workoutTitle = "Musical Cadence Mode"
        self.apply_cadence_ranges()
        self.workoutFileSelected.setText(f" Musical Cadence: {songCount} song{'s' if songCount != 1 else ''} ")
        self.workoutInfo.setText("Ride Duration: " + MinimizedDuration(AppState.workoutDurationSeconds))
        self.show_workout_text()
        self.workoutDisplayLabel.show()

    def apply_cadence_ranges(self):
        """The zone boundaries (AppState.ftpRanges, which color the blocks, graph, gauge arc and band): zone "0" up
        to FtpColors.CADENCE_SCALE_BOTTOM_RPM plus a step, the rest a step apart, red from
        FtpColors.CADENCE_SCALE_TOP_RPM (see FtpColors.cadence_ranges)"""
        AppState.ftpRanges[:] = FtpColors.cadence_ranges(FtpColors.CADENCE_SCALE_BOTTOM_RPM)
        FtpColors.update_graph_brushes()

    def clear_workout(self):
        AppState.workoutDurationSeconds = 0
        AppState.workoutList = []
        AppState.workoutTitle = ""
        AppState.workoutMessages = []

    def process_workout_file(self, filePath):
        self.clear_workout()
        ext = filePath.split(".")[-1].lower()
        filename = filePath.split("/")[-1]
        if ext == "awo":
            try:
                with open(filePath, encoding="utf-8-sig") as f:
                    contents = f.read()
            except UnicodeDecodeError:
                self.reject_workout(filename, "File must be saved as UTF-8 text")
                return False
            contentsList = contents.split("\n")
            #remove the last line if it is blank
            if contentsList[-1] == "": 
                contentsList = contentsList[0:-1]
            return self.process_workout_lines(contentsList, filename)
        elif ext == "zwo":
            #a Zwift workout, read as the .awo it converts to (see ZwoConvert.py)
            try:
                contentsList, _, warnings = ZwoConvert.convert(filePath)
            except ZwoConvert.ZwoError as e:
                self.reject_workout(filename, str(e))
                return False
            for warning in warnings:
                ClassAntLogger.log_event("SETUP", f"{filename}: {warning}")
            return self.process_workout_lines(contentsList, filename)
        else:
            self.reject_workout(filename, "File must be of type .awo or .zwo")
            return False

    #a workout's .awo lines (from a file, or an FTP test's, see build_ftp_test) into AppState.workoutList and
    #AppState.workoutMessages; filename is what any error is reported against
    def process_workout_lines(self, contentsList, filename):
        self.clear_workout()
        self.workoutDisplayLabel.ui.widget.ftpRanges = AppState.ftpRanges
        #get the ftp difficulty value list
        ftpIncrement = []
        for i in range(-10,11):
            ftpIncrement.append(int(int(AppState.ftp)*i*.02))
        #workout time at the start of the current block, used to place the timed messages
        blockStartSeconds = 0
        #process the workout contents
        for lnNum, line in enumerate(contentsList):
            lnNum += 1 #increment line number by 1 so first line is 1 instead of 0
            #split any |message|seconds|start fields off of block lines
            blockText, messageFields = self.split_message_fields(line)
            if len(line) == 0: #ignore blank lines
                pass
            elif line[0] == "T":
                #T:<title>
                if not line.startswith("T:") or line[2:].strip() == "":
                    self.reject_workout(filename, "Title line must be T:<title>", lnNum, line)
                    return False
                if AppState.workoutTitle != "":
                    self.reject_workout(filename, "Only one title (T:) line is allowed", lnNum, line)
                    return False
                AppState.workoutTitle = line[2:].strip()
            elif line[0] == "0":
                successBool, duration, position = self.valid_free_ride(blockText, lnNum, filename)
                if successBool == False:
                    return False
                if not self.add_block_messages(messageFields, duration, blockStartSeconds, line, lnNum, filename):
                    return False
                blockStartSeconds += duration
                #(0,duration,position)
                AppState.workoutList.append((0,duration,position))
            elif line[0] == "1":
                successBool, duration, power, position = self.valid_steady_state(blockText, lnNum, filename)
                if successBool == False:
                    return False
                if not self.add_block_messages(messageFields, duration, blockStartSeconds, line, lnNum, filename):
                    return False
                blockStartSeconds += duration
                #(1,duration,(power),position)
                AppState.workoutList.append((1,
                                               duration,
                                               (tuple(int(self.watts_at(power, i, ftpIncrement)) for i in range(21))),
                                               position))
            elif line[0] == "2":
                successBool, duration, powerStart, powerEnd, position = self.valid_ramp(blockText, lnNum, filename)
                if successBool == False:
                    return False
                if not self.add_block_messages(messageFields, duration, blockStartSeconds, line, lnNum, filename):
                    return False
                blockStartSeconds += duration
                block, rampEvents = self.ramp_block(duration, powerStart, powerEnd, position, ftpIncrement)
                AppState.workoutList.append(block)
                #a ramp's power change events go after its block event: (4,durationEvent,(power))
                AppState.workoutList.extend(rampEvents)
            elif line[0] == "3":
                #interval sub-blocks carry their own messages, so pass the whole line
                successBool, multiplier, intvBlocks, rampList, intvMessages = self.valid_interval(line, lnNum, filename, ftpIncrement)
                if successBool == False:
                    return False
                if len(intvBlocks) == 1:
                    #one block repeated: written out as the one block it amounts to (see single_block_interval)
                    blockStartSeconds = self.single_block_interval(multiplier, intvBlocks[0], intvMessages[0],
                                                                   blockStartSeconds)
                    continue
                #each sub-block's messages show on every repeat
                for _ in range(multiplier):
                    for iBlock, iMessages in zip(intvBlocks, intvMessages):
                        for msgStart, msgEnd, msgText in iMessages:
                            AppState.workoutMessages.append((blockStartSeconds+msgStart, blockStartSeconds+msgEnd, msgText))
                        blockStartSeconds += iBlock[1]
                #(3,multiplier,intervalBlock0,intervalBlock1,<intervalBlock2>)
                if len(intvBlocks) == 2:
                    AppState.workoutList.append((3, multiplier, intvBlocks[0], intvBlocks[1]))
                else:
                    AppState.workoutList.append((3, multiplier, intvBlocks[0], intvBlocks[1], intvBlocks[2]))
                #now append the interval block change events and any ramp power change lists
                while multiplier > 0:
                    for iBlIndex, iBlock in enumerate(intvBlocks):
                        #(5,multiplier,intervalIndex,(intervalBlock))
                        AppState.workoutList.append((5,multiplier,iBlIndex,iBlock))
                        #add ramp info if it was a ramp
                        if rampList[iBlIndex] != 0:
                            AppState.workoutList.extend(rampList[iBlIndex])
                    #decrement the multiplier
                    multiplier = multiplier - 1
            else:
                self.reject_workout(filename, f"Invalid block type: {line[0]}", lnNum, line)
                return False
        if self.previewing: #(the workout picker's preview leaves the screen as it is)
            return True
        self.workoutInfo.setText("Workout Duration: " + MinimizedDuration(AppState.workoutDurationSeconds))
        if self.workoutFileError:
            self.workoutFileError = None
            self.render_init_status()
        self.show_workout_text()
        return True

    #a ramp block and its power change events: (2,duration,(start, stop),position) and [(4,durationEvent,(power)),...];
    #a "ramp" with the same start and end power is the steady block it amounts to, (1,duration,(power),position), with
    #no events
    def ramp_block(self, duration, powerStart, powerEnd, position, ftpIncrement):
        if powerStart == powerEnd:
            return (1, duration, tuple(int(self.watts_at(powerStart, i, ftpIncrement)) for i in range(21)), position), []
        block = (2, duration, tuple((int(self.watts_at(powerStart, i, ftpIncrement)),
                                     int(self.watts_at(powerEnd, i, ftpIncrement))) for i in range(21)), position)
        return block, self.process_ramp(duration, powerStart, powerEnd, ftpIncrement)

    #an interval of a single block, 3:<repeats>;<block>, as the one block it amounts to (the workout screen shows
    #intervals of 2 or 3 blocks): a free ride or steady block that long (valid_interval only lets through ones that
    #fit in a block). The block's messages show every repeat, as in an interval. Returns the workout time after it
    def single_block_interval(self, multiplier, block, messages, blockStartSeconds):
        duration = block[1]
        AppState.workoutList.append(block[:1] + (duration*multiplier,) + block[2:])
        for rep in range(multiplier):
            for msgStart, msgEnd, msgText in messages:
                AppState.workoutMessages.append((blockStartSeconds + msgStart, blockStartSeconds + msgEnd, msgText))
            blockStartSeconds += duration
        return blockStartSeconds

    #the loaded workout's blocks, listed beside the settings column
    def show_workout_text(self, more=False):
        workoutText = self.workout_text()
        if more:
            workoutText += self.workoutDisplayLabel.ui.widget.moreText + "\n"
        self.introductoryLayout.addWidget(self.introStartButton, 15,0,1,3)
        self.workoutDisplayLabel.ui.widget.setFullText(workoutText)
        self.introductoryLayout.setAlignment(self.centerInitWidget, Qt.AlignmentFlag.AlignTop|Qt.AlignmentFlag.AlignHCenter)

    #the loaded workout's blocks as the workout list shows them, one line each
    def workout_text(self):
        workoutText = ""
        ##display the workout text
        for index, workoutElement in enumerate(AppState.workoutList):
            #Musical Cadence Mode: a line for each song as it starts
            if AppState.cadenceMode and index in AppState.musicSongStarts:
                workoutText = workoutText + AppState.musicSongStarts[index] + "\n"
            if workoutElement[0] in [0,1,2,3]:
                 #Just use the default power for initialization
                returnedList = ProcessWorkoutLine(workoutElement, 10)
                if workoutElement[0] in [0,1,2]:
                    workoutText = workoutText + returnedList[0] + "\n"
                elif workoutElement[0] == 3:
                    #interval text handling
                    workoutText = workoutText + returnedList[0] #get multiplier
                    workoutText = workoutText + " " + "\n      ".join(returnedList[1:]) + "\n" #add the remaining interval blocks, comma separated
        return workoutText

    def preview_workout(self, filePath, keep=False):
        """A workout file read as if it were being loaded, without loading it (the workout picker's list and preview):
        {"ok", "title", "seconds", "text" (workout_text()), "error" (why it was rejected, None if it wasn't)}.
        keep: its blocks are left in AppState (a simulated ride's own, see PreviewRide.RideSandbox), the screen still
        as it was"""
        saved = (AppState.workoutList, AppState.workoutDurationSeconds, AppState.workoutTitle, AppState.workoutMessages)
        self.previewing = True
        self.previewError = None
        try:
            ok = self.process_workout_file(filePath)
            return {"ok": ok, "title": AppState.workoutTitle, "seconds": AppState.workoutDurationSeconds,
                    "text": self.workout_text() if ok else "", "error": self.previewError}
        finally:
            self.previewing = False
            if not keep:
                AppState.workoutList, AppState.workoutDurationSeconds, AppState.workoutTitle, AppState.workoutMessages = saved

    def eventFilter(self, obj, event):
        #keep the left (device) column exactly as tall as the center column so the
        #bottom buttons on the left align with the bottom of the center column
        if obj is self.centerInitWidget and event.type() == QEvent.Type.Resize:
            self.initializationWidget.setFixedHeight(self.centerInitWidget.height())
        return super().eventFilter(obj, event)

    def process_ramp(self, duration, startPower, endPower, ftpIncrement):
        ramp = []
        rampDurationSecondsList = []
        #find the number of events to be added for the ramp, use non-modified slope to get the count
        adjustedStartPower = int(self.watts_at(startPower, 10, ftpIncrement))
        adjustedEndPower = int(self.watts_at(endPower, 10, ftpIncrement))
        slope = ((adjustedEndPower-adjustedStartPower))/duration
        #store first power recording to ensure no duplicates
        lastPower = adjustedStartPower
        #store fist duration to track when to reduce ramp points
        lastDuration = 0
        #a new power event for each whole watt the ramp moves, at most one a second (a gentle ramp is already held
        #back by the whole watts; drawing a steep one's many bars is cheap, see SolidColoredColumns.paint)
        durationCheck = 1
        #need to stop at the duration, not one before it
        for i in range(duration+1):
            power=int((slope*i)+adjustedStartPower)
            if power != lastPower and i-lastDuration >= durationCheck:
                #new value at long enough duration, set last values and add duration to list
                lastPower = power
                rampDurationSecondsList.append([i,i-lastDuration])
                lastDuration = i
        #the ramp's power events must cover the whole block: HUDWindow reads the element after a ramp block
        #as its first power event, and moves on to the next block once the last one runs out. A ramp too
        #shallow to change by a whole watt (at base FTP) would otherwise get no events at all
        if lastDuration < duration:
            rampDurationSecondsList.append([duration, duration-lastDuration])
        #now append events at the durations found for each difficulty
        def slopey(endPower, startPower, rDur, tDur, difficulty):
            ePower = self.watts_at(endPower, difficulty, ftpIncrement)
            sPower = self.watts_at(startPower, difficulty, ftpIncrement)
            outPower = int(sPower + (((ePower - sPower)*rDur)/tDur))
            return outPower
        for durationSeconds in rampDurationSecondsList:
            ramp.append((4,
                         durationSeconds[1],
                         (tuple(slopey(endPower, startPower, durationSeconds[0], duration, i) for i in range(21))),
                         duration))
        return ramp

    #Timed messages go after a block, separated by |:
    #<block>|<message>|<seconds shown>|<start second in block>
    #seconds shown is optional (default: rest of the block), start is optional (default: 0, the block start)
    #more than one message on a block needs all three fields for each: <block>|<msg>|<sec>|<start>|<msg>|<sec>|<start>
    # 5m steady state, "Stay seated" for the first 15s and "Halfway!" at 150s for 10s
        #1:300,0.85|Stay seated|15|0|Halfway!|10|150
    #in intervals each sub-block can have its own messages, shown on every repeat
        #3:4;1:30,1.2,3|Sprint!|10;0:30|Recover
    #messages can contain commas and colons but not | or ;
    def split_message_fields(self, blockText):
        #returns the block text with its message fields removed, and the list of message fields
        fields = blockText.split("|")
        return fields[0], fields[1:]

    def parse_block_messages(self, messageFields, blockDuration, line, lnNum, filename):
        #returns successBool and a list of (start second, end second, text) relative to the block start
        messages = []
        if len(messageFields) == 0:
            return True, messages
        if len(messageFields) <= 3:
            groups = [messageFields]
        elif len(messageFields) % 3 == 0:
            groups = [messageFields[i:i+3] for i in range(0, len(messageFields), 3)]
        else:
            self.reject_workout(filename, "Multiple messages need |message|seconds|start each", lnNum, line)
            return False, messages
        for group in groups:
            text = group[0].strip()
            seconds = group[1].strip() if len(group) > 1 else ""
            start = group[2].strip() if len(group) > 2 else ""
            if text == "":
                self.reject_workout(filename, "Message text can't be empty", lnNum, line)
                return False, messages
            if start == "":
                start = 0
            elif start.isdigit() and int(start) < blockDuration:
                start = int(start)
            else:
                self.reject_workout(filename, "Message start must be before the block ends", lnNum, line)
                return False, messages
            if seconds == "":
                seconds = blockDuration - start
            elif seconds.isdigit() and int(seconds) >= 1:
                seconds = int(seconds)
            else:
                self.reject_workout(filename, "Message seconds must be a whole number, 1+", lnNum, line)
                return False, messages
            if start + seconds > blockDuration:
                self.reject_workout(filename, "Message must end by the end of its block", lnNum, line)
                return False, messages
            messages.append((start, start + seconds, text))
        return True, messages

    def add_block_messages(self, messageFields, blockDuration, blockStartSeconds, line, lnNum, filename):
        #validates a block's messages and adds them to the workout message list at workout time
        successBool, messages = self.parse_block_messages(messageFields, blockDuration, line, lnNum, filename)
        if successBool:
            for msgStart, msgEnd, msgText in messages:
                AppState.workoutMessages.append((blockStartSeconds+msgStart, blockStartSeconds+msgEnd, msgText))
        return successBool

    def duration_checker(self, duration, line, lnNum, filename):
        if duration.isdigit() == False:
            self.reject_workout(filename, "Block durations must be integers", lnNum, line)
            return False
        if not 1 <= int(duration) <= self.maxBlockSeconds: #max block length of 30m, duration must be positive
            self.reject_workout(filename, f"Block durations must be between 1 and {self.maxBlockSeconds}s", lnNum, line)
            return False
        return True
    
    #a block's power: (FTP multiplier, False), or (watts, True) for explicit power written with a W after it
    #(e.g. 250W), which doesn't change with FTP; None (rejected) if it's neither
    def power_value(self, text, line, lnNum, filename):
        text = text.strip()
        if text[-1:] in ("W", "w"):
            if text[:-1].strip().isdigit() and MIN_EXPLICIT_WATTS <= int(text[:-1]) <= MAX_EXPLICIT_WATTS:
                return (int(text[:-1]), True)
            self.reject_workout(filename, f"Watts must be a whole number, {MIN_EXPLICIT_WATTS} to {MAX_EXPLICIT_WATTS}W",
                                lnNum, line)
            return None
        try:
            multiplier = float(text)
        except ValueError:
            multiplier = None
        if multiplier is None or not MIN_FTP_MULTIPLIER <= multiplier <= MAX_FTP_MULTIPLIER:
            self.reject_workout(filename, f"Power must be {MIN_FTP_MULTIPLIER} to {MAX_FTP_MULTIPLIER} (x FTP) "
                                          f"or watts (e.g. 250W)", lnNum, line)
            return None
        return (multiplier, False)

    #a power_value() at one of the 21 difficulties (10 is as written): an FTP multiplier x the difficulty's FTP, or
    #explicit watts moved the same fraction per difficulty step as FTP is
    @staticmethod
    def watts_at(power, difficulty, ftpIncrement):
        value, explicit = power
        if explicit:
            return value*(1 + (difficulty - 10)*DIFFICULTY_STEP)
        return value*(int(AppState.ftp)+ftpIncrement[difficulty])

    def position_checker(self, position, line, lnNum, filename):
        try:
            intP = int(position)
        except ValueError:
            self.reject_workout(filename, "Position must be 0 to 5", lnNum, line)
            return False, 0
        if intP not in [0,1,2,3,4,5]:
            self.reject_workout(filename, "Position must be 0 to 5", lnNum, line)
            return False, 0
        return True, intP
        
    #<freeride = 0>:<duration in seconds>,<riding position>
    #riding position: 0=seated (default), 1=seated high cadence, 2=seated low cadence, 3=standing, 4=single leg, 5=switch single leg
    # 10s switch single leg
        #0:10,5
    def valid_free_ride(self, line, lnNum, filename, multiplier=1):
        duration = 0
        position = 0
        blockData  = line.split(":")
        if len(blockData) != 2:
            self.reject_workout(filename, "Need exactly one colon after block type", lnNum, line)
            return False, duration, position
        if blockData[0] != "0":
            self.reject_workout(filename, "Block type must be 0 to 3", lnNum, line)
            return False, duration, position
        blockDataList = blockData[1].split(",")
        if len(blockDataList) not in [1,2]:
            self.reject_workout(filename, "Free ride needs 1 or 2 values", lnNum, line)
            return False, duration, position
        if self.duration_checker(blockDataList[0], line, lnNum, filename) == False:
            return False, duration, position
        duration = int(blockDataList[0])
        AppState.workoutDurationSeconds = AppState.workoutDurationSeconds + (duration * multiplier)
        if len(blockDataList) == 2: #position data exists
            success, position = self.position_checker(blockDataList[1], line, lnNum, filename)
            if success == False:
                return False, duration, position
        return True, duration, position


    #<steady = 1>:<duration in seconds>,<ftp% or watts>,<riding position>
    #riding position: 0=seated (default), 1=seated high cadence, 2=seated low cadence, 3=standing, 4=single leg, 5=switch single leg
    #power is an FTP multiplier, or explicit watts with a W after them (the same whatever the FTP)
    # SteadyState 60s 0.8xFTP standing
        #1:60,0.8,3
    # SteadyState 60s at 250 watts
        #1:60,250W
    def valid_steady_state(self, line, lnNum, filename, multiplier=1):
        duration = 0
        power = 0
        position = 0
        blockData  = line.split(":")
        if len(blockData) != 2:
            self.reject_workout(filename, "Need exactly one colon after block type", lnNum, line)
            return False, duration, power, position
        if blockData[0] != "1":
            self.reject_workout(filename, "Block type must be 0 to 3", lnNum, line)
            return False, duration, power, position
        blockDataList = blockData[1].split(",")
        if len(blockDataList) not in [2,3]:
            self.reject_workout(filename, "Steady state needs 2 or 3 values", lnNum, line)
            return False, duration, power, position
        if self.duration_checker(blockDataList[0], line, lnNum, filename) == False:
            return False, duration, power, position
        duration = int(blockDataList[0])
        AppState.workoutDurationSeconds = AppState.workoutDurationSeconds + (duration * multiplier)
        power = self.power_value(blockDataList[1], line, lnNum, filename)
        if power is None:
            return False, duration, power, position
        if len(blockDataList) == 3: #position data exists
            success, position = self.position_checker(blockDataList[2], line, lnNum, filename)
            if success == False:
                return False, duration, power, position
        return True, duration, power, position


    #<ramp = 2>:<duration in seconds>,<start ftp% or watts>,<end ftp% or watts>,<riding position>
    #riding position: 0=seated (default), 1=seated high cadence, 2=seated low cadence, 3=standing, 4=single leg, 5=switch single leg
    # Ramp Duration 600s from 0.25xFTP to 0.75xFTP seated low cadence 
        #2:600,0.25,0.75,2
    def valid_ramp(self, line, lnNum, filename, multiplier=1):
        duration = 0
        powerStart = 0
        powerEnd = 0
        position = 0
        blockData  = line.split(":")
        if len(blockData) != 2:
            self.reject_workout(filename, "Need exactly one colon after block type", lnNum, line)
            return False, duration, powerStart, powerEnd, position
        if blockData[0] != "2":
            self.reject_workout(filename, "Block type must be 0 to 3", lnNum, line)
            return False, duration, powerStart, powerEnd, position
        blockDataList = blockData[1].split(",")
        if len(blockDataList) not in [3,4]:
            self.reject_workout(filename, "Ramp needs 3 or 4 values", lnNum, line)
            return False, duration, powerStart, powerEnd, position
        if self.duration_checker(blockDataList[0], line, lnNum, filename) == False:
            return False, duration, powerStart, powerEnd, position
        duration = int(blockDataList[0])
        AppState.workoutDurationSeconds = AppState.workoutDurationSeconds + (duration * multiplier)
        powerStart = self.power_value(blockDataList[1], line, lnNum, filename)
        if powerStart is None:
            return False, duration, powerStart, powerEnd, position
        powerEnd = self.power_value(blockDataList[2], line, lnNum, filename)
        if powerEnd is None:
            return False, duration, powerStart, powerEnd, position
        #(the same start and end power is a steady block, see ramp_block)
        if len(blockDataList) == 4: #position data exists
            success, position = self.position_checker(blockDataList[3], line, lnNum, filename)
            if success == False:
                return False, duration, powerStart, powerEnd, position
        return True, duration, powerStart, powerEnd, position


    #<interval = 3>:<multiplier>;<workout block 1>;<workout block 2 (optional)>;<workout block 3 (optional)>
    #(a single block has to amount to one free ride or steady block, see single_block_interval)
    # Intervals Repeat=2 (Steady State 30s 1.85xFTP standing) (Ramp 60s from 0.25xFTP to 0.75xFTP seated high cadence)
        #3:2;1:30,1.85,3;2:60,0.25,0.75,1
    #returnIntervalList returned is 21 element array:
    # each has 3 or 4 sub arrays filled with [<multiplier string>,<interval block 0>,<interval block 1>,<maybe interval block2>]
    #returnIntervalListRaw returned also has 21 element array:
    # each has 4 or 5 sub arrays filled with [3,<multiplier int>,<interval block 0 raw>,<interval block 1 raw>,<maybe interval block2 raw>]
    def valid_interval(self, line, lnNum, filename, ftpIncrement):
        #returns: successBool, multiplier, intvBlocks, rampList, intvMessages
        #initialize return values
        multiplier = 1
        intvBlocks = []
        rampList = []
        intvMessages = [] #per interval block list of (start, end, text), relative to that block's start
        blockData = line.split(";")
        if len(blockData) not in [2,3,4]:
            self.reject_workout(filename, "Interval needs 1 to 3 blocks after 3:<repeats>", lnNum, line)
            return False, multiplier, intvBlocks, rampList, intvMessages
        if "|" in blockData[0]:
            self.reject_workout(filename, "Put messages on interval blocks, not on 3:<repeats>", lnNum, line)
            return False, multiplier, intvBlocks, rampList, intvMessages
        firstBlockList = blockData[0].split(":")
        if len(firstBlockList) != 2:
            self.reject_workout(filename, "Interval must start with 3:<repeats>", lnNum, line)
            return False, multiplier, intvBlocks, rampList, intvMessages
        if firstBlockList[0] != "3":
            self.reject_workout(filename, "Block type must be 0 to 3", lnNum, line)
            return False, multiplier, intvBlocks, rampList, intvMessages
        if firstBlockList[1].isdigit() == False:
            self.reject_workout(filename, "Interval repeats must be a whole number, 1+", lnNum, line)
            return False, multiplier, intvBlocks, rampList, intvMessages
        if int(firstBlockList[1]) < 1:
            self.reject_workout(filename, "Interval repeats must be a whole number, 1+", lnNum, line)
            return False, multiplier, intvBlocks, rampList, intvMessages
        multiplier = int(firstBlockList[1])
        #remove the first element, 3:<multiplier>, from the interval workout block
        blockData.pop(0) 
        for intervalBlock in blockData:
            #split off the interval block's messages
            intervalBlock, messageFields = self.split_message_fields(intervalBlock)
            if intervalBlock == "":
                self.reject_workout(filename, "Empty interval block (extra semicolon?)", lnNum, line)
                return False, multiplier, intvBlocks, rampList, intvMessages
            if intervalBlock[0] == "0":
                successBool, duration, position = self.valid_free_ride(intervalBlock, lnNum, filename, multiplier)
                if successBool == False:
                    return False, multiplier, intvBlocks, rampList, intvMessages
                #add the free ride
                intvBlocks.append((0,duration,position))
                #add a blank spot to the ramp list
                rampList.append(0)
            elif intervalBlock[0] == "1":
                successBool, duration, power, position = self.valid_steady_state(intervalBlock, lnNum, filename, multiplier)
                if successBool == False:
                    return False, multiplier, intvBlocks, rampList, intvMessages
                #add the steady state ride at each difficulty
                intvBlocks.append((1,
                                    duration,
                                    (tuple(int(self.watts_at(power, i, ftpIncrement)) for i in range(21))),
                                    position))
                #add a blank spot to the ramp list
                rampList.append(0)
            elif intervalBlock[0] == "2":
                successBool, duration, powerStart, powerEnd, position = self.valid_ramp(intervalBlock, lnNum, filename, multiplier)
                if successBool == False:
                    return False, multiplier, intvBlocks, rampList, intvMessages
                #add the ramp rides at each difficulty, and its power change events (none if it's really steady)
                block, rampEvents = self.ramp_block(duration, powerStart, powerEnd, position, ftpIncrement)
                intvBlocks.append(block)
                rampList.append(rampEvents or 0)
            else:
                self.reject_workout(filename, f"Invalid block type in interval: {intervalBlock[0]}", lnNum, line)
                return False, multiplier, intvBlocks, rampList, intvMessages
            #validate this interval block's messages against its duration
            successBool, messages = self.parse_block_messages(messageFields, intvBlocks[-1][1], line, lnNum, filename)
            if successBool == False:
                return False, multiplier, intvBlocks, rampList, intvMessages
            intvMessages.append(messages)
            ##Now update the duration power workout list with the multiplier
        #a single block repeated has to be one free ride or steady block (see single_block_interval)
        if len(intvBlocks) == 1:
            if intvBlocks[0][0] == 2:
                self.reject_workout(filename, "A one block interval can't be a ramp: write the ramps out, or add a "
                                              "second block", lnNum, line)
                return False, multiplier, intvBlocks, rampList, intvMessages
            if intvBlocks[0][1]*multiplier > self.maxBlockSeconds:
                self.reject_workout(filename, f"A one block interval is one block, at most {self.maxBlockSeconds}s "
                                              f"in all", lnNum, line)
                return False, multiplier, intvBlocks, rampList, intvMessages
        return True, multiplier, intvBlocks, rampList, intvMessages

    #function to check if FTP and Max HR have been set, and if the devices are connected
    def check_enable_start_button(self):
        ready = False
        mainReqs = False
        if AppState.wheelCircumference != "0":
            self.wheelCSet = True
        if AppState.ftp != "0":
            self.ftpSet = True
        if AppState.maxHr != "0":
            self.maxHrSet = True
        self.render_init_status()
        if self.wheelCSet and (self.ftpSet or self.ftp_optional()) and self.maxHrSet and self.parent.startExe.is_ready():
            mainReqs = True
        if mainReqs:
            if AppState.workoutMode:
                if self.workout_ready():
                    ready = True
            else: #free mode
                ready = True
        if ready:
            self.introStartButton.setEnabled(True)
            if not (self.hrButtonBlink or self.spdCadButtonBlink):
                self.startButtonBlink = True
        else:
            self.introStartButton.setEnabled(False)
            self.startButtonBlink = False

    #function to start the HUD
    def show_hud_window(self):
        #Guard against a stale enabled Start button (e.g. Workout Mode with no workout loaded)
        if AppState.workoutMode and not self.workout_ready():
            self.check_enable_start_button()
            return
        self.hide()
        self.stop()
        AppState.startTimestamp = datetime.datetime.now()
        self.parent.generate_hud()