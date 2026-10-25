###Two window layout viewer: one window shows the real Init (setup), Workout (HUD, frozen part way
###through a simulated ride), Synopsis, Music Picker or Workout Picker screen, all in the mode picked
###on the Scenario tab (Workout Mode, Free Mode, an FTP test or Musical Cadence Mode); the other picks
###which one is shown and changes its fonts, font sizes and colors live (one color list for the whole
###app, so a color changes everywhere it's used, and the power zone colors on their own tab), and
###previews it on other screen sizes (the app scales itself to fit each one, see AppLayout.py). No ANT stick needed, a fake bridge stands in for
###it, and Musical Cadence Mode rides made-up songs (nothing is analyzed or played).
###
###The theming lives in AppTheme.py and the scaling in AppLayout.py, both shared with the app: a
###theme .json saved here holds the fonts and colors, and the app loads it from its title bar
###settings menu.
###Run: python LayoutViewer.py
###Fonts render with the same engine the app uses (Qt 6.7's GDI), so what shows here is exactly
###what the app will show. GDI only sees each variable font's default instance (e.g. "Anybody Thin");
###add --directwrite to browse their other weights, which the app can't use yet.

###Same import order guard as AntWorkout.py: ant_native.pyd must load before PyQt6
import os as _os
import sys as _sys
_ant_native_python_dir = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "AntNative", "python")
if _ant_native_python_dir not in _sys.path:
    _sys.path.insert(0, _ant_native_python_dir)
import ant.manager as _ant_manager_preload  # noqa: F401  (import order guard, see above)

import datetime
import re
import sys
import time

#settings, workouts and themes are all relative to the project folder (a frozen build's: the exe's folder,
#same as AntWorkout.py; __file__ points inside the bundle there)
_os.chdir(_os.path.dirname(_sys.executable) if getattr(_sys, "frozen", False) else
          _os.path.dirname(_os.path.abspath(__file__)))
if "--directwrite" in sys.argv and "QT_QPA_PLATFORM" not in _os.environ:
    _os.environ["QT_QPA_PLATFORM"] = "windows:fontengine=directwrite"

from PyQt6.QtCore import QEvent, QPoint, QRect, QRectF, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import (QColor, QFont, QFontDatabase, QLinearGradient, QPainter, QPalette, QPen, QShortcut,
                         QKeySequence)
from PyQt6.QtWidgets import (QApplication, QButtonGroup, QCheckBox, QColorDialog, QComboBox, QFileDialog,
                             QFormLayout, QGridLayout, QGroupBox, QHBoxLayout, QLabel, QLayout, QMainWindow,
                             QMessageBox, QPushButton, QRadioButton, QScrollArea, QSizePolicy, QSlider, QSpinBox,
                             QTabWidget, QVBoxLayout, QWidget)

from AppState import AppState
import WorkoutData
from WorkoutHelpers import WriteSelectedDeviceLines, ClearSelectedDeviceLines
from ClassCustomTitleBar import CustomTitleBar
from ClassWorkoutSetupWindow import WorkoutSetupWindow
from ClassHUDWindow import HUDWindow
from ClassSynopsisWindow import SynopsisWindow
from ClassMusicSelectWindow import MusicSelectWindow
from ClassWorkoutSelectWindow import WorkoutSelectWindow
import MusicCadence
import AppTheme
import AppLayout
import AppStyle
import FtpColors
import KeyShortcuts
from FtpColorsEditor import FtpColorsTab, FtpMockupWindow
from AppLayout import TASKBAR_HEIGHT
from AppTheme import ROLE_MAIN, ROLE_GAUGE, ROLE_TITLES, WEIGHTS, colors_in_css, colors_in_value
#the fake ANT bridge, simulated rider and ride stepping are shared with the app's theme picker
from PreviewRide import (FakeBridge, Rider, ViewerMusicPlayer, save_reading, show_reading, run_hud,
                         reset_ride_state)


PROJECT_DIR = _os.getcwd()
WORKOUT_DIR = _os.path.join(PROJECT_DIR, "awoWorkouts")
DEFAULT_WORKOUT = "0_Little_bit_of_everything.awo"
APP_TITLE = "Awooo Workout"
#the view buttons; Workout is the Free Ride screen in Free Mode
VIEWS = ("Init", "Workout", "Synopsis", "Music Picker", "Workout Picker")
#the Scenario tab's modes: (setup screen mode, name)
MODES = (("workout", "Workout Mode (workout file)"), ("free", "Free Mode"), ("20m", "20m FTP Test"),
         ("1h", "1h FTP Test"), ("ramp10", "10W delta FTP Ramp"), ("ramp20", "20W delta FTP Ramp"),
         ("cadence", "Musical Cadence Mode"))
#how long a Free Mode ride is (Workout progress is a part of it)
FREE_RIDE_SECONDS = 45*60
SCREENSHOT_DIR = _os.path.join(PROJECT_DIR, "layoutScreenshots")
#where colors and gradients files are saved by default (see AppTheme.PARTS)
PART_DIRS = AppTheme.PART_DIRS

#simulated screens: effective (after Windows display scaling) size, and what typically has it
SCREEN_SIZES = [
    (None, "This screen (maximized)"),
    ((1280, 720), "13–14\" FHD laptop at 150%"),
    ((1280, 800), "13–14\" 1920×1200 laptop at 150%"),
    ((1366, 768), "Older budget laptop"),
    ((1440, 900), "2880×1800 laptop at 200%"),
    ((1536, 864), "15\" FHD laptop at 125% (most common laptop)"),
    ((1600, 900), "1600×900 laptop"),
    ((1707, 1067), "2560×1600 laptop at 150%"),
    ((1920, 1080), "FHD monitor / 4K TV at 200%"),
    ((1920, 1200), "The app's design size"),
    ((2560, 1440), "QHD monitor / 4K at 150%"),
    ((3840, 2160), "4K at 100%"),
]

AppTheme.install()
THEME = AppTheme.THEME


#Musical Cadence Mode without music: made-up songs, each (title, [(seconds, rpm - 0 is free ride)])
VIEWER_SONGS = (("Viewer Band - Warm Up Groove", [(30, 0), (90, 82), (60, 88)]),
                ("The Layouts - Tempo Change", [(70, 96), (50, 72), (60, 104)]),
                ("Sample Artist - Cool Down", [(80, 90), (20, 0), (60, 78)]))


def viewer_music_workout(songs, analyses, preferred=None, minSegmentSeconds=None):
    """Stands in for MusicCadence.build_workout: VIEWER_SONGS as (blocks, tracks, songStarts, pulses)"""
    blocks, tracks, songStarts, pulses = [], [], {}, []
    start = 0
    for index, (title, songBlocks) in enumerate(VIEWER_SONGS):
        duration = sum(seconds for seconds, _ in songBlocks)
        tracks.append((f"viewer-song-{index + 1}.mp3", title, float(start), float(duration)))
        songStarts[len(blocks)] = index
        blockStart = start
        for seconds, rpm in songBlocks:
            blocks.append((seconds, rpm))
            if rpm:
                pulses += [blockStart + k*60/rpm for k in range(int(seconds*rpm/60))]
            blockStart += seconds
        start += duration
    return blocks, tracks, songStarts, pulses


class ViewerSynopsis(SynopsisWindow):
    #the real one re-maximizes and show()s itself a second after it's built, which would pop it up
    #over whichever view is showing here
    def fix_size(self):
        QTimer.singleShot(0, self.layout_max_labels)


class ViewerMusicPicker(MusicSelectWindow):
    """The song picker with made-up songs: nothing analyzed, nothing saved"""
    def __init__(self, parent):
        super().__init__(parent)
        self.clear_songs()
        self.add_to_list([(f"C:/Music/{title}.mp3", title) for title, _ in VIEWER_SONGS] +
                         [("C:/Music/Unchecked Song.mp3", "Some Artist - Unchecked Song"),
                          ("C:/Music/Broken File.mp3", "Broken File")],
                         checked={f"C:/Music/{title}.mp3" for title, _ in VIEWER_SONGS})
        self.missing = ["C:/Music/Not There.mp3"]
        self.update_summary()

    def analyze(self, paths):
        for path in paths:
            self.analyses[path] = ({"error": "can't be read"} if "Broken" in path else
                                   {"duration": 180.0 + 37*(len(self.analyses) % 4)})
        for item in self.items():
            self.label_item(item)

    def save(self):
        pass


class ViewerWorkoutPicker(WorkoutSelectWindow):
    """The workout picker, listing what the app's would (nothing saved)"""
    def save(self):
        pass


