import sys

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QApplication, QPushButton, QVBoxLayout, QWidget

import AppLayout

from ClassAnalogGauge import *
from ClassLongVerticalLabel import *
from ClassLongVerticalLabelSeries import *
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *
import ClassAntLogger


###AntWorkoutBridge (see ClassAntDevices.py) replaces the old StartAnt/
###AntWorkout.exe launch entirely - MainWindow now instantiates
###AntWorkoutBridge directly, which owns the live ANT connection for the
###life of the app instead of a fire-and-exit exe launch.

###Confirm Exit QWidget Window
class ConfirmExitWindow(QWidget):
    def __init__(self, closeEvent, parent):
        super(ConfirmExitWindow, self).__init__()

        self.parent = parent
        self.closeEvent = closeEvent

        self.setWindowTitle("   Exit Awooo Workout?   ")
        self.exitSynopsisConfirmButton = QPushButton("Yes, Exit")
        self.exitSynopsisCancelButton = QPushButton("No")

        self.setStyleSheet("""
                            ConfirmExitWindow {background-color:#9fc5d1;}
                          """)

        self.exitSynopsisConfirmButton.clicked.connect(self.exit_program)
        self.exitSynopsisCancelButton.clicked.connect(self.hide_confirm_exit_window)

        exitLayout = QVBoxLayout()     
        #Frameless window
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        #Titlebar
        self.title_bar = CustomTitleBar(self, False)
        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        self.title_bar.setFixedHeight(36)
        exitLayout.addWidget(self.title_bar)
        exitLayout.addWidget(self.exitSynopsisConfirmButton)
        exitLayout.addWidget(self.exitSynopsisCancelButton)
        self.setLayout(exitLayout)

        #scaled for this screen, its text fitted to it (see AppLayout.py)
        AppLayout.fit_dialog(self)

    def exit_program(self):
        # Hide every window first so the app disappears the moment Yes is
        # clicked; the ANT shutdown below can take a second or more (the
        # SDK's USB receive thread only notices the stop between 1s reads)
        for window in QApplication.topLevelWidgets():
            window.hide()
        # Cleanly close the ANT USB connection (replaces the old
        # startANT.txt flag-file signal to AntWorkout.exe). The bridge lives
        # on MainWindow; from the synopsis screen self.parent is the
        # SynopsisWindow, so look one level up too.
        bridge = getattr(self.parent, "startExe", None)
        if bridge is None:
            bridge = getattr(getattr(self.parent, "parent", None), "startExe", None)
        if bridge is not None:
            bridge.stop()
            # stop() only asks the bridge thread to finish; wait for it to
            # actually close the stick before the process exits, or the
            # stick is left mid-session for the next launch to recover from
            if not bridge.wait(3000):
                ClassAntLogger.log_event("ANT", "ANT bridge did not shut down within 3s, exiting anyway")
        sys.exit(0)

    def hide_confirm_exit_window(self):
        self.hide()