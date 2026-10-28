"""Renders the Python app's custom widgets to PNG files, set up as the app sets them up and fed the same made-up
readings as render_widgets.exe (the C++ side, tests/parity/render_widgets.cpp), so the two can be compared side by
side (compare.py).

    py -3.10 render_python.py OUT_DIR [SCENE ...]

Scenes are listed in SCENES below; all of them by default. Run from anywhere: it works in ..\\..\\..\\AntWorkout."""

import math
import os
import sys

APP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "AntWorkout"))
sys.path.insert(0, APP_DIR)
OUT_DIR = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
os.chdir(APP_DIR)
#(the app's ANT module has to load before PyQt6, see AntWorkout.py; the HUD imports it)
sys.path.insert(0, os.path.join(APP_DIR, "AntNative", "python"))
import ant.manager  # noqa: E402,F401

from PyQt6.QtCore import Qt, QPointF  # noqa: E402
from PyQt6.QtGui import QColor, QFont  # noqa: E402
from PyQt6.QtWidgets import QApplication, QWidget, QVBoxLayout  # noqa: E402

app = QApplication(sys.argv[:1])

import AppStyle  # noqa: E402
import AppTheme  # noqa: E402
from AppState import AppState  # noqa: E402
import FtpColors  # noqa: E402
import Performance  # noqa: E402

AppTheme.install()
AppState.defaultFont = QFont(app.font())
app.setStyleSheet(AppStyle.APP_STYLESHEET)
AppTheme.clear()
Performance.gauge_fps = lambda: 0  #(no needle easing: drawn right at the reading)

AppState.ftp = "250"
AppState.ftpRanges = FtpColors.power_ranges()
FtpColors.update_graph_brushes()


# ---- The made-up ride (the same formulas as render_widgets.cpp) ----

def ride_power(t):
    return 150 + 60*math.sin(t/20) + 20*math.sin(t/3)


def ride_hr(t):
    return 120 + 20*math.sin(t/50)


def ride_cadence(t):
    return 85 + 5*math.sin(t/7)


def ride_series(fn, seconds, gap=None):
    times, values = [], []
    for i in range(int(seconds*4) + 1):
        t = i/4
        if gap and gap[0] < t < gap[1]:
            continue
        times.append(t)
        values.append(round(fn(t)))
    return times, values


def shown(widget, width, height):
    #(themed as the app's screen fitting themes everything it builds, see AppLayout.apply_layout)
    AppTheme.apply()
    widget.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen)
    widget.resize(width, height)
    widget.show()
    for _ in range(3):
        app.processEvents()
    return widget


# ---- Scenes ----