def embedded(dialog):
    """A picker dialog as a screen of the display window (its own title shows in the display's title bar)"""
    dialog.setWindowFlags(Qt.WindowType.Widget)
    dialog.title_bar.hide()
    return dialog


class _NoRestart:
    def restart_workout(self): pass


###----------------------------------------------------------------------------------------------
### Display window: stands in for MainWindow
###----------------------------------------------------------------------------------------------
class DisplayWindow(QMainWindow):
    viewChanged = pyqtSignal(str)
    resized = pyqtSignal()
    playingChanged = pyqtSignal(bool)

    def __init__(self, appCss):
        super().__init__()
        self._themePrefixSheet = appCss
        self.startExe = FakeBridge()
        self.workoutHudWidget = _NoRestart() #synopsis RESTART calls into this
        self.views = {}
        self.current = "Init" #a key of views: Free Mode's Workout view is "Free Ride"
        self.mode = "workout" #the setup screen mode the views are built in, see MODES
        self.maximizedMode = True #False while showing a simulated screen size
        #Play: the Workout view runs live from where it's frozen, the rider's readings coming once a second
        self.playing = False
        self.readingTimer = QTimer(self)
        self.readingTimer.setInterval(1000)
        self.readingTimer.timeout.connect(self.play_reading)
        self.resize(800, 600)
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        self.setWindowTitle("   " + APP_TITLE)
        self.setStyleSheet("QMainWindow{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}")
        self.mainLayout = QVBoxLayout()
        #let the window be smaller than its contents need, so a too small screen shows what gets cut off
        self.mainLayout.setSizeConstraint(QLayout.SizeConstraint.SetNoConstraint)
        self.title_bar = CustomTitleBar(self)
        self.title_bar.setFixedHeight(36)
        self.mainLayout.addWidget(self.title_bar)
        central = QWidget()
        central.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Ignored)
        central.setLayout(self.mainLayout)
        self.setCentralWidget(central)

    def discard(self, name):
        if name in ("Workout", "Free Ride"):
            self.pause()
        view = self.views.pop(name, None)
        if view is None:
            return
        for timerName in ("timer", "blinkTimer", "smoothBarTimer", "selectionTimer", "starTimer"):
            if hasattr(view, timerName):
                getattr(view, timerName).stop()
        self.mainLayout.removeWidget(view)
        view.hide()
        view.deleteLater()

    def set_battery_warnings(self, show):
        self.startExe.batteryWarnings = show
        self.startExe.batteryChanged.emit()

    #the synopsis' Apply FTP (an FTP test) sets it on the setup screen, like MainWindow's
    @property
    def setupWidget(self):
        return self.views.get("Init")

    def view_key(self, name):
        """The views key (and layout) for a view button: Free Mode's Workout is the Free Ride screen"""
        if name in ("Workout", "Free Ride"):
            #(either way: current stays "Free Ride" when the views get rebuilt in another mode)
            return "Free Ride" if self.mode == "free" else "Workout"
        return name

    def build(self, mode, workoutPath, progressPercent):
        """Builds every view in this mode (Workout Mode with this workout file); the synopsis rides the whole
        workout (Free Mode: FREE_RIDE_SECONDS)"""
        for name in list(self.views):
            self.discard(name)
        old = self.startExe
        self.startExe = FakeBridge(old.batteryWarnings, old.trainer)
        old.deleteLater()

        reset_ride_state()
        self.mode = mode
        AppState.workoutMode = True
        AppState.cadenceMode = False
        AppState.ftpTestMode = None
        AppState.workoutFilename = AppState.workoutFilenameShort = ""
        ClearSelectedDeviceLines()
        WriteSelectedDeviceLines(self.startExe.devices)

        ##Init: connected, settings entered, workout loaded, ready for START
        setup = WorkoutSetupWindow(self)
        if AppState.wheelCircumference == "0":
            AppState.wheelCircumference = "2.105"
        if AppState.ftp == "0":
            AppState.ftp = "250"
        if AppState.maxHr == "0":
            AppState.maxHr = "185"
        setup.get_ftp_ranges()
        if AppState.powerDriveCutoff == 0: #none saved, and no saved FTP to default it from
            setup.default_power_drive_cutoff()
        setup.update_max_watts_mode()
        setup.displayWheelC.setText(AppState.wheelCircumference + "m")
        setup.displayFtp.setText(AppState.ftp + " Watts")
        setup.displayMaxHr.setText(AppState.maxHr + " BPM")
        setup.init_ant_scan_connect()
        if setup.process_workout_file(workoutPath.replace("\\", "/")):
            AppState.workoutFilename = workoutPath.replace("\\", "/")
            AppState.workoutFilenameShort = _os.path.basename(workoutPath)
            setup.workoutFileSelected.setText(" " + AppState.workout_display_name() + " ")
            setup.workoutDisplayLabel.show()

        ##Pickers: as Workout Mode opens them (the workout picker), and Musical Cadence Mode (the song picker)
        self.views["Workout Picker"] = embedded(ViewerWorkoutPicker(setup))
        self.views["Music Picker"] = embedded(ViewerMusicPicker(setup))

        ##the mode, picked on the setup screen like the app's own mode buttons
        if mode != "workout":
            button = next(b for b, m in setup.modeButtons if m == mode)
            if mode == "cadence":
                setup.musicSelection = {"songs": [], "analyses": {}, "preferred": MusicCadence.DEFAULT_PREFERRED_RPM,
                                        "minSegment": MusicCadence.DEFAULT_MIN_SEGMENT_SECONDS}
                buildWorkout, MusicCadence.build_workout = MusicCadence.build_workout, viewer_music_workout
                try:
                    button.click()
                finally:
                    MusicCadence.build_workout = buildWorkout
            else:
                button.click()
        setup.check_enable_start_button()
        setup.timer.stop()
        setup.blinkTimer.stop()
        #settle every blinking control in its normal (not blinked) look
        for name in [n for n in vars(setup) if n.endswith("BlinkTgl")]:
            setattr(setup, name, True)
        setup.setup_blink()
        #its own event filter keeps this column as tall as the settings column, leave that be
        setup.initializationWidget._layoutSkipHeight = True
        self.views["Init"] = setup

        ##Synopsis: the whole workout ridden start to finish (built from a headless HUD run)
        AppState.antHRM, AppState.antSpeedCadence, AppState.antCadence, AppState.antSpeed = True, True, False, False
        startTimestamp = datetime.datetime.now() - datetime.timedelta(hours=2)
        startSeconds = startTimestamp.timestamp()
        speedState = {"revs": 0.0}
        def record(t, power, hr, cadence, speed):
            ts = startSeconds + t
            WorkoutData.power.times.append(ts)
            WorkoutData.power.columns[0].append(power)
            WorkoutData.heartRate.times.append(ts)
            WorkoutData.heartRate.columns[0].append(hr)
            WorkoutData.fallbackCadence.times.append(ts)
            WorkoutData.fallbackCadence.columns[0].append(int(round(cadence)))
            #speed sensor counters: cumulative revolutions and the 1/1024s time of the last one
            revsPerSecond = speed/2.23694/float(AppState.wheelCircumference)
            speedState["revs"] += revsPerSecond
            revs = speedState["revs"]
            lastRevTime = t - (revs - int(revs))/max(revsPerSecond, 0.01)
            WorkoutData.speed.times.append(ts)
            WorkoutData.speed.columns[0].append(int(revs) & 0xFFFF)
            WorkoutData.speed.columns[1].append(int(lastRevTime*1024) & 0xFFFF)
        AppState.workoutMode = mode != "free"
        simHud = HUDWindow(self)
        run_hud(simHud, AppState.workoutDurationSeconds + 1 if AppState.workoutMode else FREE_RIDE_SECONDS,
                Rider(seed=11), record)
        simHud.timer.stop()
        simHud.deleteLater()
        AppState.startTimestamp = startTimestamp
        AppState.workoutPowerRequestsSysTime[:] = [startTimestamp + datetime.timedelta(seconds=s)
                                                   for s in AppState.workoutPowerRequestsSeconds]
        self.views["Synopsis"] = ViewerSynopsis(self)
        for view in self.views.values():
            self.mainLayout.addWidget(view)
            view.hide()
        self.build_workout(progressPercent)

    def build_workout(self, progressPercent):
        """(Re)builds the Workout view (Free Ride in Free Mode), frozen progressPercent of the way through the ride"""
        workoutMode = self.mode != "free"
        seconds = (AppState.workoutDurationSeconds if workoutMode else FREE_RIDE_SECONDS)*progressPercent/100
        for name in ("Workout", "Free Ride"):
            self.discard(name)
        reset_ride_state()
        AppState.startTimestamp = datetime.datetime.now()
        AppState.workoutMode = workoutMode
        hud = HUDWindow(self)
        run_hud(hud, seconds, Rider(seed=3 if workoutMode else 5))
        name = self.view_key("Workout")
        self.views[name] = hud
        self.mainLayout.addWidget(hud)
        hud.hide()
        self.show_view(self.current)

    def show_view(self, name):
        name = self.view_key(name)
        if name != self.current:
            self.pause()
        self.current = name
        #hide the others before showing this one: for a moment both in the layout would add their
        #minimum heights together, pushing the window taller than the screen and out of maximized
        for viewName, view in self.views.items():
            if viewName != name:
                view.hide()
        self.views[name].show()
        #same deferred re-maximize MainWindow uses after swapping screens (frameless window quirk)
        if self.maximizedMode and self.isVisible() and not self.isMaximized():
            QTimer.singleShot(0, self.showMaximized)
        if name in ("Music Picker", "Workout Picker"):
            title = "   " + self.views[name].windowTitle().strip()
        elif name == "Workout" and getattr(self.views[name], "songTitle", None):
            #Musical Cadence Mode: the song playing (see HUDWindow.update_song_title)
            title = "   " + self.views[name].songTitle
        elif name in ("Workout", "Synopsis") and self.mode != "free" and AppState.workout_display_name():
            title = f"   {APP_TITLE}: {AppState.workout_display_name()}"
        else:
            title = "   " + APP_TITLE
        self.setWindowTitle(title)
        self.title_bar.set_title(title)
        if name == "Synopsis":
            QTimer.singleShot(0, self.views[name].layout_max_labels)
        self.viewChanged.emit(name)

    def required_size(self, name):
        """Smallest window the view fits in without anything squeezed below its minimum"""
        view = self.views[name]
        if not view.isVisible():
            #Qt doesn't refresh a hidden screen's cached layout sizes when its contents change, so a hidden
            #view could report far more (or less) room than it really needs
            for layout in [view.layout()] + view.findChildren(QLayout):
                layout.invalidate()
        margins = self.mainLayout.contentsMargins()
        viewMin = view.minimumSizeHint()
        width = max(viewMin.width(), self.title_bar.minimumSizeHint().width()) + margins.left() + margins.right()
        height = (self.title_bar.minimumHeight() + max(self.mainLayout.spacing(), 0) + viewMin.height()
                  + margins.top() + margins.bottom())
        return width, height

    def workout_hud(self):
        return self.views.get(self.view_key("Workout"))

    def play(self):
        """Runs the Workout view live from where it is: its own timers, the rider's readings, the songs' clock"""
        hud = self.workout_hud()
        if hud is None or self.playing:
            return
        self.show_view("Workout")
        if hud.cadenceMode:
            if hud.musicPlayer is None: #(leaving the view stops the music, see HUDWindow.hideEvent)
                hud.musicPlayer = ViewerMusicPlayer(hud.workoutSecondsPassed)
            hud.musicPlayer.run_clock(True)
        hud.lastTimestamp = time.monotonic()
        hud.timer.start()
        if hasattr(hud, "smoothBarTimer"):
            hud.smoothBarTimer.start()
        self.readingTimer.start()
        self.playing = True
        self.playingChanged.emit(True)

    def pause(self):
        """Freezes the Workout view where it's got to"""
        if not self.playing:
            return
        self.playing = False
        self.readingTimer.stop()
        hud = self.workout_hud()
        if hud is not None:
            hud.timer.stop()
            if hasattr(hud, "smoothBarTimer"):
                hud.smoothBarTimer.stop()
            if hud.cadenceMode and hud.musicPlayer is not None:
                hud.musicPlayer.run_clock(False)
        self.playingChanged.emit(False)

    def play_reading(self):
        """A second's reading from the rider while playing, as AntDevicesPoller would give it"""
        hud = self.workout_hud()
        if hud is None:
            return self.pause()
        reading = hud.viewerRider.sample(AppState.secondsPassed, hud.currentPower, hud.cadenceMode)
        if hud.workoutUnpausedBool or not AppState.workoutMode: #(the HUD's own pause holds the ride)
            hud.viewerRevolutions += reading[3]/2.23694/float(AppState.wheelCircumference)
            save_reading(AppState.secondsPassed, reading)
        show_reading(hud, reading)

    #the screens' own buttons call these on MainWindow
    def generate_hud(self): self.show_view("Workout")
    def generate_synopsis(self): self.show_view("Synopsis")
    def go_back_to_setup(self): self.show_view("Init")

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.resized.emit()

    def closeEvent(self, event):
        QApplication.quit()


