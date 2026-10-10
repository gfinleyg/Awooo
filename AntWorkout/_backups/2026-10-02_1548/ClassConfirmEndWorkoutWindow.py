from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QGridLayout, QPushButton, QWidget

from ClassAnalogGauge import *
from ClassLongVerticalLabel import *
from ClassLongVerticalLabelSeries import *
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *

from AppState import AppState



###Confirm End Workout QWidget Window
class ConfirmEndWorkoutWindow(QWidget):
    def __init__(self, parent):
        super(ConfirmEndWorkoutWindow, self).__init__()
        self.parent = parent
        if AppState.workoutMode:
            self.setWindowTitle("End Awooo Workout?")
            self.endWorkoutRestartButton = QPushButton("Keep Existing Data and Restart")
            self.endWorkoutRestartButton.clicked.connect(self.restart_workout)
        else:
            self.setWindowTitle("End Free Ride?")
        self.endWorkoutSetupButton = QPushButton("Clear Data and Go Back to Setup")
        self.endWorkoutConfirmButton = QPushButton("Exit to Synopsis")
        self.endWorkoutCancelButton = QPushButton("Keep Going!")

        self.setStyleSheet("""
                            ConfirmEndWorkoutWindow {background-color:#9fc5d1;}
                          """)
        
        
        self.endWorkoutSetupButton.clicked.connect(self.back_to_setup)
        self.endWorkoutConfirmButton.clicked.connect(self.end_workout)
        self.endWorkoutCancelButton.clicked.connect(self.hide_confirm_exit_window)

        if self.parent.workoutUnpausedBool and AppState.workoutMode:
            self.parent.pause_recording()

        endLayout = QGridLayout()     
        #Frameless window
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        #Titlebar
        self.title_bar = CustomTitleBar(self, False)

        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        self.title_bar.setFixedHeight(36)
        endLayout.addWidget(self.title_bar,0,0,1,2)
        if AppState.workoutMode:
            endLayout.addWidget(self.endWorkoutRestartButton,1,0,1,2)
        endLayout.addWidget(self.endWorkoutSetupButton,2,0,1,2)
        endLayout.addWidget(self.endWorkoutConfirmButton,3,0)
        endLayout.addWidget(self.endWorkoutCancelButton,3,1)
        self.setLayout(endLayout)

    def restart_workout(self):
        #reset the workout settings
        self.parent.restart_workout()
        
        #hide this dialogue box
        self.hide()

    def back_to_setup(self):
        #stop timer
        self.parent.timer.stop()

        #reset the workout settings
        self.parent.restart_workout()

        #hide the hud window and show the setup window again
        self.parent.parent.go_back_to_setup()

        #hide this end workout dialogue box
        self.hide()

    def end_workout(self):
        #Guard against a rapid double-click on "Exit to Synopsis" firing
        #this twice (QPushButton.clicked fires once per press-release, so
        #two quick clicks - or a mouse double-click - queue two calls
        #here before the first one's self.hide() below has a chance to
        #actually stop further clicks from reaching this button)  - that
        #used to create two full SynopsisWindow instances stacked on top
        #of each other, since generate_synopsis() had no idea it had
        #already been called once. Disabling the button as the very
        #first thing makes a second, already-queued click into a no-op.
        self.endWorkoutConfirmButton.setEnabled(False)

        #stop timer
        self.parent.timer.stop()

        #grade the block being ridden up to now (workout mode; a completed workout has nothing left to grade)
        if AppState.workoutMode:
            self.parent.end_grading()

        # end startedBool and workoutUnpausedBool bool
        self.parent.workoutUnpausedBool = False
        self.parent.startedBool = False

        #stop the ant device text reading
        self.parent.antDevices.stop()

        #hide this end workout dialogue box
        self.hide()

        #hide the HUD widget, show the synopsis widget
        #ugly, but parent is HUD and it's parent is the main window
        self.parent.parent.generate_synopsis()

    def hide_confirm_exit_window(self):
        self.parent.pause_recording()
        self.hide()

    def closeEvent(self, _):
        self.parent.pause_recording()
        self.hide