def scene_hud_graph():
    """The HUD's progress graph (ClassHUDWindow.__init__), 300 s into a workout"""
    import pyqtgraph as pg
    import ClassHUDWindow
    from ClassMaxStar import MaxStar, MaxBubble
    from ClassSolidColoredColumns import SolidColoredColumns
    from ClassWorkoutGraph import WorkoutGraph
    from WorkoutHelpers import WithDropoutsAsZero

    hud = ClassHUDWindow.HUDWindow
    graph = WorkoutGraph()
    graph.plotScreen.getViewBox().setMouseEnabled(x=False, y=False)
    graph.powerVBox.setMouseEnabled(x=False, y=False)
    graph.hrCadenceVBox.setMouseEnabled(x=False, y=False)
    graph.updateViews()
    graph.plotScreen.vb.sigResized.connect(graph.updateViews)
    graph.setXRange(0, AppState.graphSecondsShown)
    graph.setYRange(0, 1000, padding=0)
    graph.setMaximumHeight(200)
    graph.plotScreen.setTitle(None)
    graph.plotScreen.showAxis(axis='left', show=False)
    graph.plotScreen.showAxis(axis='right', show=False)
    graph.plotScreen.hideButtons()
    timeAxis = graph.plotScreen.getAxis('bottom')
    tickFont = QFont(graph.font())
    if tickFont.pointSizeF() > 0:
        tickFont.setPointSizeF(tickFont.pointSizeF()*0.8)
    else:
        tickFont.setPixelSize(max(1, round(tickFont.pixelSize()*0.8)))
    timeAxis.setTickFont(tickFont)
    graph.plotScreen.setLabel("bottom", "Time (minutes)", **{**graph.axisStyles, "font-size": "16px"})
    targetVBox = graph.powerVBox
    workoutPowerPlot = SolidColoredColumns(x=[0], y=[], ranges=[], colors=[0])
    targetVBox.addItem(workoutPowerPlot)
    workoutPowerCompletedPlot = SolidColoredColumns(x=[0], y=[], ranges=[], colors=[0])
    targetVBox.addItem(workoutPowerCompletedPlot)
    graph.powerVBox.addItem(pg.PlotCurveItem([0, 0]))
    powerPlot = graph.powerVBox.addedItems[-1]
    graph.hrCadenceVBox.addItem(pg.PlotCurveItem([0, 0]))
    hrPlot = graph.hrCadenceVBox.addedItems[-1]
    graph.hrCadenceVBox.addItem(pg.PlotCurveItem([0, 0]))
    cadencePlot = graph.hrCadenceVBox.addedItems[-1]
    graph.setBackground(None)
    graph.viewport().setAutoFillBackground(False)
    graph.setStyleSheet("QGraphicsView { border: 0px; background: transparent; }")
    for axisName in ('bottom', 'left'):
        graph.plotScreen.getAxis(axisName).setPen(pg.mkPen(QColor(0, 0, 0)))
        graph.plotScreen.getAxis(axisName).setTextPen(pg.mkPen(QColor(0, 0, 0)))
    graph.plotScreen.showGrid(x=True, y=True, alpha=0.25)
    nowLine = pg.InfiniteLine(pos=0, angle=90, movable=False)
    AppTheme.theme_pen(nowLine, pg.mkPen(QColor("#bdd6de"), width=1.5))
    nowLine.addMarker('v', position=0.0, size=14)
    nowLine.addMarker('^', position=1.0, size=14)
    nowLine.setZValue(5)
    graph.powerVBox.addItem(nowLine, ignoreBounds=True)
    sources = [(graph.powerVBox, "power", powerPlot, "Max Pwr: {:.0f} W"),
               (graph.hrCadenceVBox, "hr", hrPlot, "Max HR: {:.0f} bpm"),
               (graph.hrCadenceVBox, "cadence", cadencePlot, "Max Cad: {:.0f} rpm")]
    markers, stars, bubbles = [], [], []
    for vbox, sensor, curve, fmt in sources:
        color = FtpColors.SENSOR_COLORS[sensor]
        marker = pg.ScatterPlotItem(symbol='d', size=hud.CURRENT_MARKER_SIZE, brush=pg.mkBrush(color),
                                    pen=FtpColors.marker_outline(color))
        marker.setZValue(20)
        vbox.addItem(marker, ignoreBounds=True)
        markers.append(marker)
        star = MaxStar(curve, vbox, lambda: graph.plotScreen.vb.viewRange()[0])
        graph.scene().addItem(star)
        stars.append(star)
        bubble = MaxBubble(star, fmt)
        graph.scene().addItem(bubble)
        bubbles.append(bubble)
    #(the HUD's own size: the graph row under the gauges)
    holder = QWidget()
    holder.setStyleSheet("background: #bdd6de;")
    layout = QVBoxLayout(holder)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.addWidget(graph)
    shown(holder, 1800, 200)

    now = 300
    AppState.secondsPassed = now
    diff = max(0, now - (AppState.graphSecondsShown - AppState.graphBreakpoint))
    graph.setXRange(diff, diff + AppState.graphSecondsShown)
    nowLine.setValue(now)
    blocks = [0, 120, 300, 420, 600, 720]
    watts = [120, 200, 150, 260, 180]
    series = [ride_series(ride_power, now, gap=(200, 210)), ride_series(ride_hr, now), ride_series(ride_cadence, now)]
    for (vbox, sensor, curve, _), (times, values), marker, star in zip(sources, series, markers, stars):
        xs, ys = WithDropoutsAsZero(times, values, endTime=now)
        pen, halo = FtpColors.sensor_pens(FtpColors.SENSOR_COLORS[sensor])
        curve.setData(xs, ys, pen=pen, shadowPen=halo)
        marker.setData(x=[xs[-1]], y=[ys[-1]])
        index = max(range(len(values)), key=lambda i: (values[i], i))
        star.go_to(times[index], values[index])
        star.markerReach = hud.CURRENT_MARKER_REACH if star.x == times[-1] else 0.0
    powerMax = max(max(watts), max(series[0][1])) + 100
    hrCadenceMax = max(max(series[1][1]), max(series[2][1])) + 20
    workoutPowerPlot.setData(x=blocks[2:], y=watts[2:], ranges=AppState.ftpRanges,
                             colors=AppState.graphWorkoutBrushMid)
    workoutPowerCompletedPlot.setData(x=blocks[:3], y=watts[:2], ranges=AppState.ftpRanges,
                                      colors=AppState.graphWorkoutBrushDark)
    for _ in range(3):
        app.processEvents()
    tops = {graph.powerVBox: powerMax, graph.hrCadenceVBox: hrCadenceMax}
    for vbox, top in tops.items():
        rooms = [(bubble, bubble.star.y) for bubble in bubbles if bubble.star.vbox is vbox]
        vbox.setYRange(0, MaxStar.top_with_room(vbox, top, rooms), padding=0)
    for _ in range(3):
        app.processEvents()
    #the stars at rest (a first go_to puts them straight there) at a time their twinkle is at its middle size, and
    #the bubbles beside them grown in
    for phase, star in enumerate(stars):
        star.step(((3 + phase)*math.pi - phase*2.1)/3.6, phase)
    for when in (10.0, 10.0 + MaxBubble.APPEAR_SECONDS):
        avoid = [star.sceneBoundingRect() for star in stars if star.isVisible()]
        for bubble in bubbles:
            bubble.step(when, avoid)
    return holder


def gauge_scene(make):
    def scene():
        widget = make()
        return shown(widget, 400, 400)
    scene.__doc__ = make.__doc__
    return scene


@gauge_scene
def scene_power_gauge():
    """The HUD's power gauge (new_power_gauge) at 230 W, a 250 W target arc, rings for the last few blocks"""
    import ClassHUDWindow
    gauge = ClassHUDWindow.new_power_gauge()
    widget = gauge.ui.widget
    widget.set_value_colors(FtpColors.zone_stops(FtpColors.power_ranges()))
    widget.setValue(230)
    c0 = FtpColors.gauge_ring(230)
    widget.outer_circle_bg = [[0.25, QColor(0, 0, 0, 0)], [0.18, QColor("#" + c0)]]
    widget.set_center_rings([FtpColors.zone_color(p) for p in (90, 260, 140, 180, 230)]) #(oldest outermost, the power now in the middle)
    widget.arcCenterValue = 250
    a0, a1 = FtpColors.gauge_arc(250)
    widget.newArcColors = [[0, QColor("#88" + a0)], [.5, QColor("#88" + a1)]]
    widget.setEnableDrawNewArc()
    return gauge