class PreviewWindow(QLabel):
    """Scaled down picture of the display window, for simulated screens bigger than this one"""
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Layout Viewer Preview (scaled)")
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setStyleSheet("background:#202020;")

    def refresh(self, display):
        picture = display.grab()
        available = self.screen().availableGeometry()
        target = picture.size().scaled(int(available.width()*0.95), int(available.height()*0.9),
                                       Qt.AspectRatioMode.KeepAspectRatio)
        AppTheme.original_set_stylesheet(self, "background:#202020;")
        self.setPixmap(picture.scaled(target, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation))
        self.setWindowTitle(f"Layout Viewer Preview: {picture.width()}×{picture.height()} shown at "
                            f"{round(100*target.width()/picture.width())}%")
        if not self.isVisible():
            self.resize(target)
            self.move(available.topLeft())
            self.show()


###----------------------------------------------------------------------------------------------
### Colors: which a widget uses, gradients kept together
###----------------------------------------------------------------------------------------------
GRADIENT_RE = re.compile(r"q(?:linear|radial|conical)gradient\([^)]*\)", re.I)


def split_gradients(css):
    """(the css without its gradients, {tuple of each gradient's colors in order})"""
    gradients = set()
    for match in GRADIENT_RE.finditer(css):
        colors = tuple(("#" + m.group(1)[-6:]).lower() for m in AppTheme.HEX_RE.finditer(match.group(0)))
        if len(set(colors)) > 1:
            gradients.add(colors)
        elif colors: #"from one color to the same one" is just that color
            css += f" {colors[0]}"
    return GRADIENT_RE.sub("", css), gradients


def widget_colors(widget):
    """(single colors, gradients) the widget's own styling uses"""
    rest, gradients = split_gradients(getattr(widget, "_themeOrigSheet", ""))
    singles = set(colors_in_css(rest))
    for color in tuple(getattr(widget, "_themeOrigBar", ())) + tuple(getattr(widget, "_themeOrigLine", ())):
        singles |= colors_in_value(color)
    singles |= {c.lower() for c in getattr(widget, "_themeColors", ())}
    if getattr(widget, "_ftpColored", False):
        #the power zone colors are on the FTP Colors tab instead
        zoneColors = FtpColors.all_colors()
        singles -= zoneColors
        gradients = {g for g in gradients if not set(g) & zoneColors}
    if type(widget).__name__ == "SynopsisWindow" or widget.objectName() == "synopsisGraphFrame":
        singles.add(AppTheme.PLOT_LABEL_BG)
    return singles, gradients


class GradientPreview(QWidget):
    """The gradient as it looks with the theme's colors; clicking it shows where it's used"""
    def __init__(self, colors, onClick):
        super().__init__()
        self.colors = colors
        self.onClick = onClick
        self.highlight = False
        self.setMinimumSize(110, 24)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        gradient = QLinearGradient(rect.topLeft(), rect.topRight())
        for i, color in enumerate(self.colors):
            gradient.setColorAt(i/(len(self.colors) - 1), QColor(THEME.map_hex(color)))
        painter.setBrush(gradient)
        painter.setPen(QPen(QColor("#ffeb3b"), 4) if self.highlight else QPen(QColor("#444444")))
        painter.drawRoundedRect(rect.adjusted(1.5, 1.5, -1.5, -1.5) if self.highlight else rect, 4, 4)

    def set_highlight(self, on):
        self.highlight = on
        self.update()

    def mousePressEvent(self, event):
        self.onClick()


class ClickableLabel(QLabel):
    def __init__(self, text, onClick):
        super().__init__(text)
        self.onClick = onClick
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def mousePressEvent(self, event):
        self.onClick()


