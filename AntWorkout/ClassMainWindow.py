
from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtGui import QKeySequence, QShortcut
from PyQt6.QtWidgets import QMainWindow, QVBoxLayout, QWidget

from ClassAnalogGauge import *
from ClassLongVerticalLabelSeries import *
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *
from ClassAntDevices import AntWorkoutBridge

from AppState import AppState
import WorkoutData
from ClassWorkoutSetupWindow import WorkoutSetupWindow
from ClassHUDWindow import HUDWindow
from ClassSynopsisWindow import SynopsisWindow
from ClassConfirmExitWindow import ConfirmExitWindow
import SoundEffects
import AppLayout





class MainWindow(QMainWindow):
    #App title shown in the custom title bar. On the Workout GUI/Synopsis
    #screens this gets suffixed with the active workout's filename (see
    #_apply_app_title below); the Setup screen always shows just this.
    APP_TITLE = "Awooo Workout"

    def __init__(self):
        super(MainWindow, self).__init__()

        #Open the ANT USB stick and start the live device bridge
        #(replaces the old AntWorkout.exe launch - see ClassAntDevices.py)
        self.startExe = AntWorkoutBridge()
        self.startExe.start()

        #Generate the layout for the main widget. This was a CLASS
        #attribute before (mainLayout = QVBoxLayout() directly under the
        #class line) rather than an instance one - harmless as long as
        #MainWindow is only ever constructed once per process (true
        #today), but a latent bug that would silently share one QLayout
        #across every instance if that ever changed, so it's an instance
        #attribute now like everything else built in __init__.
        self.mainLayout = QVBoxLayout()

        #Screensize
        self.resize(800,600) #this resize fixes an issue with some widget sizes
        self.showMaximized()
        #Store the resolution

        #Keep keboard focus, prevents weird spacebar screen doubling bug
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        #Frameless window
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        #See what lies below
        # self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

        #Title
        self.setWindowTitle("   " + self.APP_TITLE)
        self.setStyleSheet("""
                            QMainWindow{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}
                          """)
        
        #Titlebar
        self.title_bar = CustomTitleBar(self)
        self.title_bar.setFixedHeight(36)
        self.mainLayout.addWidget(self.title_bar)

        #Generate the main, initial widget
        self.mainWidget = QWidget()

        ### Initial Window Display setup
        #Define the setup widget and add it to the main layout
        self.setupWidget = WorkoutSetupWindow(self)

        self.mainLayout.addWidget(self.setupWidget)

        #Add the main layout to the main widget and set it as central
        self.mainWidget.setLayout(self.mainLayout)
        self.setCentralWidget(self.mainWidget)
        
        #Hide the synopsis and show the setup in the main window
        self.setupWidget.show()

        #fit the window to this screen's size (the theme's layout for it, see AppLayout.py)
        AppLayout.layout_window(self)

        #Esc: the End Workout prompt during a ride (it pauses the ride), otherwise the same as the X (the exit prompt)
        QShortcut(QKeySequence(Qt.Key.Key_Escape), self, activated=self.escape_pressed)
        #no text box focused to start with: the first Tab goes to the first one (see KeyShortcuts.tab_cycle)
        self.setFocus()

        #HUD will be set up once we're sure that FTP and Max HR are set

    def _apply_app_title(self, show_workout_filename=False):
        #Setup screen always shows just the app name. The Workout GUI and
        #Synopsis screens also show the active workout's filename, but only
        #when running an actual (non-Free Ride) workout with a file chosen, or an FTP test (its name).
        if show_workout_filename and AppState.workoutMode and AppState.workout_display_name():
            title_text = f"{self.APP_TITLE}: {AppState.workout_display_name()}"
        else:
            title_text = self.APP_TITLE
        display_text = "   " + title_text
        self.setWindowTitle(display_text)
        self.title_bar.set_title(display_text)

    ###Main Synopsis Window functions
    def go_back_to_setup(self):
        #Get the AntWorkout executable started
        self.workoutHudWidget.antDevices.stop()
        self.workoutHudWidget.hide()

        #Clears the bridge's selected devices and connect/ready state and
        #stops RecoveryManager watching them
        self.startExe.request_disconnect()

        ## Clear the finished workout's sensor history
        WorkoutData.clear()

        ##Reset global timer
        AppState.secondsPassed = 0.0

        #Time of workout power requested; ([secondsPassed],[power],[system time])
        AppState.workoutPowerRequestsSeconds.clear()
        AppState.workoutPowerRequestsWatts.clear()
        AppState.workoutPowerRequestsSysTime.clear()

        #Clear the workout filename back out of the title bar
        self._apply_app_title()

        #Setup window Settings
        self.setupWidget.scanBool = True

        #Widget settings
        SoundEffects.play("window")
        self.setupWidget.show()
        self.setupWidget.check_enable_start_button()
        self.setupWidget.restartScanButton.setEnabled(False)
        self.setupWidget.connectButton.setEnabled(False)
        self.setupWidget.resume()

        #Calling showMaximized() immediately after swapping the central
        #widget doesn't reliably take effect on this frameless window (a
        #known Qt-on-Windows quirk) - the window can end up sized smaller
        #than the screen, pushing the bottom-row START button off the
        #visible area until the user manually clicks the OS maximize
        #button. Deferring it a tick, the same way SynopsisWindow.fix_size()
        #already does elsewhere in this file, lets the new layout settle
        #first so the maximize actually applies.
        QTimer.singleShot(0, self.showMaximized)

    def _discard_widget(self, attr_name):
        #Removes and destroys a previously-added screen (HUD/synopsis)
        #before replacing it, so a screen that somehow gets generated
        #twice (e.g. a double-click on "Exit to Synopsis" firing
        #end_workout() twice before the first click's work disables
        #anything) self-heals instead of leaving two full copies of that
        #screen stacked on top of each other in mainLayout - hide() alone
        #doesn't remove a widget from its layout, only makes it invisible,
        #so a stale reference here would otherwise linger forever.
        old = getattr(self, attr_name, None)
        if old is not None:
            self.mainLayout.removeWidget(old)
            old.hide()
            old.deleteLater()
            setattr(self, attr_name, None)

    def generate_hud(self):
        #Show the active workout's filename alongside the app title
        self._apply_app_title(show_workout_filename=True)

        self._discard_widget("workoutHudWidget")

        #Define the workout hud widget and add it to the main layout
        self.workoutHudWidget = HUDWindow(self)
        self.mainLayout.addWidget(self.workoutHudWidget)
        AppLayout.layout_window(self)
        SoundEffects.play("window")
        self.workoutHudWidget.show()
        self.workoutHudWidget.antDevices.start()
        self.showMaximized()

    def generate_synopsis(self):
        ## Drop the connecting/ready state to ready the user for hardware/setup changes
        self.startExe.clear_connection_flags()

        #Keep showing the workout filename on the synopsis screen
        self._apply_app_title(show_workout_filename=True)

        self._discard_widget("synopsisWidget")

        self.synopsisWidget = SynopsisWindow(self)
        self.mainLayout.addWidget(self.synopsisWidget)
        AppLayout.layout_window(self)
        self.workoutHudWidget.hide()
        SoundEffects.play("window")
        self.synopsisWidget.show()
        self.showMaximized()
   

    def escape_pressed(self):
        #the synopsis' Block Grades sheet or an open block grade closes first
        synopsis = getattr(self, "synopsisWidget", None)
        if synopsis is not None and synopsis.isVisible() and synopsis.close_overlays():
            return
        hud = getattr(self, "workoutHudWidget", None)
        if hud is not None and hud.isVisible() and hud.end_available():
            prompt = getattr(hud, "w", None)
            if prompt is not None and prompt.isVisible():
                #already up: bring it back rather than stacking a second one
                prompt.raise_()
                prompt.activateWindow()
            else:
                hud.confirm_end_workout()
            return
        prompt = getattr(self, "w", None)
        if prompt is not None and prompt.isVisible():
            prompt.raise_()
            prompt.activateWindow()
        else:
            self.close()

    ###Generic close function
    def closeEvent(self, event):
        # Make sure the user wants to close the app
        event.ignore()
        self.w = ConfirmExitWindow(self)
        self.w.show()