@gauge_scene
def scene_cadence_gauge():
    """The HUD's cadence gauge (workout mode) at 87.4 RPM"""
    from ClassAnalogGauge import Gauge
    gauge = Gauge(None, 'Chainring.svg', spin=True)
    gauge.ui.svg.spinTimer.stop()
    w = gauge.ui.widget
    w.setNeedleColor(*QColor(AppState.gaugeRed).getRgb())
    w.setMouseTracking(False)
    w.outer_circle_bg = [[0, QColor(0, 0, 0, 0)]]
    w.set_scale_polygon_colors([[0, QColor(0, 0, 0, 0)]])
    w.bigScaleMarker = QColor(0, 0, 0, 0)
    w.needle_center_bg = [[0, QColor("#FFFFFF")]]
    w.big_needle_center_size = 5
    w.units = "RPM"
    w.initial_scale_fontsize = 18
    w.scale_fontsize = w.initial_scale_fontsize
    w.initial_value_fontsize = 56
    w.value_fontsize = w.initial_value_fontsize
    w.setMaxValue(200)
    w.setScalaCount(1)
    w.scala_subdiv_count = 1
    w.setEnableBigScaleGrid(False)
    w.altUnitsDisplay = True
    w.value_decimals = 1
    w.fit_value_in_center = True
    w.setValue(87.4)
    return gauge


@gauge_scene
def scene_cadence_mode_gauge():
    """Musical Cadence Mode's big cadence gauge at 92.3 RPM, a 95 RPM target, rings on a half beat"""
    from ClassAnalogGauge import Gauge
    gauge = Gauge(None, 'Chainring.svg', spin=True)
    gauge.ui.svg.spinTimer.stop()
    w = gauge.ui.widget
    w.setNeedleColor(*QColor(AppState.gaugeRed).getRgb())
    w.outer_circle_bg = [[0, QColor(0, 0, 0, 0)]]
    w.set_scale_polygon_colors([[0, QColor(0, 0, 0, 0)]])
    w.bigScaleMarker = QColor(0, 0, 0, 0)
    w.needle_center_bg = [[0, QColor("#FFFFFF")]]
    w.big_needle_center_size = 5
    w.units = "RPM"
    w.initial_value_fontsize = 56
    w.value_fontsize = w.initial_value_fontsize
    w.setScalaCount(1)
    w.scala_subdiv_count = 1
    w.setEnableBigScaleGrid(False)
    w.altUnitsDisplay = True
    w.value_decimals = 1
    w.fit_value_in_center = True
    w.setMaxValue(140)
    w.setScalaCount(14)
    w.initial_scale_fontsize = 13
    w.scale_fontsize = 13
    w.gauge_color_inner_radius_factor = 0.95
    w.setEnableBarGraph(False)
    gauge.ui.svg.scale = w.gauge_color_inner_radius_factor
    cadenceRanges = FtpColors.CadenceRanges([60, 75, 90, 105])
    w.set_value_colors(FtpColors.zone_stops(cadenceRanges))
    color = QColor("#" + FtpColors.gauge_ring(95, cadenceRanges))
    w.set_center_rings((color.darker(130), color), 0.5)
    w.arcCenterValue = 95
    a0, a1 = FtpColors.gauge_arc(95, cadenceRanges)
    w.newArcColors = [[0, QColor("#88" + a0)], [.5, QColor("#88" + a1)]]
    w.setEnableDrawNewArc()
    w.setValue(92.3)
    return gauge


@gauge_scene
def scene_hr_gauge():
    """The HUD's heart rate gauge at 132 BPM, the heart at rest"""
    from ClassAnalogGauge import Gauge
    gauge = Gauge(None, 'HeartSmaller.svg', beatSvg=('Heart.svg', "#FF8FAB", "#E8102A"))
    gauge.ui.svg.beatTimer.stop()
    w = gauge.ui.widget
    w.setNeedleColor(*QColor(AppState.gaugeRed).getRgb())
    w.setMouseTracking(False)
    w.needle_center_bg = [[0, QColor("#FFFFFF")]]
    w.set_scale_polygon_colors([[0, QColor(0, 0, 0, 0)]])
    w.outer_circle_bg = [[0, QColor(0, 0, 0, 0)]]
    w.big_needle_center_size = 20
    w.units = "BPM"
    w.setMinValue(40)
    w.setMaxValue(200)
    w.setEnableBarGraph(False)
    w.setScalaCount(2)
    w.scala_subdiv_count = 5
    w.setValue(132)
    return gauge


def make_power_spiral():
    """The HUD's spiral 200 s into a 10 minute workout after a 5 minute ride (at a higher FTP), 80 s into its second
    2 minute block, with a stretch without cadence"""
    from ClassPowerSpiral import PowerSpiral
    now = FtpColors.power_ranges()
    then = [limit*1.2 for limit in now]
    spiral = PowerSpiral()
    spiral.clear(600, [(300, [(t + 0.5, 250 + 50*math.sin(t/30), 90 + 10*math.sin(t/9)) for t in range(300)], then)])
    spiral.set_block(120)
    for i in range(401):
        t = i*0.5
        if t == 120:
            spiral.set_block(120)
        cadence = None if 100 <= t < 110 else 85 + 15*math.sin(t/11)
        spiral.add_sample(t, t if t < 120 else t - 120, 150 + 80*math.sin(t/15), cadence)
    spiral.set_progress(200, 600, 80, 120)
    return spiral