class OutlineFlash(QWidget):
    """A blinking outline over part of the display window, to show what was picked"""
    def __init__(self, display, widget, color="#ff1744"):
        super().__init__(display)
        if widget is not display and not display.isAncestorOf(widget):
            #only parts of the display can be outlined (mapTo() crashes on anything else)
            self.deleteLater()
            return
        self._layoutExempt = True
        self.color = color
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        topLeft = widget.mapTo(display, QPoint(0, 0))
        self.setGeometry(QRect(topLeft, widget.size()).adjusted(-3, -3, 3, 3))
        self.blinks = 0
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.blink)
        self.timer.start(160)
        self.raise_()
        self.show()

    def blink(self):
        self.blinks += 1
        if self.blinks >= 8:
            self.deleteLater()
            return
        self.setVisible(not self.isVisible())

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setPen(QPen(QColor(self.color), 4))
        painter.drawRect(QRectF(self.rect()).adjusted(2, 2, -2, -2))


def blink_widgets(widgets, times=6, interval=160):
    """Flashes a yellow highlight on control rows (the controls for what was clicked)"""
    state = {"n": 0}
    timer = QTimer(widgets[0])
    def step():
        state["n"] += 1
        on = state["n"] % 2 == 1 and state["n"] < times
        for widget in widgets:
            if hasattr(widget, "set_highlight"):
                widget.set_highlight(on)
                continue
            #through the palette: a stylesheet would also swap in the app wide (theme's) font
            if not hasattr(widget, "_blinkPalette"):
                widget._blinkPalette = (QPalette(widget.palette()), widget.autoFillBackground())
            palette, autoFill = widget._blinkPalette
            if on:
                highlighted = QPalette(palette)
                highlighted.setColor(QPalette.ColorRole.Window, QColor("#ffeb3b"))
                widget.setPalette(highlighted)
                widget.setAutoFillBackground(True)
            else:
                widget.setPalette(palette)
                widget.setAutoFillBackground(autoFill)
        if state["n"] >= times:
            timer.stop()
            timer.deleteLater()
    timer.timeout.connect(step)
    timer.start(interval)
    step()


