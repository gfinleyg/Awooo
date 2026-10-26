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
    widget.set_center_rings([FtpColors.zone_color(p) for p in (230, 180, 140, 260, 90)])
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
    cadenceRanges = [60, 75, 90, 105]
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


SCENES = {name[len("scene_"):]: fn for name, fn in globals().items() if name.startswith("scene_")}


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