def scene_power_spiral():
    """The HUD's spiral (make_power_spiral)"""
    return shown(make_power_spiral(), 400, 400)


def scene_grade_overlay():
    """A block's grade over the spiral, before it fades"""
    from ClassGradeWidgets import GradeOverlay
    spiral = make_power_spiral()
    overlay = GradeOverlay(spiral)
    overlay.show_grade("A+")
    overlay.timer.stop()
    return shown(spiral, 400, 400)


GRADE_ROWS = [
    {"kind": "ride", "label": "Workout: Test Ride", "pct": 91.2, "grade": "A", "note": "45m", "graded": True},
    {"kind": "block", "label": "5:00 @ 150W", "pct": 95, "grade": "A+", "note": ""},
    {"kind": "interval", "label": "3x", "pct": None, "grade": "", "note": ""},
    {"kind": "sub", "label": "1:00 @ 300W", "pct": 80, "grade": "B", "note": "S · A · B+"},
    {"kind": "sub", "label": "2:00 @ 120W", "pct": 99, "grade": "S", "note": ""},
    {"kind": "song", "label": "Artist - Title", "pct": 70, "grade": "C+", "note": ""},
    {"kind": "block", "label": "3:00 @ 200-250W", "pct": 60, "grade": "D", "note": "ended early", "indent": True},
    {"kind": "block", "label": "Free Ride", "pct": 88, "grade": "A-", "note": ""},
]


def scene_grade_list():
    """The grade list's kinds of rows, and the average"""
    from ClassGradeWidgets import GradeList
    grades = GradeList()
    grades.rows = [dict(row) for row in GRADE_ROWS]
    grades.average = 85.3
    return shown(grades, 600, 500)


def scene_grade_list_scrolled():
    """The grade list scrolled, with "more..." above and below"""
    from ClassGradeWidgets import GradeList
    grades = GradeList()
    grades.rows = [dict(row) for row in GRADE_ROWS]*3
    grades.average = 85.3
    grades.scrollOffset = 4
    return shown(grades, 600, 400)


def scene_smooth_bar():
    """The HUD's timer bar part of the way along, between pixels"""
    from ClassSmoothProgressBar import SmoothProgressBar
    bar = SmoothProgressBar()
    bar.setRange(0, 100*600)
    bar.setTextVisible(False)
    bar.setFraction(0.4373)
    bar.setBarColors("#1f15e1", "#bdd6de")
    return shown(bar, 301, 34)


BLOCK_LINES = ["5:00 @ 150W", "1:00 @ 300W", "2:00 @ 120W", "3:00 @ 200-250W", "Free Ride",
               "4:00 @ 180W", "0:30 @ 400W", "6:00 @ 90W", "2:00 @ 260W", "1:00 @ 140W"]


def scene_long_labels():
    """The coming up list, longer than fits: scrolled one down, "more..." above and below"""
    from ClassLongVerticalLabelSeries import LongVLabels
    labels = LongVLabels(True, AppState.ftp, AppState.ftpRanges)
    labels.ui.widget.setFullText("\n".join(BLOCK_LINES) + "\n")
    shown(labels, 500, 300)
    labels.ui.widget.scroll_to(1)
    for _ in range(3):
        app.processEvents()
    return labels


def scene_zone_legend():
    """The zone legend under the coming up list: the zones side by side, and where each ends (haloed: on its own
    it's its window, the default theme's black on the gradient's middle)"""
    from ClassZoneLegend import LEGEND_HEIGHT, ZoneLegend
    from ClassLongVerticalLabelSeries import BLOCK_LABEL_WIDTH
    return shown(ZoneLegend(), BLOCK_LABEL_WIDTH, LEGEND_HEIGHT)


# ---- The screens ----

#the rider's saved settings for the screens' scenes (written to a folder of their own, like render_widgets.exe's)
SCENE_SETTINGS = {"wheelCircumference": 2.1, "ftp": 250, "maxHr": 185}


def stub_classes():
    from PyQt6.QtCore import QObject, pyqtSignal

    class StubBridge(QObject):
        deviceDiscovered = pyqtSignal(int, int)
        devicesConnected = pyqtSignal()
        devicesDisconnected = pyqtSignal()
        statusMessage = pyqtSignal(str)
        errorOccurred = pyqtSignal(str)
        batteryChanged = pyqtSignal()
        calibrationUpdate = pyqtSignal(int, str, object, int)
        def found_devices(self): return ["11 100", "11 101", "120 200"]
        def take_error(self): return None
        def is_connect_requested(self): return False
        def waiting_for_stick(self): return None
        def is_ready(self): return False
        def selected_roles(self): return set()
        def selected_device(self, role): return None
        def is_initialized(self): return True
        def low_battery_warnings(self): return []
        def scan_settle_remaining(self): return 0
        def request_connect(self, selection): pass

    class Parent:
        startExe = StubBridge()
    return StubBridge, Parent


def in_work_folder(build, files=None):
    """build() run in a folder of its own holding SCENE_SETTINGS (and files: {name: text}), as render_widgets.exe
    runs"""
    import json
    import tempfile
    workDir = tempfile.mkdtemp()
    with open(os.path.join(workDir, "savedSettingsANT.json"), "w") as f:
        json.dump(SCENE_SETTINGS, f)
    for name, text in (files or {}).items():
        with open(os.path.join(workDir, name), "w", encoding="utf-8") as f:
            f.write(text)
    here = os.getcwd()
    os.chdir(workDir)
    try:
        return build()
    finally:
        os.chdir(here)