###----------------------------------------------------------------------------------------------
### Control window
###----------------------------------------------------------------------------------------------
class ControlWindow(QWidget):
    def __init__(self, display, appCss, families):
        super().__init__()
        self.display = display
        self.appCss = appCss
        self.colorRows = {}     #color -> (swatch, name label, uses label, reset)
        self.gradientRows = {}  #gradient colors -> preview
        #every color (and gradient) seen in any view, in any mode, this session: {color: where it's used}
        self.knownColors, self.knownGradients = {}, {}
        self.preview = PreviewWindow()
        #the screen the display window is shown on (it's parked off it for oversized screens)
        self.homeScreen = display.screen() #see home_screen()
        self.monitorTimer = QTimer(self, singleShot=True, interval=500) #see display_screen_changed()
        self.monitorTimer.timeout.connect(self.monitor_changed)
        #the layout scale never applies to the controls or the preview
        self._layoutExempt = self.preview._layoutExempt = True
        self.setWindowTitle("Layout Viewer Controls")
        #the app wide font is the theme's; keep the controls in the plain system font
        self.setFont(AppState.defaultFont)
        #Qt 6.7's windows11 style draws disabled text the same black as enabled, so grey it here
        palette = self.palette()
        for role in (QPalette.ColorRole.WindowText, QPalette.ColorRole.Text, QPalette.ColorRole.ButtonText):
            palette.setColor(QPalette.ColorGroup.Disabled, role, QColor("#a0a0a0"))
        self.setPalette(palette)
        self.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
        self.restyleTimer = QTimer(self)
        self.restyleTimer.setSingleShot(True)
        self.restyleTimer.setInterval(60)
        self.restyleTimer.timeout.connect(self.apply_theme)

        outer = QVBoxLayout(self)

        ##View
        viewBox = QGroupBox("View  (keys 1 – 5)")
        viewLayout = QHBoxLayout(viewBox)
        self.viewButtons = QButtonGroup(self)
        for i, name in enumerate(VIEWS):
            radio = QRadioButton(name)
            radio.setChecked(name == display.current)
            self.viewButtons.addButton(radio, i)
            viewLayout.addWidget(radio)
            for target in (self, display, self.preview):
                QShortcut(QKeySequence(str(i + 1)), target, activated=lambda n=name: self.display.show_view(n))
        self.viewButtons.idClicked.connect(lambda i: display.show_view(VIEWS[i]))
        self.playButton = QPushButton("▶ Play")
        self.playButton.setCheckable(True)
        self.playButton.setToolTip("Runs the Workout view live from where it is (pauses on another view or a rebuild)")
        self.playButton.toggled.connect(lambda on: display.play() if on else display.pause())
        display.playingChanged.connect(self.playing_changed)
        viewLayout.addWidget(self.playButton)
        display.viewChanged.connect(self.view_changed)
        display.resized.connect(lambda: QTimer.singleShot(100, self.update_fit_report))
        outer.addWidget(viewBox)

        ##Fit report, always in view
        self.fitLabel = QLabel()
        self.fitLabel.setTextFormat(Qt.TextFormat.RichText)
        self.fitLabel.setWordWrap(True)
        outer.addWidget(self.fitLabel)

        tabs = self.tabs = QTabWidget()
        outer.addWidget(tabs, 1)
        tabs.addTab(self.build_screen_tab(), "Screen")
        self.fontsTab = self.build_fonts_tab(families)
        tabs.addTab(self.fontsTab, "Fonts")
        self.colorsTab = self.build_colors_tab()
        tabs.addTab(self.colorsTab, "Colors")
        tabs.addTab(self.build_scenario_tab(), "Scenario")
        #the power zone colors: edited here, shown on their own mockup window while the tab is open
        self.ftpTab = FtpColorsTab()
        self.ftpPage = QWidget()
        ftpLayout = QVBoxLayout(self.ftpPage)
        ftpLayout.setContentsMargins(0, 0, 0, 0)
        ftpLayout.addLayout(self.part_buttons("ftpColors", "gradients"))
        ftpLayout.addWidget(self.ftpTab, 1)
        tabs.addTab(self.ftpPage, "FTP Colors")
        self.ftpMockup = None
        tabs.currentChanged.connect(self.tab_changed)

        ##Theme file: fonts and colors, everything the app loads from its ⚙ menu
        buttons = QHBoxLayout()
        for text, slot, tip in (("Save theme…", self.save_theme, "Fonts, colors and power/cadence gradients, for the app's ⚙ menu"),
                                ("Save as default", self.save_default_theme,
                                 f"Save as {AppTheme.DEFAULT_THEME_FILE}, which the app uses whenever no theme is picked"),
                                ("Load theme…", self.load_theme, ""),
                                ("Reset all", self.reset_theme, "Back to the app's own fonts and colors")):
            button = QPushButton(text)
            button.clicked.connect(slot)
            button.setToolTip(tip)
            buttons.addWidget(button)
        outer.addLayout(buttons)

        self.statusLabel = QLabel()
        self.statusLabel.setWordWrap(True)
        outer.addWidget(self.statusLabel)

        self.sync_font_controls()
        #on the Fonts and Colors tabs, clicking the display identifies what's clicked (see eventFilter)
        QApplication.instance().installEventFilter(self)

    ##Tabs
    def tab_changed(self, index):
        if self.tabs.widget(index) is self.ftpPage:
            if self.ftpMockup is None:
                self.ftpMockup = FtpMockupWindow()
                self.ftpMockup.resize(1000, 1000)
                #beside the controls, on their screen
                screen = self.screen().availableGeometry()
                x = self.frameGeometry().right() + 10
                if x + 1000 > screen.right():
                    x = max(screen.left(), self.frameGeometry().left() - 1010)
                self.ftpMockup.move(x, screen.top() + 30)
            self.ftpMockup.show()
            self.ftpMockup.raise_()
        elif self.ftpMockup is not None:
            self.ftpMockup.hide()
    def build_screen_tab(self):
        tab = QWidget()
        form = QFormLayout(tab)
        self.screenCombo = QComboBox()
        #item data is "WxH" ("screen" for this one): Qt doesn't hand Python tuples back intact
        for size, note in SCREEN_SIZES:
            self.screenCombo.addItem(note if size is None else f"{size[0]}×{size[1]}  –  {note}",
                                     "screen" if size is None else f"{size[0]}x{size[1]}")
        self.screenCombo.addItem("Custom", "custom")
        form.addRow("Screen", self.screenCombo)
        #in its own widget so the whole row (label included) can be greyed out when Custom isn't picked
        customWidget = QWidget()
        customRow = QHBoxLayout(customWidget)
        customRow.setContentsMargins(0, 0, 0, 0)
        self.customW, self.customH = QSpinBox(), QSpinBox()
        for spin, value in ((self.customW, 1280), (self.customH, 720)):
            spin.setRange(640, 7680)
            spin.setValue(value)
            spin.setMinimumWidth(95)
            spin.valueChanged.connect(lambda: self.screenCombo.currentData() == "custom" and self.apply_screen())
            customRow.addWidget(spin)
        customRow.addWidget(QLabel("(effective size)"))
        customRow.addStretch()
        form.addRow("Custom", customWidget)
        customLabel = form.labelForField(customWidget)
        def update_custom_row():
            custom = self.screenCombo.currentData() == "custom"
            customWidget.setEnabled(custom)
            customLabel.setEnabled(custom)
        self.screenCombo.currentIndexChanged.connect(update_custom_row)
        update_custom_row()
        self.taskbarCheck = QCheckBox(f"Leave room for the taskbar ({TASKBAR_HEIGHT}px)")
        self.taskbarCheck.setChecked(True)
        form.addRow(self.taskbarCheck)
        self.scaleNote = QLabel()
        self.scaleNote.setWordWrap(True)
        form.addRow(self.scaleNote)
        self.screenCombo.currentIndexChanged.connect(self.apply_screen)
        self.taskbarCheck.toggled.connect(self.apply_screen)

        shots = QHBoxLayout()
        for text, slot, tip in (("Screenshot", self.screenshot, "This view at this size, into layoutScreenshots/"),
                                ("Capture all sizes", self.capture_all,
                                 "Every view at every listed size, plus fit_report.txt, into layoutScreenshots/")):
            button = QPushButton(text)
            button.setToolTip(tip)
            button.clicked.connect(slot)
            shots.addWidget(button)
        form.addRow(shots)
        note = QLabel("The app scales itself to whatever screen it's on (every screen fits, all text fits); "
                      "pick a size to see how the fonts and colors look there.")
        note.setWordWrap(True)
        form.addRow(note)
        return tab

    def build_fonts_tab(self, families):
        tab = QWidget()
        outer = QVBoxLayout(tab)
        tip = QLabel("While this tab is open, click anything in the display: it's outlined, and the font row "
                     "that styles it blinks below.")
        tip.setWordWrap(True)
        outer.addWidget(tip)
        fontBox = QGroupBox("Fonts")
        fontGrid = QGridLayout(fontBox)
        fontGrid.addWidget(QLabel("Family"), 0, 1)
        fontGrid.addWidget(QLabel("Size"), 0, 2)
        fontGrid.addWidget(QLabel("Weight"), 0, 3)
        self.fontControls = {}
        self.fontRoleLabels = {}
        systemFamilies = [f for f in QFontDatabase.families() if f not in families]
        for row, role in enumerate(ROLE_TITLES, start=1):
            roleLabel = QLabel(ROLE_TITLES[role])
            self.fontRoleLabels[role] = roleLabel
            fontGrid.addWidget(roleLabel, row*2 - 1, 0, 1, 4)
            combo = QComboBox()
            combo.setMaxVisibleItems(25)
            for family in families:
                combo.addItem(family)
                combo.setItemData(combo.count() - 1, QFont(family, 11), Qt.ItemDataRole.FontRole)
            combo.insertSeparator(combo.count())
            combo.addItems(systemFamilies)
            size = QSpinBox()
            size.setRange(30, 300)
            size.setSingleStep(5)
            size.setSuffix("%")
            size.setMinimumWidth(95)
            weight = QComboBox()
            weight.setMinimumWidth(80)
            weight.addItems(WEIGHTS)
            weight.setEnabled(role != ROLE_GAUGE) #the gauges always draw bold
            fontGrid.addWidget(combo, row*2, 1)
            fontGrid.addWidget(size, row*2, 2)
            fontGrid.addWidget(weight, row*2, 3)
            combo.currentTextChanged.connect(lambda v, r=role: self.font_changed(r, "family", v))
            size.valueChanged.connect(lambda v, r=role: self.font_changed(r, "scale", v))
            weight.currentTextChanged.connect(lambda v, r=role: self.font_changed(r, "weight", v))
            self.fontControls[role] = (combo, size, weight)
        fontGrid.setColumnStretch(1, 1)
        outer.addWidget(fontBox)
        outer.addStretch(1)
        return tab

    def build_colors_tab(self):
        tab = QWidget()
        outer = QVBoxLayout(tab)
        tip = QLabel("Every color the app uses, once each: changing one changes it everywhere, gradients included "
                     "(the power zone colors are on the FTP Colors tab). Click a color's name or a gradient to see "
                     "where it's used in this view; click anything in the display to blink its colors here.")
        tip.setWordWrap(True)
        outer.addWidget(tip)
        self.colorListWidget = QWidget()
        self.colorList = QGridLayout(self.colorListWidget)
        self.colorList.setContentsMargins(2, 2, 2, 2)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self.colorListWidget)
        self.colorScroll = scroll
        outer.addWidget(scroll, 1)
        outer.addLayout(self.part_buttons("colors", "colors"))
        return tab

    def part_buttons(self, part, name):
        """Save/Load buttons for just one part of the theme (its colors or its power/cadence gradients)"""
        row = QHBoxLayout()
        what = AppTheme.PARTS[part]
        for text, slot, tip in ((f"Save {name}…", lambda: self.save_part(part),
                                 f"Just the {what}, as a file of their own (the app loads it from its ⚙ menu, on top "
                                 "of whichever theme it uses)"),
                                (f"Load {name}…", lambda: self.load_part(part), f"Replaces just the {what}")):
            button = QPushButton(text)
            button.setToolTip(tip)
            button.clicked.connect(slot)
            row.addWidget(button)
        row.addStretch()
        return row

    def build_scenario_tab(self):
        tab = QWidget()
        form = QFormLayout(tab)
        self.modeCombo = QComboBox()
        for mode, name in MODES:
            self.modeCombo.addItem(name, mode)
        self.modeCombo.setToolTip("The setup screen's mode every view is built in")
        form.addRow("Mode", self.modeCombo)
        self.workoutCombo = QComboBox()
        workouts = sorted(f for f in _os.listdir(WORKOUT_DIR) if f.endswith(".awo")) if _os.path.isdir(WORKOUT_DIR) else []
        self.workoutCombo.addItems(workouts)
        if DEFAULT_WORKOUT in workouts:
            self.workoutCombo.setCurrentText(DEFAULT_WORKOUT)
        form.addRow("Workout file", self.workoutCombo)
        progressRow = QHBoxLayout()
        self.progressSlider = QSlider(Qt.Orientation.Horizontal)
        self.progressSlider.setRange(1, 99)
        self.progressSlider.setValue(40)
        self.progressLabel = QLabel("40%")
        self.progressLabel.setMinimumWidth(36)
        self.progressSlider.valueChanged.connect(lambda v: self.progressLabel.setText(f"{v}%"))
        progressRow.addWidget(self.progressSlider)
        progressRow.addWidget(self.progressLabel)
        form.addRow("Workout progress", progressRow)
        self.batteryCheck = QCheckBox("Show low battery warnings")
        form.addRow(self.batteryCheck)
        self.trainerCheck = QCheckBox("Power device is an FE-C trainer (Max Watts/Power Drive Cutoff box, Free Ride Watts slider)")
        form.addRow(self.trainerCheck)
        self.messageCheck = QCheckBox("Show an example workout message (under Ride At)")
        self.messageCheck.setToolTip("What an .awo file's timed |message| lines look like during a workout")
        self.messageCheck.toggled.connect(self.apply_example_message)
        form.addRow(self.messageCheck)
        self.keysCheck = QCheckBox("Show key bindings (the labels holding Ctrl shows in the app)")
        self.keysCheck.setToolTip("The keyboard keys for the buttons on the view showing, see KeyShortcuts.py")
        self.keysCheck.toggled.connect(self.keys_toggled)
        form.addRow(self.keysCheck)
        #(kept up to date while shown: buttons clicked here change which keys apply)
        self.keyLabelTimer = QTimer(self)
        self.keyLabelTimer.setInterval(500)
        self.keyLabelTimer.timeout.connect(self.update_key_labels)
        rebuildButton = QPushButton("Rebuild views")
        rebuildButton.setToolTip("Puts the screens back as they were built, e.g. after clicking their buttons")
        rebuildButton.clicked.connect(self.rebuild)
        form.addRow(rebuildButton)
        self.workoutCombo.currentTextChanged.connect(self.rebuild)
        self.modeCombo.currentIndexChanged.connect(self.mode_changed)
        self.progressSlider.sliderReleased.connect(self.rebuild_workout)
        self.progressSlider.valueChanged.connect(lambda: self.progressSlider.isSliderDown() or self.rebuild_workout())
        self.batteryCheck.toggled.connect(self.display.set_battery_warnings)
        self.trainerCheck.toggled.connect(self.trainer_changed)
        return tab

    ##View
    def playing_changed(self, playing):
        self.playButton.blockSignals(True)
        self.playButton.setChecked(playing)
        self.playButton.blockSignals(False)
        self.playButton.setText("❚❚ Pause" if playing else "▶ Play")

    def view_changed(self, name):
        self.viewButtons.button(VIEWS.index("Workout" if name == "Free Ride" else name)).setChecked(True)
        self.refresh_color_list()
        self.update_fit_report()
        #once the newly shown view has laid itself out: its key labels, then the preview
        QTimer.singleShot(250, self.update_key_labels)
        if self.is_oversize():
            QTimer.singleShot(300, lambda: self.preview.refresh(self.display))

    def rebuild(self):
        path = _os.path.join(WORKOUT_DIR, self.workoutCombo.currentText())
        self.busy(lambda: self.display.build(self.modeCombo.currentData(), path, self.progressSlider.value()), full=True)

    def mode_changed(self):
        #(the workout file is only Workout Mode's)
        self.workoutCombo.setEnabled(self.modeCombo.currentData() == "workout")
        self.rebuild()

    def rebuild_workout(self):
        self.busy(lambda: self.display.build_workout(self.progressSlider.value()))

    EXAMPLE_MESSAGE = "Stand up for the last 30 seconds and keep your cadence above 90!"

    def apply_example_message(self):
        """The Workout view's message line, as a timed .awo message would show it (or empty)"""
        hud = self.display.views.get("Workout")
        if hud is not None:
            hud.set_workout_message(self.EXAMPLE_MESSAGE if self.messageCheck.isChecked() else "")
            if self.is_oversize():
                QTimer.singleShot(300, lambda: self.preview.refresh(self.display))

    def keys_toggled(self, show):
        if show:
            self.keyLabelTimer.start()
            self.update_key_labels()
        else:
            self.keyLabelTimer.stop()
            KeyShortcuts.unpin_labels()
        if self.is_oversize():
            QTimer.singleShot(100, lambda: self.preview.refresh(self.display))

    def update_key_labels(self):
        if self.keysCheck.isChecked() and self.display.views:
            KeyShortcuts.pin_labels(self.display)

    def trainer_changed(self, trainer):
        self.display.startExe.trainer = trainer
        self.rebuild() #the Init view's power device changes too

    def busy(self, work, full=False):
        #one build at a time: a rebuild asked for during one (e.g. the progress slider moved again while the
        #status shows) waits, and only the latest waiting one runs, once this is done; a waiting full rebuild
        #(every view, so the Workout view too) isn't replaced by a Workout view one. Run inside this one, a
        #second build would discard views this one is still building on (a doubled screen, or a crash)
        if getattr(self, "busyNext", None) is not None:
            if full or not getattr(self, "busyNextFull", False) or self.busyNext is False:
                self.busyNext, self.busyNextFull = work, full
            return
        self.busyNext, self.busyNextFull = work, full
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        self.statusLabel.setText("Building…")
        try:
            while self.busyNext is not False:
                QApplication.processEvents() #(shows the status; may ask for another build, see above)
                work, self.busyNext, self.busyNextFull = self.busyNext, False, False
                work()
                self.apply_example_message() #the rebuilt Workout view starts without it
                self.apply_layout()
                QApplication.processEvents()
        finally:
            self.busyNext = None
            QApplication.restoreOverrideCursor()
            self.statusLabel.setText("")

    ##Screen size
    def screen_size(self):
        data = self.screenCombo.currentData()
        if data == "custom":
            return (self.customW.value(), self.customH.value())
        if data in (None, "screen"):
            return None
        return tuple(int(n) for n in data.split("x"))

    def available_size(self):
        size = self.screen_size()
        if size is None:
            geometry = self.home_screen().availableGeometry()
            return geometry.width(), geometry.height()
        return size[0], size[1] - (TASKBAR_HEIGHT if self.taskbarCheck.isChecked() else 0)

    def apply_screen(self):
        self.apply_layout()
    def display_screen_changed(self, screen):
        """The display window was moved to another monitor: when showing this screen's own size, that
        monitor's layout and scale (else they'd only catch up at the next edit)"""
        if self.screen_size() is None:
            self.monitorTimer.start()
    def monitor_changed(self):
        #the scale only: the window is where the user (or Windows) put it, and changing its state in
        #the middle of a move to another monitor crashes Qt
        self.apply_layout(place=False)
    def place_display(self):
        """Maximized for this screen, else exactly the simulated size. Only touches the window when
        something has to change: re-showing/re-maximizing it on every apply crashed Qt now and then"""
        display = self.display
        if not display.isVisible():
            #first build at startup: main() shows the window once it's done
            return
        size = self.screen_size()
        screen = self.home_screen().availableGeometry()
        if size is None:
            display.maximizedMode = True
            self.preview.hide()
            if display.isMaximized():
                return
            if not screen.contains(display.geometry().center()):
                #back from off the edge of the desktop onto its monitor, to maximize there
                display.setGeometry(screen.x(), screen.y(), 800, 600)
            display.setWindowState(Qt.WindowState.WindowMaximized)
            return
        display.maximizedMode = False
        width, height = self.available_size()
        if self.is_oversize():
            #bigger than this screen: laid out at full size just past the right edge of the desktop
            #(out of sight), and shown scaled down in the preview window instead
            target = QRect(self.home_screen().virtualGeometry().right() + 100, screen.y(), width, height)
        else:
            target = QRect(screen.x(), screen.y(), width, height)
            self.preview.hide()
        wasMaximized = display.isMaximized()
        if wasMaximized:
            display.showNormal()
        if wasMaximized or display.geometry() != target:
            display.setGeometry(target)
            #again once the window system has settled: leaving maximized, Windows restores the window's
            #old normal size afterwards (even when the maximized size already matched)
            for delay in (50, 250):
                QTimer.singleShot(delay, lambda: display.geometry() != target and not display.maximizedMode
                                  and display.setGeometry(target))

    def home_screen(self):
        """The monitor the display window is on (wherever it's been moved to); while it's parked off
        the edge of the desktop for an oversized screen, the one it was on before"""
        center = self.display.geometry().center()
        for screen in QApplication.screens():
            if screen.geometry().contains(center):
                self.homeScreen = screen
                break
        return self.homeScreen

    def is_oversize(self):
        width, height = self.available_size()
        screen = self.home_screen().availableGeometry()
        return self.screen_size() is not None and (width > screen.width() or height > screen.height())

    ##Scaling to the screen (as the app does)
    def apply_layout(self, place=True):
        """This screen size's scale, then the theme on top"""
        available = self.available_size()
        AppLayout.apply_layout(self.display, self.display.views, AppLayout.scale_for(available), available)
        self.scaleNote.setText(f"Available area {available[0]}×{available[1]}; "
                               f"scaled to {round(AppTheme.ui_scale*100)}%.")
        self.refresh_color_list()
        if place:
            self.place_display()
        QTimer.singleShot(250, self.after_layout)
    def after_layout(self):
        self.update_fit_report()
        self.update_key_labels()
        if self.is_oversize():
            self.preview.refresh(self.display)

    def fit_results(self):
        width, height = self.available_size()
        results = {}
        for name in self.display.views:
            needW, needH = self.display.required_size(name)
            results[name] = (needW, needH, needW <= width and needH <= height)
        return results

    def update_fit_report(self):
        if not self.display.views:
            return
        width, height = self.available_size()
        lines = [f"<b>{width}×{height}</b> available, layout scale {round(AppTheme.ui_scale*100)}%"]
        for name, (needW, needH, fits) in self.fit_results().items():
            current = "▶ " if name == self.display.current else ""
            if fits:
                lines.append(f"{current}<span style='color:#1b7f1b'>{name}: fits</span> (needs {needW}×{needH})")
            else:
                over = []
                if needW > width:
                    over.append(f"{needW - width}px too wide")
                if needH > height:
                    over.append(f"{needH - height}px too tall")
                lines.append(f"{current}<span style='color:#b00020'>{name}: {', '.join(over)}</span> (needs {needW}×{needH})")
        self.fitLabel.setText("<br>".join(lines))

    ##Screenshots
    def screen_tag(self):
        size = self.screen_size()
        if size is None:
            width, height = self.available_size()
            return f"screen_{width}x{height}"
        return f"{size[0]}x{size[1]}"

    def screenshot(self):
        _os.makedirs(SCREENSHOT_DIR, exist_ok=True)
        path = _os.path.join(SCREENSHOT_DIR, f"{self.screen_tag()}_{self.display.current.replace(' ', '')}.png")
        self.update_key_labels()
        self.display.grab().save(path)
        self.statusLabel.setText(f"Saved {_os.path.relpath(path, PROJECT_DIR)}")

    def capture_all(self):
        """Every listed screen size, every view: screenshots plus a fit report"""
        _os.makedirs(SCREENSHOT_DIR, exist_ok=True)
        startIndex, startView = self.screenCombo.currentIndex(), self.display.current
        indexes = [i for i in range(self.screenCombo.count()) if self.screenCombo.itemData(i) not in ("screen", "custom")]
        report = []
        steps = []
        for index in indexes:
            steps.append(("size", index))
            steps += [("view", view) for view in VIEWS]
        def run(position=0):
            if position == len(steps):
                with open(_os.path.join(SCREENSHOT_DIR, "fit_report.txt"), "w", encoding="utf-8") as f:
                    f.write("\n".join(report) + "\n")
                self.screenCombo.setCurrentIndex(startIndex)
                self.display.show_view(startView)
                self.statusLabel.setText(f"Captured {len(indexes)} sizes × {len(VIEWS)} views into layoutScreenshots/")
                return
            kind, value = steps[position]
            if kind == "size":
                self.statusLabel.setText(f"Capturing {self.screenCombo.itemText(value)}…")
                self.screenCombo.setCurrentIndex(value)
                def measure():
                    #once the new size has settled
                    width, height = self.available_size()
                    report.append(f"{self.screenCombo.itemText(value)}  (available {width}×{height}, "
                                  f"scale {round(AppTheme.ui_scale*100)}%)")
                    for name, (needW, needH, fits) in self.fit_results().items():
                        report.append(f"    {name:10} {'fits' if fits else 'DOES NOT FIT'}  (needs {needW}×{needH})")
                    run(position + 1)
                QTimer.singleShot(700, measure)
            else:
                self.display.show_view(value)
                def shoot():
                    self.screenshot()
                    run(position + 1)
                QTimer.singleShot(500, shoot)
        run()

    ##Fonts
    def sync_font_controls(self):
        for role, (combo, size, weight) in self.fontControls.items():
            settings = THEME.fonts[role]
            for control in (combo, size, weight):
                control.blockSignals(True)
            if combo.findText(settings["family"]) < 0:
                combo.addItem(settings["family"])
            combo.setCurrentText(settings["family"])
            size.setValue(int(settings["scale"]))
            weight.setCurrentText(settings["weight"])
            for control in (combo, size, weight):
                control.blockSignals(False)

    def font_changed(self, role, key, value):
        THEME.fonts[role][key] = value
        self.restyleTimer.start()

    ##Colors
    def used_colors(self):
        """({color: views}, {gradient colors: views}) across the app stylesheet, the window and each
        view; a view is "All" for what every view shares"""
        singles, gradients = {}, {}
        def add(widgetSingles, widgetGradients, where):
            for c in widgetSingles:
                singles.setdefault(c, set()).add(where)
            for g in widgetGradients:
                gradients.setdefault(g, set()).add(where)
        rest, appGradients = split_gradients(self.appCss)
        add(colors_in_css(rest), appGradients, "All")
        shared = [self.display] + [w for w in self.display.findChildren(QWidget)
                                   if not any(v is w or v.isAncestorOf(w) for v in self.display.views.values())]
        for widget in shared:
            add(*widget_colors(widget), "All")
        for name, view in self.display.views.items():
            for widget in [view] + view.findChildren(QWidget):
                add(*widget_colors(widget), name)
        return singles, gradients
    def refresh_color_list(self):
        """One row per color the app uses (in any view, in any mode seen this session), then the gradients
        made of them"""
        singles, gradients = self.used_colors()
        for known, found in ((self.knownColors, singles), (self.knownGradients, gradients)):
            for key, where in found.items():
                known.setdefault(key, set()).update(where)
        #a gradient's colors are rows like any other: what they're used for, gradients included
        uses = {}
        for color, where in self.knownColors.items():
            uses.setdefault(color, set()).update(where)
        for colors, where in self.knownGradients.items():
            for color in colors:
                uses.setdefault(color, set()).update(f"{w} gradient" for w in where)
        if set(uses) == set(self.colorRows) and set(self.knownGradients) == set(self.gradientRows):
            for color in uses:
                self.colorRows[color][2].setText(self.uses_text(uses[color]))
                self.update_color_row(color)
            return
        while self.colorList.count():
            item = self.colorList.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        self.colorRows, self.gradientRows = {}, {}
        def hue(c):
            #greys first (no hue), then around the color wheel, dark to light
            return (QColor(c).hslHue(), QColor(c).lightness())
        row = 0
        for color in sorted(uses, key=hue):
            swatch = QPushButton()
            swatch.setFixedSize(64, 24)
            swatch.setToolTip(f"{color}: click to change it (everywhere it's used)")
            swatch.clicked.connect(lambda _, c=color: self.pick_color(c))
            name = ClickableLabel(color, lambda c=color: self.show_uses(color=c))
            name.setToolTip("Click to see where it's used in this view")
            usesLabel = QLabel(self.uses_text(uses[color]))
            usesLabel.setWordWrap(True)
            usesLabel.setStyleSheet("color: #606060;")
            reset = QPushButton("↺")
            reset.setFixedWidth(28)
            reset.setToolTip("Back to the original color")
            reset.clicked.connect(lambda _, c=color: self.set_color(c, None))
            self.colorList.addWidget(swatch, row, 0)
            self.colorList.addWidget(name, row, 1)
            self.colorList.addWidget(usesLabel, row, 2)
            self.colorList.addWidget(reset, row, 3)
            self.colorRows[color] = (swatch, name, usesLabel, reset)
            row += 1
        if self.knownGradients:
            self.colorList.addWidget(QLabel("<b>Gradients</b> (made of the colors above)"), row, 0, 1, 4)
            row += 1
        for colors in sorted(self.knownGradients, key=lambda g: [hue(c) for c in g]):
            preview = GradientPreview(colors, lambda g=colors: self.show_uses(gradient=g))
            preview.setToolTip(" → ".join(colors) + "\nUsed in: " + self.uses_text(self.knownGradients[colors])
                               + "\nClick to see where")
            self.colorList.addWidget(preview, row, 0, 1, 2)
            self.colorList.addWidget(QLabel(" → ".join(colors)), row, 2, 1, 2)
            self.gradientRows[colors] = preview
            row += 1
        for color in self.colorRows:
            self.update_color_row(color)
        self.colorList.setRowStretch(row, 1)
        self.colorList.setColumnStretch(2, 1)

    @staticmethod
    def uses_text(where):
        """"every view" or the views a color is in ("All": what every view shares), and whether gradients use it"""
        views = {w.removesuffix(" gradient") for w in where}
        text = "every view" if "All" in views else ", ".join(sorted(views))
        return text + (" · in gradients" if any(w.endswith(" gradient") for w in where) else "")

    def update_color_row(self, color):
        """Redraws the color's row and the gradients it's in"""
        new = THEME.colors.get(color)
        if color in self.colorRows:
            swatch, name, _, reset = self.colorRows[color]
            AppTheme.original_set_stylesheet(swatch, f"background-color:{new or color}; border:1px solid #444; "
                                                     "border-radius:4px;")
            name.setText(f"{color}  →  {new}" if new else color)
            reset.setEnabled(new is not None)
        for colors, preview in self.gradientRows.items():
            if color in colors:
                preview.update()

    def pick_color(self, color):
        before = THEME.colors.get(color)
        dialog = QColorDialog(QColor(before or color), self)
        dialog.setWindowTitle(f"Replace {color}")
        dialog.setOption(QColorDialog.ColorDialogOption.DontUseNativeDialog)
        dialog.currentColorChanged.connect(lambda c: self.set_color(color, c.name()))
        if not dialog.exec():
            self.set_color(color, before)

    def set_color(self, color, new):
        if new is None or new.lower() == color:
            THEME.colors.pop(color, None)
        else:
            THEME.colors[color] = new.lower()
        self.update_color_row(color)
        self.restyleTimer.start()

    ##Identifying what's what (Fonts and Colors tabs)
    def eventFilter(self, obj, event):
        """While the Fonts or Colors tab is open, a click on the display identifies what's under it
        instead of clicking it"""
        if (event.type() in (QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonRelease, QEvent.Type.MouseButtonDblClick)
                and self.tabs.currentWidget() in (self.fontsTab, self.colorsTab) and isinstance(obj, QWidget)
                and obj.window() is self.display and not getattr(obj, "_layoutExempt", False)):
            if event.type() == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
                #the widget the click went to (widgetAt() would find whatever is on top there, e.g. the
                #controls window overlapping the display)
                self.identify(obj)
            return True
        return super().eventFilter(obj, event)

    def font_role_of(self, widget):
        """Which font row styles the widget's text"""
        chain = [widget]
        while chain[-1].parentWidget() is not None:
            chain.append(chain[-1].parentWidget())
        if any(type(w).__name__ in ("AnalogGaugeWidget", "Gauge") for w in chain[:3]):
            return ROLE_GAUGE
        buttonFamily = THEME.fonts[AppTheme.ROLE_BUTTON]["family"]
        if isinstance(widget, QPushButton) or (widget.font().family() == buttonFamily
                                               and buttonFamily != THEME.fonts[ROLE_MAIN]["family"]):
            return AppTheme.ROLE_BUTTON
        return ROLE_MAIN

    def identify(self, widget):
        """Outlines the clicked part of the display and blinks the font and color rows for it"""
        OutlineFlash(self.display, widget)
        role = self.font_role_of(widget)
        stops = set(self.display.views.values()) | {self.display, self.display.centralWidget()}
        chain = [widget]
        while chain[-1].parentWidget() is not None and chain[-1].parentWidget() not in stops:
            chain.append(chain[-1].parentWidget())
        inGauge = any(type(w).__name__ in ("AnalogGaugeWidget", "Gauge", "RotatingSvgWidget", "BeatingSvgWidget")
                      for w in chain[:3])
        #colors: the widget's own, else the closest part around it (never the whole screen's)
        singles, gradients = set(), set()
        if not inGauge:
            for source in chain[:3]:
                singles, gradients = widget_colors(source)
                if singles or gradients:
                    break
        rows = [self.fontRoleLabels[role]]
        found = []
        for color in sorted(singles):
            if color in self.colorRows:
                rows.append(self.colorRows[color][1])
                found.append(color)
        for colors in gradients:
            if colors in self.gradientRows:
                rows.append(self.gradientRows[colors])
                rows += [self.colorRows[c][1] for c in colors if c in self.colorRows]
                found.append(" → ".join(colors))
        blink_widgets(rows)
        if len(rows) > 1 and self.tabs.currentWidget() is self.colorsTab:
            self.colorScroll.ensureWidgetVisible(rows[1])
        what = widget.objectName() if widget.objectName() and not widget.objectName().startswith("themed") else type(widget).__name__
        if inGauge:
            colorText = "; its colors are drawn by the gauge itself (not themeable)"
        elif found:
            colorText = f"; colors {', '.join(found)}"
        else:
            colorText = "; no colors of its own"
        self.statusLabel.setText(f"Clicked {what}: font row \"{ROLE_TITLES[role]}\"{colorText}")

    def show_uses(self, color=None, gradient=None):
        """Outlines everything in the current view (and the title bar) that uses the color or gradient"""
        view = self.display.views.get(self.display.current)
        widgets = [self.display.title_bar] + self.display.title_bar.findChildren(QWidget)
        if view is not None:
            widgets += [view] + view.findChildren(QWidget)
        hits = []
        for widget in widgets:
            if not widget.isVisible():
                continue
            singles, gradients = widget_colors(widget)
            if (color and (color in singles or any(color in g for g in gradients))) or (gradient and gradient in gradients):
                hits.append(widget)
        #the innermost ones are enough when a whole screen uses it (e.g. a background)
        hits = [w for w in hits if not any(w is not h and w.isAncestorOf(h) for h in hits)] or hits
        for widget in hits[:60]:
            OutlineFlash(self.display, widget, "#00e5ff")
        label = color or " → ".join(gradient)
        self.statusLabel.setText(f"{label}: {len(hits)} part(s) of this view" + ("" if hits else
                                 " (it may be in the app wide stylesheet, used by parts not on this view)"))
    ##Applying the theme
    def apply_theme(self):
        AppTheme.apply()
        self.refresh_color_list()
        QTimer.singleShot(250, self.after_layout)

    ##Theme file
    def save_theme(self):
        suggested = _os.path.join(AppTheme.THEMES_DIR, "theme.json")
        path, _ = QFileDialog.getSaveFileName(self, "Save theme", suggested, "Theme (*.json)")
        if path:
            AppTheme.save_file(path)
            self.statusLabel.setText(f"Saved to {_os.path.basename(path)}  (load it in the app from the ⚙ menu)")

    def save_part(self, part):
        folder = PART_DIRS[part]
        _os.makedirs(folder, exist_ok=True)
        path, _ = QFileDialog.getSaveFileName(self, f"Save {AppTheme.PARTS[part]}", folder, "Theme (*.json)")
        if path:
            AppTheme.save_file(path, part)
            self.statusLabel.setText(f"Saved the {AppTheme.PARTS[part]} to {_os.path.basename(path)}  (load it in the "
                                     "app from the ⚙ menu: it goes on top of whichever theme the app uses)")

    def load_part(self, part):
        folder = PART_DIRS[part] if _os.path.isdir(PART_DIRS[part]) else PROJECT_DIR
        path, _ = QFileDialog.getOpenFileName(self, f"Load {AppTheme.PARTS[part]}", folder, "Theme (*.json)")
        if not path:
            return
        try:
            data = AppTheme._read(path)
            if part not in data:
                raise ValueError(f"it has no {AppTheme.PARTS[part]}")
            kind = AppTheme.load_file(path) if AppTheme.file_kind(data) == part else None
            if kind is None:
                #a whole theme: just this part of it
                THEME.set_part(part, data[part])
                AppTheme.apply()
        except (OSError, ValueError) as e:
            self.statusLabel.setText(f"Couldn't load {_os.path.basename(path)}: {e}")
            return
        self.apply_layout()
        self.statusLabel.setText(f"Loaded the {AppTheme.PARTS[part]} from {_os.path.basename(path)}")

    def save_default_theme(self):
        path = _os.path.join(PROJECT_DIR, AppTheme.DEFAULT_THEME_FILE)
        if _os.path.exists(path) and QMessageBox.question(
                self, "Save as default", f"Replace the current {AppTheme.DEFAULT_THEME_FILE}?") != QMessageBox.StandardButton.Yes:
            return
        AppTheme.save_file(path)
        self.statusLabel.setText(f"Saved as {AppTheme.DEFAULT_THEME_FILE}: the app uses it whenever no theme is picked "
                                 "(and after Clear Theme)")

    def load_theme(self):
        folder = AppTheme.THEMES_DIR if _os.path.isdir(AppTheme.THEMES_DIR) else PROJECT_DIR
        path, _ = QFileDialog.getOpenFileName(self, "Load theme", folder, "Theme (*.json)")
        if path:
            try:
                kind = AppTheme.load_file(path)
            except (OSError, ValueError) as e:
                self.statusLabel.setText(f"Couldn't load {_os.path.basename(path)}: {e}")
                return
            self.sync_font_controls()
            self.apply_layout()
            self.statusLabel.setText(f"Loaded {_os.path.basename(path)}" +
                                     ("" if kind == "theme" else f" (just the {AppTheme.PARTS[kind]})"))
    def reset_theme(self):
        THEME.reset()
        self.sync_font_controls()
        self.apply_layout()
        self.statusLabel.setText("Back to the app's own fonts and colors")
    def closeEvent(self, event):
        if self.ftpMockup is not None:
            self.ftpMockup.close()
        QApplication.quit()