def shown_as_is(widget):
    """A popup at the size it gives itself"""
    AppTheme.apply()
    widget.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen)
    widget.show()
    for _ in range(3):
        app.processEvents()
    return widget


def first_shipped_workout():
    folder = os.path.join(APP_DIR, "awoWorkouts")
    return os.path.join(folder, sorted(os.listdir(folder), key=str.lower)[0]).replace("\\", "/")


def scene_setup_screen():
    """The setup screen with the saved settings, two power meters and a heart rate monitor found (the heart rate
    monitor picked by itself), the first shipped workout loaded, laid out at 1920x1116"""
    from ClassWorkoutSetupWindow import WorkoutSetupWindow
    import AppLayout
    StubBridge, Parent = stub_classes()

    def build():
        setup = WorkoutSetupWindow(Parent())
        setup.timer.stop()
        setup.blinkTimer.stop()
        path = first_shipped_workout()
        if setup.process_workout_file(path):
            setup.workoutDisplayLabel.show()
            AppState.workoutFilename = path
            AppState.workoutFilenameShort = path.split("/")[-1]
            setup.workoutFileSelected.setText(" " + AppState.workout_display_name() + " ")
        #(as the workout picker's pick ends, see open_workout_dialog)
        setup.workoutSelectBlink = AppState.workoutFilenameShort == ""
        setup.check_enable_start_button()
        setup.setup_blink()
        setup.init_ant_scan_connect()
        holder = QWidget()
        holder.setObjectName("holder")
        holder.setStyleSheet("#holder{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}")
        layout = QVBoxLayout(holder)
        layout.addWidget(setup)
        AppLayout.apply_layout(holder, {"Init": setup}, 1.0)
        return shown(holder, 1920, 1116)
    return in_work_folder(build)


def scene_select_devices():
    """Picking a speed/cadence source: a combined meter, a cadence meter and two speed meters found, the cadence
    meter picked before"""
    from ClassSelectDevicesWindow import SelectDevicesWindow
    from WorkoutHelpers import WriteSelectedDeviceLines
    found = ["121 400", "122 300", "123 301", "123 302"]
    WriteSelectedDeviceLines(["122 300"])
    try:
        window = SelectDevicesWindow(None, found, "sc", foundDevices=found)
        window.blinkTimer.stop()
        #(the Python app's title bar says "CustomTitleBar": the C++ app's shows the window's title, see CHANGES.md)
        window.title_bar.title.setText(window.windowTitle())
        return shown_as_is(window)
    finally:
        WriteSelectedDeviceLines([])


def scene_connect_devices():
    """The connect prompt: a power meter picked and found, a heart rate monitor picked and not, a speed/cadence
    meter found and not picked"""
    from ClassConnectDevicesWindow import ConnectDevicesWindow
    from WorkoutHelpers import WriteSelectedDeviceLines
    StubBridge, _ = stub_classes()
    class Bridge(StubBridge):
        def found_devices(self): return ["11 100", "121 400"]
    WriteSelectedDeviceLines(["11 100", "120 200"])
    try:
        window = ConnectDevicesWindow(bridge=Bridge())
        #(as for the device selection's title, see scene_select_devices)
        window.title_bar.title.setText("   Connect Devices   ")
        return shown_as_is(window)
    finally:
        WriteSelectedDeviceLines([])


def scene_calibrate_power():
    """Calibrate Power with no power meter connected"""
    from ClassCalibratePowerWindow import CalibratePowerWindow
    StubBridge, _ = stub_classes()
    def build():
        window = CalibratePowerWindow(StubBridge())
        window.timer.stop()
        return shown_as_is(window)
    return in_work_folder(build)


def scene_workout_picker():
    """The workout picker listing the first three shipped workouts, the second picked"""
    import json
    from ClassWorkoutSetupWindow import WorkoutSetupWindow
    from ClassWorkoutSelectWindow import WorkoutSelectWindow
    _, Parent = stub_classes()
    folder = os.path.join(APP_DIR, "awoWorkouts")
    files = [os.path.join(folder, name).replace("\\", "/") for name in sorted(os.listdir(folder), key=str.lower)[:3]]
    def build():
        setup = WorkoutSetupWindow(Parent())
        setup.timer.stop()
        setup.blinkTimer.stop()
        return shown_as_is(WorkoutSelectWindow(setup))
    return in_work_folder(build, {"workoutPickerANT.json": json.dumps({"files": files, "last": files[1]})})



def rest_stars(stars, bubbles):
    """The graph's stars at rest (a first go_to puts them straight there) at a time their twinkle is at its middle
    size, and the bubbles beside them grown in"""
    import math
    from ClassMaxStar import MaxBubble
    for phase, star in enumerate(stars):
        star.step(((3 + phase)*math.pi - phase*2.1)/3.6, phase)
    for when in (10.0, 10.0 + MaxBubble.APPEAR_SECONDS):
        avoid = [star.sceneBoundingRect() for star in stars if star.isVisible()]
        for bubble in bubbles:
            bubble.step(when, avoid)


def scene_hud_screen():
    """The workout screen 600 s into the first shipped workout, ridden by PreviewRide's Rider(seed=3) a second a
    tick, laid out at 1920x1116"""
    import datetime
    import time as timeModule
    from types import SimpleNamespace
    import AppLayout
    import ClassHUDWindow
    import PreviewRide
    from ClassWorkoutSetupWindow import WorkoutSetupWindow
    _, Parent = stub_classes()

    def build():
        setup = WorkoutSetupWindow(Parent())
        setup.timer.stop()
        setup.blinkTimer.stop()
        PreviewRide.reset_ride_state()
        AppState.ftp, AppState.maxHr, AppState.wheelCircumference = "250", "185", "2.105"
        AppState.workoutMode, AppState.cadenceMode, AppState.ftpTestMode = True, False, None
        AppState.antHRM, AppState.antSpeedCadence, AppState.antCadence, AppState.antSpeed = True, True, False, False
        AppState.ftpRanges = FtpColors.power_ranges()
        FtpColors.update_graph_brushes()
        path = first_shipped_workout()
        assert setup.process_workout_file(path)
        AppState.workoutFilename = path
        AppState.workoutFilenameShort = path.split("/")[-1]
        AppState.startTimestamp = datetime.datetime.now()
        hud = ClassHUDWindow.HUDWindow(SimpleNamespace(startExe=PreviewRide.FakeBridge()))
        realMonotonic = timeModule.monotonic
        timeModule.monotonic = lambda: 1000.0
        try:
            PreviewRide.run_hud(hud, 600, PreviewRide.Rider(seed=3))
        finally:
            timeModule.monotonic = realMonotonic
        hud.stop_timers()
        hud.gradeOverlay.timer.stop()
        hud.cadenceGaugeDisplay.ui.svg.spinTimer.stop()
        hud.hrGaugeDisplay.ui.svg.beatTimer.stop()
        holder = QWidget()
        holder.setObjectName("holder")
        holder.setStyleSheet("#holder{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}")
        layout = QVBoxLayout(holder)
        layout.addWidget(hud)
        AppLayout.apply_layout(holder, {"Workout": hud}, 1.0)
        shown(holder, 1920, 1116)
        rest_stars([star for star, _ in hud.maxStars], hud.maxBubbles)
        for _ in range(3):
            app.processEvents()
        return holder
    return in_work_folder(build)



def scene_synopsis_screen():
    """The synopsis of the first shipped workout ridden start to finish by PreviewRide's Rider(seed=11) a second a
    tick, its readings recorded as LayoutViewer records them, laid out at 1920x1116"""
    import datetime
    import math
    import time as timeModule
    from types import SimpleNamespace
    import AppLayout
    import ClassHUDWindow
    import ClassSynopsisWindow
    import PreviewRide
    import WorkoutData
    from ClassWorkoutSetupWindow import WorkoutSetupWindow
    _, Parent = stub_classes()

    def build():
        setup = WorkoutSetupWindow(Parent())
        setup.timer.stop()
        setup.blinkTimer.stop()
        PreviewRide.reset_ride_state()
        AppState.ftp, AppState.maxHr, AppState.wheelCircumference = "250", "185", "2.105"
        AppState.workoutMode, AppState.cadenceMode, AppState.ftpTestMode = True, False, None
        AppState.antHRM, AppState.antSpeedCadence, AppState.antCadence, AppState.antSpeed = True, True, False, False
        AppState.ftpRanges = FtpColors.power_ranges()
        FtpColors.update_graph_brushes()
        path = first_shipped_workout()
        assert setup.process_workout_file(path)
        AppState.workoutFilename = path
        AppState.workoutFilenameShort = path.split("/")[-1]
        startTimestamp = datetime.datetime(2026, 10, 26, 8, 0, 0)
        AppState.startTimestamp = startTimestamp
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
            revsPerSecond = speed/2.23694/float(AppState.wheelCircumference)
            speedState["revs"] += revsPerSecond
            revs = speedState["revs"]
            lastRevTime = t - (revs - int(revs))/max(revsPerSecond, 0.01)
            WorkoutData.speed.times.append(ts)
            WorkoutData.speed.columns[0].append(int(revs) & 0xFFFF)
            WorkoutData.speed.columns[1].append(int(lastRevTime*1024) & 0xFFFF)

        bridge = PreviewRide.FakeBridge()
        hud = ClassHUDWindow.HUDWindow(SimpleNamespace(startExe=bridge))
        realMonotonic = timeModule.monotonic
        timeModule.monotonic = lambda: 1000.0
        try:
            PreviewRide.run_hud(hud, AppState.workoutDurationSeconds + 1, PreviewRide.Rider(seed=11), record)
        finally:
            timeModule.monotonic = realMonotonic
        hud.stop_timers()
        hud.deleteLater()
        AppState.startTimestamp = startTimestamp
        AppState.workoutPowerRequestsSysTime[:] = [startTimestamp + datetime.timedelta(seconds=s)
                                                   for s in AppState.workoutPowerRequestsSeconds]
        parent = SimpleNamespace(startExe=bridge, workoutHudWidget=None, setupWidget=setup, showMaximized=lambda: None,
                                 setFocus=lambda: None)
        synopsis = ClassSynopsisWindow.SynopsisWindow(parent)
        synopsis.starTimer.stop()
        synopsis.selectionTimer.stop()
        holder = QWidget()
        holder.setObjectName("holder")
        holder.setStyleSheet("#holder{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}")
        layout = QVBoxLayout(holder)
        layout.addWidget(synopsis)
        AppLayout.apply_layout(holder, {"Synopsis": synopsis}, 1.0)
        shown(holder, 1920, 1116)
        synopsis.layout_max_labels()
        for phase, marker in enumerate(synopsis.maxMarkers):
            marker["star"].step(((3 + phase)*math.pi - phase*2.1)/3.6, phase)
        for _ in range(3):
            app.processEvents()
        return holder
    return in_work_folder(build)