def main():
    app = QApplication(sys.argv)
    families = AppTheme.register_fonts()
    #same as AntWorkout.py: the system font, for buttons (the theme sets the app-wide one)
    AppState.defaultFont = QFont(app.font())
    appCss = AppStyle.APP_STYLESHEET

    #start where the app does with no theme picked
    AppTheme.clear()
    display = DisplayWindow(appCss)
    controls = ControlWindow(display, appCss, families)
    controls.rebuild()

    #display maximized on the main screen; controls on a second screen if there is one
    screens = app.screens()
    display.setScreen(screens[0])
    display.move(screens[0].availableGeometry().topLeft()) #Windows may otherwise open it on another monitor
    controls.homeScreen = screens[0]
    display.winId() #its native window, to hear about moves to another monitor from the start
    display.windowHandle().screenChanged.connect(controls.display_screen_changed)
    display.showMaximized()
    controls.resize(520, 960)
    if len(screens) > 1:
        controls.move(screens[1].availableGeometry().topLeft())
    else:
        available = screens[0].availableGeometry()
        controls.move(available.right() - 540, available.top() + 40)
    controls.show()
    code = app.exec()
    #(out without Python's teardown, as AntWorkout.py: destroying the Qt objects in its order could crash Qt6Core)
    for stream in (sys.stdout, sys.stderr):
        if stream is not None:
            stream.flush()
    _os._exit(code)


if __name__ == "__main__":
    main()