#LayoutViewer's made up songs (VIEWER_SONGS): (title, [(seconds, rpm)])
VIEWER_SONGS = (("Viewer Band - Warm Up Groove", [(30, 0), (90, 82), (60, 88)]),
                ("The Layouts - Tempo Change", [(70, 96), (50, 72), (60, 104)]),
                ("Sample Artist - Cool Down", [(80, 90), (20, 0), (60, 78)]))


def viewer_music_workout(songs, analyses, preferred=None, minSegmentSeconds=None):
    """LayoutViewer.viewer_music_workout: VIEWER_SONGS as MusicCadence.build_workout's (blocks, tracks, songStarts,
    pulses)"""
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


def scene_music_picker():
    """The song picker with LayoutViewer's made up songs (LayoutViewer.ViewerMusicPicker): three checked, one not,
    one unreadable, a playlist entry not found"""
    from ClassWorkoutSetupWindow import WorkoutSetupWindow
    from ClassMusicSelectWindow import MusicSelectWindow
    _, Parent = stub_classes()

    class Picker(MusicSelectWindow):
        def analyze(self, paths):
            for path in paths:
                self.analyses[path] = ({"error": "can't be read"} if "Broken" in path else
                                       {"duration": 180.0 + 37*(len(self.analyses) % 4)})
            for item in self.items():
                self.label_item(item)

    def build():
        setup = WorkoutSetupWindow(Parent())
        setup.timer.stop()
        setup.blinkTimer.stop()
        picker = Picker(setup)
        picker.clear_songs()
        picker.add_to_list([(f"C:/Music/{title}.mp3", title) for title, _ in VIEWER_SONGS] +
                           [("C:/Music/Unchecked Song.mp3", "Some Artist - Unchecked Song"),
                            ("C:/Music/Broken File.mp3", "Broken File")],
                           checked={f"C:/Music/{title}.mp3" for title, _ in VIEWER_SONGS})
        picker.missing = ["C:/Music/Not There.mp3"]
        picker.update_summary()
        return shown_as_is(picker)
    return in_work_folder(build)


def scene_cadence_screen():
    """Musical Cadence Mode's workout screen 40% into LayoutViewer's made up songs, ridden by PreviewRide's
    Rider(seed=3) a second a tick (silent music), laid out at 1920x1116"""
    import datetime
    import time as timeModule
    from types import SimpleNamespace
    import AppLayout
    import ClassHUDWindow
    import MusicCadence
    import PreviewRide
    from ClassWorkoutSetupWindow import WorkoutSetupWindow
    _, Parent = stub_classes()

    def build():
        setup = WorkoutSetupWindow(Parent())
        setup.timer.stop()
        setup.blinkTimer.stop()
        PreviewRide.reset_ride_state()
        AppState.ftp, AppState.maxHr, AppState.wheelCircumference = "250", "185", "2.105"
        AppState.workoutMode, AppState.cadenceMode, AppState.ftpTestMode = True, True, None
        AppState.antHRM, AppState.antSpeedCadence, AppState.antCadence, AppState.antSpeed = True, True, False, False
        setup.musicSelection = {"songs": [], "analyses": {}, "preferred": 85, "minSegment": 8}
        buildWorkout, MusicCadence.build_workout = MusicCadence.build_workout, viewer_music_workout
        try:
            setup.build_music_workout()
        finally:
            MusicCadence.build_workout = buildWorkout
        AppState.workoutFilename = AppState.workoutFilenameShort = "music"
        AppState.startTimestamp = datetime.datetime.now()
        hud = ClassHUDWindow.HUDWindow(SimpleNamespace(startExe=PreviewRide.FakeBridge(), setWindowTitle=lambda t: None))
        realMonotonic = timeModule.monotonic
        timeModule.monotonic = lambda: 1000.0
        try:
            PreviewRide.run_hud(hud, AppState.workoutDurationSeconds*0.4, PreviewRide.Rider(seed=3))
        finally:
            timeModule.monotonic = realMonotonic
        hud.stop_timers()
        hud.gradeOverlay.timer.stop()
        for gauge in ("cadenceGaugeDisplay", "hrGaugeDisplay"):
            svg = getattr(getattr(getattr(hud, gauge, None), "ui", None), "svg", None)
            for timer in ("spinTimer", "beatTimer"):
                if hasattr(svg, timer):
                    getattr(svg, timer).stop()
        holder = QWidget()
        holder.setObjectName("holder")
        holder.setStyleSheet("#holder{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1);}")
        layout = QVBoxLayout(holder)
        layout.addWidget(hud)
        AppLayout.apply_layout(holder, {"Workout": hud}, 1.0)
        shown(holder, 1920, 1116)
        rest_stars([star for star, _ in hud.maxStars], hud.maxBubbles)
        for _ in range(3):
            app.processEvents()
        return holder
    return in_work_folder(build)



def fusion(widget):
    """The widget and everything in it in Qt's Fusion style, as ColorPicker.exe runs"""
    from PyQt6.QtWidgets import QStyleFactory
    style = QStyleFactory.create("Fusion")
    widget._fusion = style
    for child in [widget] + widget.findChildren(QWidget):
        child.setStyle(style)


def scene_color_picker():
    """ColorPicker.py's window at QColor(200, 200, 200, 130)"""
    import ColorPicker
    def build():
        picker = ColorPicker.ColorPicker(200, 200, 200, 130)
        fusion(picker)
        return shown(picker, 360, 460)
    return in_work_folder(build)


def scene_ftp_colors_tab():
    """LayoutViewer's FTP Colors tab at 700x460"""
    import FtpColorsEditor
    def build():
        AppState.ftp = "250"
        return shown(FtpColorsEditor.FtpColorsTab(), 700, 460)
    return in_work_folder(build)


def scene_ftp_mockup():
    """The FTP Colors mockup window at FTP 250, 230 W, at 1000x900"""
    import FtpColorsEditor
    def build():
        AppState.ftp = "250"
        window = shown(FtpColorsEditor.FtpMockupWindow(), 1000, 900)
        #(the point at buttons each refresh replaced, gone)
        from PyQt6.QtCore import QEvent
        app.sendPostedEvents(None, QEvent.Type.DeferredDelete.value)
        app.processEvents()
        return window
    return in_work_folder(build)



def scene_theme_picker():
    """The theme picker over the shipped themes, the base theme picked, its workout screen preview drawn"""
    import shutil
    from ClassThemeSelectWindow import ThemeSelectWindow
    from ClassWorkoutSetupWindow import WorkoutSetupWindow
    _, Parent = stub_classes()

    def build():
        os.makedirs("awoWorkouts", exist_ok=True)
        shutil.copy(os.path.join(APP_DIR, "awoWorkouts", "0_Little_bit_of_everything.awo"), "awoWorkouts")
        setup = WorkoutSetupWindow(Parent())
        setup.timer.stop()
        setup.blinkTimer.stop()
        owner = QWidget()
        owner.setupWidget = setup
        dialog = ThemeSelectWindow(owner)
        dialog._owner = owner
        AppTheme.apply()
        dialog.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen)
        dialog.show()
        for _ in range(3):
            app.processEvents()
        dialog.build_preview()
        for _ in range(3):
            app.processEvents()
        return dialog
    return in_work_folder(build)


#the viewer scenes' looks: (available size, the theme files loaded, in order; "fonts" is VIEWER_FONTS)
VIEWER_LOOKS = {
    "": ((1920, 1152), ()),
    "_720": ((1280, 672), ()),
    "_1440": ((2560, 1392), ()),
    "_synthwave": ((1920, 1152), ("colors/11-synthwave.json", "gradients/02-inferno.json")),
    "_arctic": ((1366, 720), ("fonts", "colors/28-arctic-white.json", "gradients/07-training-zones.json")),
}
#a whole theme of fonts only: the text in Orbitron, bigger and bolder, the buttons smaller
VIEWER_FONTS = {"fonts": {"main": {"family": "Orbitron", "scale": 110, "weight": "700"},
                          "button": {"family": "Roboto Flex", "scale": 90, "weight": "700"}}}


def viewer_scene(view, look=""):
    """LayoutViewer's display window in Workout Mode with the preview workout, 40% of the way through, in one of
    VIEWER_LOOKS (by default laid out for 1920x1152, the design size less the taskbar, in the default theme) and
    showing view; what moves by itself stopped where it is"""
    import json
    import AppLayout
    import LayoutViewer

    def build():
        available, files = VIEWER_LOOKS[look]
        AppTheme.clear()
        for name in files:
            if name == "fonts":
                with open("fonts.json", "w") as f:
                    json.dump(VIEWER_FONTS, f)
                AppTheme.load_file("fonts.json")
            else:
                AppTheme.load_file(os.path.join(APP_DIR, "themes", *name.split("/")))
        display = LayoutViewer.DisplayWindow(AppStyle.APP_STYLESHEET)
        display.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen)
        #(the clock frozen while the rides are simulated: each simulated second is a second and the time the tick
        #took, see PreviewRide.run_hud, so the ride would end up a little past 40%; render_widgets' are exact)
        import time as timeModule
        realMonotonic = timeModule.monotonic
        timeModule.monotonic = lambda: 1000.0
        try:
            display.build("workout", os.path.join(APP_DIR, "awoWorkouts", "0_Little_bit_of_everything.awo"), 40)
        finally:
            timeModule.monotonic = realMonotonic
        display.maximizedMode = False
        display.resize(*available)
        display.show()
        AppLayout.apply_layout(display, display.views, AppLayout.scale_for(available), available)
        display.show_view(view)
        for _ in range(3):
            app.processEvents()
        hud = display.views["Workout"]
        hud.stop_timers()
        hud.gradeOverlay.timer.stop()
        hud.cadenceGaugeDisplay.ui.svg.spinTimer.stop()
        hud.hrGaugeDisplay.ui.svg.beatTimer.stop()
        rest_stars([star for star, _ in hud.maxStars], hud.maxBubbles)
        synopsis = display.views["Synopsis"]
        synopsis.starTimer.stop()
        synopsis.selectionTimer.stop()
        if view == "Synopsis":
            synopsis.layout_max_labels()
            for phase, marker in enumerate(synopsis.maxMarkers):
                marker["star"].step(((3 + phase)*math.pi - phase*2.1)/3.6, phase)
        for _ in range(3):
            app.processEvents()
        return display
    return in_work_folder(build)


SCENES = {name[len("scene_"):]: fn for name, fn in globals().items() if name.startswith("scene_")}
SCENES.update({f"viewer_{view.lower().replace(' ', '_')}{look}": (lambda v=view, k=look: viewer_scene(v, k))
               for look in VIEWER_LOOKS for view in ("Init", "Workout", "Synopsis", "Music Picker", "Workout Picker")})


def main():
    names = sys.argv[2:] or list(SCENES)
    os.makedirs(OUT_DIR, exist_ok=True)
    for name in names:
        widget = SCENES[name]()
        path = os.path.join(OUT_DIR, name + ".png")
        widget.grab().save(path)
        print(path)


if __name__ == "__main__":
    main()
