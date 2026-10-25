import bisect
import math
import time
from html import escape

import pyqtgraph as pg

from PyQt6.QtCore import QEvent, QTime, QTimer, Qt, QPointF, QRectF, QLineF
from PyQt6.QtGui import QColor, QFont, QFontMetricsF, QPen, QPainter, QPainterPath
from PyQt6.QtWidgets import (QFrame, QGridLayout, QHBoxLayout, QPushButton, QVBoxLayout, QWidget, QLabel, QTimeEdit,
                             QGraphicsSimpleTextItem, QGraphicsLineItem, QGraphicsRectItem, QGraphicsObject)

from ClassAnalogGauge import *
from ClassLongVerticalLabelSeries import *
from ClassSolidColoredColumns import SolidColoredColumns
import FtpColors
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *
from ClassMaxStar import MaxStar, STEP_MS

from AppState import AppState
from ClassConfirmExitWindow import ConfirmExitWindow
from WorkoutHelpers import (MinimizedDuration, GetMaxOrZero, GetMaxOrZeroAndIndex, WithDropoutsAsZero,
                            DROPOUT_ZERO_SECONDS, HOLD_POWER_READINGS, HOLD_HR_READINGS, HOLD_CADENCE_READINGS,
                            HOLD_SPEED_READINGS)
import AppTheme
import WorkoutGrading
import FtpTest
import Units
import KeyShortcuts
import Session
from ClassGradeWidgets import GradeList




class PlotLabel(QGraphicsSimpleTextItem):
    """Scene-positioned text in a rounded box matching the plot background"""
    PADX = 7
    PADY = 3
    RADIUS = 7
    #Orbitron size of the max labels (the synopsis' axis titles match it)
    FONT_POINTS = 11
    #called on a left click (pyqtgraph's scene delivers clicks to any item with mouseClickEvent)
    onClick = None
    #(drawn in the theme's color for black, see AppTheme.install: this one is near (40, 40, 40) on the light label)
    borderColor = QColor(0, 0, 0, 215)
    borderWidth = 1.2

    def setBorderColor(self, color, width):
        self.borderColor, self.borderWidth = color, width
        self.update()

    def boundingRect(self):
        return super().boundingRect().adjusted(-self.PADX, -self.PADY, self.PADX, self.PADY)

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(QPen(self.borderColor, self.borderWidth))
        painter.setBrush(QColor("#bdd6de"))
        painter.drawRoundedRect(self.boundingRect().adjusted(0.6, 0.6, -0.6, -0.6), self.RADIUS, self.RADIUS)
        super().paint(painter, option, widget)

    def shape(self):
        path = QPainterPath()
        path.addRect(self.boundingRect())
        return path

    def mouseClickEvent(self, ev):
        if self.onClick is not None and ev.button() == Qt.MouseButton.LeftButton:
            ev.accept()
            self.onClick()


class GradeRegion(QGraphicsRectItem):
    """A graded block's span on the graph (in vbox's coordinates, its ride's target axis); clicking it opens its
    grade. Drawn as nothing, under the curves so they still take their own clicks"""
    def __init__(self, rect, key, vbox, onClick):
        super().__init__(rect)
        self.key = key
        self.vbox = vbox
        self.onClick = onClick
        self.setPen(QPen(Qt.PenStyle.NoPen))
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def mouseClickEvent(self, ev):
        if ev.button() == Qt.MouseButton.LeftButton:
            ev.accept()
            self.onClick(self)


class BandStrip(QGraphicsObject):
    """Bands along the top of the graph, each over its stretch of time in alternating shades, its title inside (cut
    short, or left out, where it doesn't fit). Hovering one shows its whole title and when it was; clicking one zooms
    the graph to it"""
    COLORS = (QColor(40, 39, 94, 215), QColor(72, 98, 140, 215))
    PADX = 6

    def __init__(self, bands, vbox, onClick, top=0.0):
        super().__init__()
        self.bands = bands #[(start, end, title)] in the graph's seconds
        self.vbox = vbox
        self.onClick = onClick
        self.top = top #how far down from the top of the plot area (px): under the strips above it
        self.set_font()
        self.width = 0.0
        self.setAcceptHoverEvents(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        #over the curves, under the max labels and their leader lines
        self.setZValue(998)

    def set_font(self):
        """Bold Segoe UI at the max labels' size: PlotLabel.FONT_POINTS through the theme's Text font size and the
        layout scale, as theirs is (see AppTheme.install), the strip as tall as it needs"""
        points = AppTheme.THEME.map_font(QFont("Orbitron", PlotLabel.FONT_POINTS), AppTheme.ui_scale).pointSizeF()
        self.font = QFont("Segoe UI")
        self.font.setPointSizeF(points)
        self.font.setBold(True)
        self.height = round(QFontMetricsF(self.font).height() + 8)

    def layout(self, *_):
        """Across the top of the plot area (as it's moved, resized or zoomed), at the max labels' size as it is now"""
        area = self.vbox.sceneBoundingRect()
        self.prepareGeometryChange()
        self.set_font()
        self.width = area.width()
        self.setPos(area.left(), area.top() + self.top)
        self.update()

    def boundingRect(self):
        return QRectF(0, 0, self.width, self.height)

    def spans(self):
        """(index, left, right) in the strip's own pixels of each band in view, cut to the strip's ends"""
        start, end = self.vbox.viewRange()[0]
        if end <= start or self.width <= 0:
            return []
        scale = self.width/(end - start)
        spans = []
        for index, (bandStart, bandEnd, _) in enumerate(self.bands):
            left, right = max((bandStart - start)*scale, 0.0), min((bandEnd - start)*scale, self.width)
            if right > left:
                spans.append((index, left, right))
        return spans

    def band_at(self, x):
        return next((index for index, left, right in self.spans() if left <= x < right), None)

    def paint(self, painter, option, widget=None):
        painter.setFont(self.font)
        metrics = QFontMetricsF(self.font)
        #(too narrow for a few letters: no title, hovering still shows it)
        minText = metrics.horizontalAdvance("Abc…")
        for index, left, right in self.spans():
            rect = QRectF(left, 0, right - left, self.height)
            painter.fillRect(rect, self.COLORS[index % 2])
            textWidth = rect.width() - 2*self.PADX
            if textWidth < minText:
                continue
            text = metrics.elidedText(self.bands[index][2], Qt.TextElideMode.ElideRight, textWidth)
            painter.setPen(QColor("white"))
            painter.drawText(rect.adjusted(self.PADX, 0, -self.PADX, 0),
                             Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, text)

    def hoverMoveEvent(self, ev):
        index = self.band_at(ev.pos().x())
        if index is None:
            self.setToolTip("")
            return
        start, end, title = self.bands[index]
        self.setToolTip(f"{title}\n{hourMinuteSecondFromSecond(start)} to {hourMinuteSecondFromSecond(end)}"
                        f"  ({MinimizedDuration(round(end - start))})")

    def mouseClickEvent(self, ev):
        if ev.button() == Qt.MouseButton.LeftButton:
            index = self.band_at(ev.pos().x())
            if index is not None:
                ev.accept()
                self.onClick(index)


class RideStrip(BandStrip):
    """The session's rides (see Session.py), with more than one: each's heading over its stretch, along the top"""
    COLORS = (QColor(18, 18, 48, 225), QColor(52, 70, 110, 225))


class SongStrip(BandStrip):
    """Musical Cadence Mode's songs as they were heard, each's "Artist - Title" in alternating shades of the song
    headers' colors (under the rides' strip)"""
    COLORS = (QColor(40, 39, 94, 215), QColor(72, 98, 140, 215))

    @property
    def songs(self):
        return self.bands


def sensor_pens(sensor):
    """A sensor curve's pen and halo (shadowPen) keyword arguments, see FtpColors.sensor_pens"""
    pen, halo = FtpColors.sensor_pens(FtpColors.SENSOR_COLORS[sensor])
    return {"pen": pen, "shadowPen": halo}


def grade_badge_css(grade, points):
    return (f"background-color:{WorkoutGrading.grade_color(grade)}; border: 2px solid #808080; border-radius: 12px;"
            f" font: 900 {points}pt 'Orbitron'; color: {AppTheme.text_on(WorkoutGrading.grade_color(grade))};"
            " qproperty-alignment: AlignCenter;")


class SynopsisWindow(QWidget):
    #best sustained power durations shown on the stats panel, in seconds
    BEST_POWER_SECONDS = (10, 30, 60, 120, 300, 600, 1200, 1800, 3600)
    #narrowest time span the graph can zoom in to, in seconds
    MIN_ZOOM_SECONDS = 30
    #marker and label border for a max that's only the max of the zoomed-in selection
    SELECTION_COLOR = "#e65100"
    #the dashed line where each of the session's rides after the first starts (RGBA)
    DIVIDER_COLOR = (0, 0, 0, 200) #(in the theme's color for black)

    def __init__(self, parent):
        super(SynopsisWindow, self).__init__()

        #the session's rides (see Session.py), this one joining them: the graph shows them one after the other, each
        #ride's readings from its own start in the session, a divider where each after the first starts
        Session.add_current_ride(getattr(parent, "workoutHudWidget", None))
        self.rides = list(Session.rides())
        self.lastRide = self.rides[-1]
        self.rideStarts = [ride["start"] for ride in self.rides[1:]]
        #a curve for each sensor any of the rides had
        self.showHR = any(ride["hrm"] for ride in self.rides)
        self.showSpeed = any(ride["speed"] for ride in self.rides)

        #speed and cadence helpers; last (revolutions, event time) seen per counter, None until the first reading
        #(of each ride)
        self.speedLast = None
        #cadence comes from a dedicated sensor, or from the power meter's own cadence while there wasn't one
        self.showCadence = any(ride["cadenceSensor"] or len(ride["data"]["fallbackCadence"]) > 0 for ride in self.rides)
        self.cadenceLast = None
        self.totalMeters = 0.0
        #(times, meters) of each accepted speed reading, for the distance of a zoomed-in selection
        self.speedDistance = ([],[])

        #Best 10s,30s,1m,2m,5m,10m,20m,30m,1h Power (same order as BEST_POWER_SECONDS)
        self.bestPowerRange = []
        self.bestText = [" Watts over 10s",
                    " Watts over 30s",
                    " Watts over 1m",
                    " Watts over 2m",
                    " Watts over 5m",
                    " Watts over 10m",
                    " Watts over 20m",
                    " Watts over 30m",
                    " Watts over 1h!"]

        #best power is over unbroken readings (see findBestPowerRange), but a 20m or 1h FTP test's effort keeps its
        #average through a sensor dropout, the same as the test's own average (FtpTest.EffortAverage): those rides'
        #(start, end)
        self.keepAverageSpans = [(ride["start"], ride["start"] + ride["seconds"]) for ride in self.rides
                                 if FtpTest.TESTS.get(ride["ftpTestMode"], {}).get("kind") == "effort"]
        self.dropoutsKeepAverage = bool(self.keepAverageSpans)

        #recalculate all of this info based on the raw data files; each ride's target bars are its own
        #({"ride", "plot", "vbox", "xs", "ys"}, see process_ride)
        self.targetBars = []
        self.processedPowerVals = ([],[])
        self.processedHRVals = ([],[])
        self.processedCadenceVals = ([],[])
        self.processedSpeedVals = ([],[])

        self.parent = parent

        self.synopsisGridLayout = QGridLayout()
        self.synopsisGraphLabel = WorkoutGraph()
        self.synopsisGraphLabel.updateViews()
        self.synopsisGraphLabel.plotScreen.vb.sigResized.connect(self.synopsisGraphLabel.updateViews)
        self.synopsisGraphLabel.plotScreen.hideButtons() #removes the little a in the bottom left of the screen
        #axis titles at the max labels' size, see AppTheme.theme_graph
        self.synopsisGraphLabel.axisTitlePoints = PlotLabel.FONT_POINTS
        #axis titles and tick values start at the system default font's size; their font (family, size %
        #and weight) follows the theme's Text font row, see AppTheme.theme_graph
        if AppState.defaultFont is not None:
            for axisName in ("left", "right", "bottom"):
                axis = self.synopsisGraphLabel.plotScreen.getAxis(axisName)
                axis.setTickFont(AppState.defaultFont)
                axis.label.setFont(AppState.defaultFont)
                axis.setLabel(axis.labelText, axis.labelUnits,
                              **{**axis.labelStyle, "font-family": AppState.defaultFont.family()})
        #transparent plot inside a rounded frame so the plot area gets the same rounded corners as the stats panel
        self.synopsisGraphLabel.setBackground(None)
        self.synopsisGraphLabel.viewport().setAutoFillBackground(False)
        self.synopsisGraphLabel.setStyleSheet("QGraphicsView { border: 0px; background: transparent; }")
        self.synopsisGraphFrame = QFrame()
        self.synopsisGraphFrame.setObjectName("synopsisGraphFrame")
        self.synopsisGraphFrame.setStyleSheet("#synopsisGraphFrame { background-color:#bdd6de; border-radius: 15px; }")
        self.synopsisGraphFrameLayout = QGridLayout(self.synopsisGraphFrame)
        self.synopsisGraphFrameLayout.setContentsMargins(6,6,6,6)
        self.synopsisGraphFrameLayout.addWidget(self.synopsisGraphLabel)

        FtpColors.watch(self, "refresh_ftp_colors")
        #speed is kept in the distance unit it was shown in (converted if that changes, see units_changed); power to
        #weight follows the weight set on the setup screen
        self.speedUnit = Units.current.distance
        Units.watch(self, "units_changed")
        self.powerProducedPlot = pg.PlotCurveItem([0,0])
        if self.showHR:
            self.hrPlot = pg.PlotCurveItem([0,0])
        if self.showCadence:
            self.cadencePlot = pg.PlotCurveItem([0,0])
        if self.showSpeed:
            self.speedPlot = pg.PlotCurveItem([0,0])

        #where the session ends: the last ride's end (a sensor silent that long before it shows zero)
        self.endTime = self.lastRide["start"] + self.lastRide["secondsPassed"]
        #get contents of workout files
        self.process_synopsis_data()

        #the whole session fills the graph, and is as far out as it can zoom; zooming and panning only move
        #time, the y scales stay fixed
        self.totalSeconds = max(sum(ride["seconds"] for ride in self.rides), self.MIN_ZOOM_SECONDS)
        graph = self.synopsisGraphLabel
        for vbox in (graph.plotScreen.vb, graph.powerVBox, graph.hrCadenceVBox):
            vbox.setMouseEnabled(x=True, y=False)
            vbox.setMenuEnabled(False)
            vbox.setLimits(xMin=0, xMax=self.totalSeconds,
                           minXRange=self.MIN_ZOOM_SECONDS, maxXRange=self.totalSeconds)
        graph.plotScreen.vb.setXRange(0, self.totalSeconds, padding=0)
        self.set_tick_spacing(self.totalSeconds)

        #set graph y ranges (a Musical Cadence Mode ride's targets are cadences, on the cadence axis)
        maxWorkoutPower = GetMaxOrZero([y for bar in self.targetBars if bar["vbox"] is graph.powerVBox for y in bar["ys"]])
        maxTargetCadence = GetMaxOrZero([y for bar in self.targetBars if bar["vbox"] is graph.hrCadenceVBox
                                         for y in bar["ys"]])
        maxProducedPower, maxProducedPowerIndex = GetMaxOrZeroAndIndex(self.processedPowerVals[1])
        powerMax = GetMaxOrZero([maxWorkoutPower,maxProducedPower])
        if powerMax == 0:
            powerMax = 400
        #(and again, a little higher if need be, to make room for the max stars, see fit_star_room)
        self.powerTop = powerMax+100
        self.synopsisGraphLabel.powerVBox.setYRange(0,self.powerTop,padding=0)
        maxSavedHr, maxSavedHrIndex = GetMaxOrZeroAndIndex(self.processedHRVals[1])
        maxCadence, maxCadenceIndex = GetMaxOrZeroAndIndex(self.processedCadenceVals[1])
        maxSpeed, maxSpeedIndex = GetMaxOrZeroAndIndex(self.processedSpeedVals[1])
        hrCadenceSpeedMax = GetMaxOrZero([maxSavedHr, maxCadence, maxSpeed, maxTargetCadence])
        if hrCadenceSpeedMax == 0:
            hrCadenceSpeedMax = 190
        self.hrCadenceTop = hrCadenceSpeedMax+20
        self.synopsisGraphLabel.hrCadenceVBox.setYRange(0,self.hrCadenceTop,padding=0)

        #stats live in their own panel beside the graph so they can never overlap the plot or get clipped
        self.datapointsTextbox = QLabel(self.stats_text(0, self.totalSeconds))
        self.datapointsTextbox.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
        #stats keep the previous Segoe UI Black font rather than Orbitron
        statsFontSize = "14pt"
        self.datapointsTextbox.setStyleSheet("background-color:#bdd6de; padding:8px; font: " + statsFontSize + " 'Segoe UI Black';")
        for bar in self.targetBars:
            bar["vbox"].addItem(bar["plot"])
        self.synopsisGraphLabel.powerVBox.addItem(self.powerProducedPlot)
        if self.showHR:
            self.synopsisGraphLabel.hrCadenceVBox.addItem(self.hrPlot)
        if self.showCadence:
            self.synopsisGraphLabel.hrCadenceVBox.addItem(self.cadencePlot)
        if self.showSpeed:
            self.synopsisGraphLabel.hrCadenceVBox.addItem(self.speedPlot)
        #a divider where each ride after the first starts, over the power bars (under a highlighted curve)
        for start in self.rideStarts:
            divider = pg.InfiniteLine(pos=start, angle=90, movable=False)
            AppTheme.theme_pen(divider, pg.mkPen(QColor(*self.DIVIDER_COLOR), width=2, style=Qt.PenStyle.DashLine))
            divider.setZValue(5)
            graph.powerVBox.addItem(divider, ignoreBounds=True)

        #sensor curves can be clicked to highlight the whole line; clicking it again or empty plot space clears it
        self.sensorCurves = [self.powerProducedPlot]
        if self.showHR:
            self.sensorCurves.append(self.hrPlot)
        if self.showCadence:
            self.sensorCurves.append(self.cadencePlot)
        if self.showSpeed:
            self.sensorCurves.append(self.speedPlot)
        self.curvePens = {}
        self.selectedCurve = None
        for curve in self.sensorCurves:
            self.curvePens[curve] = (pg.mkPen(curve.opts['pen']), curve.opts['shadowPen'])
            curve.setClickable(True, width=10)
            curve.sigClicked.connect(self.select_curve)
        self.synopsisGraphLabel.scene().sigMouseClicked.connect(self.plot_clicked)

        #everything drawn on the plot that the max labels must not cover: (kind, viewbox, x values, y values);
        #curves as actually drawn, dropout zeros included
        def drawn(curve):
            xs, ys = curve.getData()
            return ([], []) if xs is None else (list(xs), list(ys))
        self.plotSeries = [("columns", bar["vbox"], bar["xs"], bar["ys"]) for bar in self.targetBars]
        self.plotSeries.append(("curve", self.synopsisGraphLabel.powerVBox, *drawn(self.powerProducedPlot)))
        if self.showHR:
            self.plotSeries.append(("curve", self.synopsisGraphLabel.hrCadenceVBox, *drawn(self.hrPlot)))
        if self.showCadence:
            self.plotSeries.append(("curve", self.synopsisGraphLabel.hrCadenceVBox, *drawn(self.cadencePlot)))
        if self.showSpeed:
            self.speedSeriesIndex = len(self.plotSeries) #(redrawn in another unit, see units_changed)
            self.plotSeries.append(("curve", self.synopsisGraphLabel.hrCadenceVBox, *drawn(self.speedPlot)))

        #along the top (before the max labels, which keep clear of them): the rides' headings with more than one,
        #and under them Musical Cadence Mode's songs heard
        self.strips = []
        self.rideStrip = self.songStrip = None
        if len(self.rides) > 1:
            self.setup_ride_strip()
        self.setup_song_strip()

        #add max value markers; their labels are placed by layout_max_labels() so they never overlap or leave the plot
        #label texts are formats for the max value, so a zoomed-in selection can show its own max
        self.maxMarkers = []
        self._themeColors = (MaxStar.COLOR, MaxStar.EDGE_COLOR) #(LayoutViewer lists them with the colors a theme can change)
        self.add_max_marker(self.synopsisGraphLabel.powerVBox, self.processedPowerVals, maxProducedPowerIndex, maxProducedPower,
                            lambda watts: f"Max Power: {watts:.0f} W" + Units.power_to_weight_text(watts),
                            self.powerProducedPlot)
        if self.showHR:
            self.add_max_marker(self.synopsisGraphLabel.hrCadenceVBox, self.processedHRVals, maxSavedHrIndex, maxSavedHr,
                                "Max Heart Rate: {:.0f} bpm", self.hrPlot)
        if self.showCadence:
            #show a decimal unless the device only ever reported whole-number cadence
            cadenceDecimals = 0 if all(float(c).is_integer() for c in self.processedCadenceVals[1]) else 1
            self.add_max_marker(self.synopsisGraphLabel.hrCadenceVBox, self.processedCadenceVals, maxCadenceIndex, maxCadence,
                                "Max Cadence: {:.%df} RPM" % cadenceDecimals, self.cadencePlot)
        if self.showSpeed:
            self.add_max_marker(self.synopsisGraphLabel.hrCadenceVBox, self.processedSpeedVals, maxSpeedIndex, maxSpeed,
                                lambda speed: f"Max Speed: {speed:.2f} {Units.speed_unit()}", self.speedPlot)
        #re-place labels whenever the plot area moves, resizes or rescales (connected after updateViews above)
        self.synopsisGraphLabel.plotScreen.vb.sigResized.connect(self.layout_max_labels)
        self.synopsisGraphLabel.powerVBox.sigRangeChanged.connect(self.layout_max_labels)
        self.synopsisGraphLabel.hrCadenceVBox.sigRangeChanged.connect(self.layout_max_labels)

        #low battery warnings sit at the bottom of the stats column, in the same panel
        self.batteryWarningLabel = QLabel()
        self.batteryWarningLabel.setAlignment(Qt.AlignmentFlag.AlignBottom | Qt.AlignmentFlag.AlignLeft)
        self.batteryWarningLabel.setStyleSheet("padding:8px; color:" + AppState.gaugeRed + ";")
        self.batteryWarningLabel.hide()
        self.statsLayout = QVBoxLayout()
        self.statsLayout.setContentsMargins(0,0,0,0)
        self.statsLayout.setSpacing(0)
        #the graded rides' grade (see WorkoutGrading.py) heads the stats column; an FTP test isn't graded, its new FTP
        #goes there too when it's the last ride (see FtpTest.py)
        self.ftpTestMode = self.lastRide["ftpTestMode"]
        self.gradedRides = [ride for ride in self.rides if ride["graded"]]
        self.grading = bool(self.gradedRides)
        #every graded ride's grades, keyed (ride index, the ride's AppState.blockGrades key)
        self.blockGrades = {(index, key): record for index, ride in enumerate(self.rides) if ride["graded"]
                            for key, record in ride["blockGrades"].items()}
        if self.grading:
            average = WorkoutGrading.workout_average(self.blockGrades)
            grade = WorkoutGrading.letter(average)
            self.workoutGradeBox = QFrame()
            self.workoutGradeBox.setObjectName("workoutGradeBox")
            self.workoutGradeBox.setStyleSheet("#workoutGradeBox { background-color:#bdd6de; border-radius: 15px; }")
            gradeBoxLayout = QHBoxLayout(self.workoutGradeBox)
            gradeBoxLayout.setContentsMargins(10,8,10,8)
            gradeBoxLayout.setSpacing(14)
            self.workoutGradeBadge = QLabel(grade)
            self.workoutGradeBadge.setFixedSize(96, 76)
            self.workoutGradeBadge.setStyleSheet(grade_badge_css(grade, 26))
            gradeBoxLayout.addWidget(self.workoutGradeBadge)
            title = "Session Grade" if len(self.gradedRides) > 1 else "Workout Grade"
            self.workoutGradeText = QLabel(title + "\n" + ("No blocks graded" if average is None else f"{average:.1f}%"))
            self.workoutGradeText.setStyleSheet("font: 16pt 'Segoe UI Black';")
            gradeBoxLayout.addWidget(self.workoutGradeText, 1)
            self.statsLayout.addWidget(self.workoutGradeBox)
            self.statsLayout.addSpacing(6)
        if self.ftpTestMode:
            self.setup_ftp_test_result()
        self.statsLayout.addWidget(self.datapointsTextbox, 1)
        self.statsLayout.addWidget(self.batteryWarningLabel)

        #the selected (shown) time span, editable to zoom in or out; kept in step with the graph by update_selection()
        #a separate rounded box for each, From stacked above To
        self.startTimeEdit = QTimeEdit()
        self.endTimeEdit = QTimeEdit()
        timeLabels = []
        for text, edit in (("From", self.startTimeEdit), ("To", self.endTimeEdit)):
            edit.setDisplayFormat("H:mm:ss")
            edit.setTimeRange(QTime(0, 0), QTime(0, 0).addSecs(math.ceil(self.totalSeconds)))
            edit.timeChanged.connect(self.time_edit_changed)
            #once done typing, show what the graph actually settled on (its limits can clamp it)
            edit.editingFinished.connect(self.sync_time_edits)
            timeBox = QFrame()
            timeBox.setObjectName("timeBox")
            timeBox.setStyleSheet("#timeBox { background-color:#bdd6de; border-radius: 15px; }"
                                  "QTimeEdit { border-style:solid; border-color:black; border-width:2px; border-radius: 8px;"
                                  " padding: 2px 4px;"
                                  " background-color: qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #bdd6de stop:1 #9fc5d1);"
                                  " font: " + statsFontSize + " 'Segoe UI Black'; }")
            timeBoxLayout = QHBoxLayout(timeBox)
            timeBoxLayout.setContentsMargins(8,8,8,8)
            timeLabel = QLabel(text)
            timeLabels.append(timeLabel)
            timeBoxLayout.addWidget(timeLabel)
            timeBoxLayout.addWidget(edit, 1)
            self.statsLayout.addSpacing(6)
            self.statsLayout.addWidget(timeBox)
        #every block's grade, over the synopsis (see show_block_grades)
        if self.grading:
            self.blockGradesButton = QPushButton("Block Grades")
            self.blockGradesButton.clicked.connect(lambda: self.show_block_grades(True))
            self.statsLayout.addSpacing(6)
            self.statsLayout.addWidget(self.blockGradesButton)
        #an FTP test's new FTP becomes the FTP from here (see apply_ftp)
        if self.ftpTestMode:
            self.applyFtpButton = QPushButton("Apply FTP")
            self.applyFtpButton.clicked.connect(self.apply_ftp)
            #greyed out when there's no different FTP to apply
            self.applyFtpButton.setEnabled(self.newFtp is not None and self.newFtp != self.previousFtp)
            self.statsLayout.addSpacing(6)
            self.statsLayout.addWidget(self.applyFtpButton)
        #the whole workout's max heart rate becomes the Max HR from here, whatever is zoomed in (see set_max_hr)
        self.workoutMaxHr = round(maxSavedHr)
        self.setMaxHrButton = QPushButton("Set Max HR")
        self.setMaxHrButton.clicked.connect(self.set_max_hr)
        #greyed out with no heart rate recorded, one the setup screen wouldn't take, or the Max HR already set
        self.setMaxHrButton.setEnabled(80 <= self.workoutMaxHr < 250 and str(self.workoutMaxHr) != AppState.maxHr)
        self.statsLayout.addSpacing(6)
        self.statsLayout.addWidget(self.setMaxHrButton)
        #same label width in both boxes so the two time fields line up; measured again whenever their
        #font changes, since the stylesheet font only applies once they're polished (measuring here
        #used the smaller default font and clipped "From")
        self.timeLabels = timeLabels
        for label in timeLabels:
            label.installEventFilter(self)
        self.match_time_label_widths()
        self.parent.startExe.batteryChanged.connect(self.update_battery_warning)
        self.update_battery_warning()

        self.synopsisGridLayout.addWidget(self.synopsisGraphFrame, 0,0,14,2)
        self.synopsisGridLayout.addLayout(self.statsLayout, 0,2,14,1)
        self.synopsisGridLayout.setColumnStretch(0, 1)
        self.synopsisGridLayout.setColumnStretch(1, 1)
        #Return to Setup and Exit split the full width, including under the stats column
        self.bottomButtonsLayout = QHBoxLayout()
        self.SynopsisRestartButton = QPushButton("Return to Setup")
        self.SynopsisRestartButton.clicked.connect(self.restart_action)
        self.bottomButtonsLayout.addWidget(self.SynopsisRestartButton)
        self.SynopsisEndButton = QPushButton("Exit")
        self.SynopsisEndButton.clicked.connect(self.confirm_exit)
        self.bottomButtonsLayout.addWidget(self.SynopsisEndButton)
        self.synopsisGridLayout.addLayout(self.bottomButtonsLayout, 15,0,1,3)

        self.setLayout(self.synopsisGridLayout)

        #clicking a graded block on the graph opens its grade (one at a time, see open_grade_popup)
        self.gradePopup = None
        self.gradePopupRegion = None
        self.gradeRegions = []
        self.blockGradesOverlay = None
        if self.grading:
            self.setup_grade_regions()

        #keys (Ctrl shows them), none of them while the Block Grades sheet covers the screen. Left/Right step through
        #the graded blocks; Esc closes an open grade or the sheet first (MainWindow.escape_pressed)
        noSheet = lambda: not self.block_grades_shown()
        KeyShortcuts.add(self.SynopsisRestartButton, Qt.Key.Key_R, label="R", when=noSheet)
        KeyShortcuts.add(self.SynopsisEndButton, Qt.Key.Key_E, label="E", when=noSheet)
        if self.grading:
            KeyShortcuts.add(self.blockGradesButton, Qt.Key.Key_B, label="B", when=noSheet)
            stepping = lambda: bool(self.gradeRegions) and noSheet()
            KeyShortcuts.add(self.synopsisGraphFrame, Qt.Key.Key_Right, label="← →", place="inside", when=stepping,
                             repeat=True, action=lambda: self.step_grade(1))
            KeyShortcuts.add(self.synopsisGraphFrame, Qt.Key.Key_Left, when=stepping, repeat=True,
                             action=lambda: self.step_grade(-1))
        if self.ftpTestMode:
            KeyShortcuts.add(self.applyFtpButton, Qt.Key.Key_A, label="A", when=noSheet)
        KeyShortcuts.add(self.setMaxHrButton, Qt.Key.Key_M, label="M", when=noSheet)
        #Enter in a time box applies it and leaves it, so Left/Right step through the blocks again
        for edit in (self.startTimeEdit, self.endTimeEdit):
            KeyShortcuts.add(edit, KeyShortcuts.ENTER, field=edit, action=self.parent.setFocus)

        #zooming/panning refreshes the selection stats, max labels and time boxes once it settles
        self.selectionTimer = QTimer(self)
        self.selectionTimer.setSingleShot(True)
        self.selectionTimer.setInterval(100)
        self.selectionTimer.timeout.connect(self.update_selection)
        #the max markers' stars: riding to a new max, or twinkling at rest
        self.starTimer = QTimer(self)
        self.starTimer.setInterval(STEP_MS)
        self.starTimer.timeout.connect(self.animate_stars)
        self.starTimer.start()
        self.synopsisGraphLabel.plotScreen.vb.sigXRangeChanged.connect(self.selectionTimer.start)
        self.update_selection()
        QTimer.singleShot(1000, self.fix_size)

    def match_time_label_widths(self):
        labelWidth = max(label.sizeHint().width() for label in self.timeLabels)
        for label in self.timeLabels:
            label.setFixedWidth(labelWidth)

    def eventFilter(self, obj, event):
        if obj in getattr(self, "timeLabels", ()) and event.type() == QEvent.Type.FontChange:
            #after the font change is done, not from inside it
            QTimer.singleShot(0, self.match_time_label_widths)
        if obj is self.synopsisGraphFrame and event.type() == QEvent.Type.Resize:
            QTimer.singleShot(0, self.place_grade_popup)
        if obj is getattr(self, "blockGradesOverlay", None) and event.type() == QEvent.Type.MouseButtonPress:
            #a click outside the grades closes them
            if not self.blockGradesSheet.geometry().contains(event.position().toPoint()):
                self.show_block_grades(False)
                return True
        return super().eventFilter(obj, event)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.layout_block_grades()

    ###FTP test (see FtpTest.py)
    def setup_ftp_test_result(self):
        """Previous FTP, the new one the test works out to, and the difference, heading the stats column. No new FTP
        if the effort had no power readings or wasn't ridden to the end (ended early), or the ramp was under 60s"""
        self.previousFtp = self.lastRide["ftp"]
        result = self.lastRide["ftpTestResult"] or {}
        average = result.get("average")
        self.newFtp = None
        reason = "No power recorded"
        if FtpTest.is_ramp(self.ftpTestMode) and average is None:
            reason = f"Ramp under {FtpTest.RAMP_BEST_SECONDS}s"
        elif average is not None and not result.get("completed"):
            reason = "Effort not completed"
        elif average is not None:
            newFtp = FtpTest.ftp_from_average(self.ftpTestMode, average)
            #(the same range the FTP box on the setup screen takes)
            if 20 <= newFtp < 450:
                self.newFtp = newFtp
            else:
                reason = f"{newFtp} is outside 20-449"
        self.ftpTestReason = reason
        self.ftpTestBox = QFrame()
        self.ftpTestBox.setObjectName("ftpTestBox")
        self.ftpTestBox.setStyleSheet("#ftpTestBox { background-color:#bdd6de; border-radius: 15px; }")
        ftpTestBoxLayout = QVBoxLayout(self.ftpTestBox)
        ftpTestBoxLayout.setContentsMargins(14,8,14,8)
        self.ftpTestText = QLabel(self.ftp_test_text())
        self.ftpTestText.setStyleSheet("font: 16pt 'Segoe UI Black';")
        ftpTestBoxLayout.addWidget(self.ftpTestText)
        self.statsLayout.addWidget(self.ftpTestBox)
        self.statsLayout.addSpacing(6)

    def ftp_test_text(self):
        #(a ramp test can be ridden with no FTP set)
        previousText = str(self.previousFtp) if self.previousFtp else "not set"
        if self.newFtp is not None:
            lines = [f"Previous FTP: {previousText}", f"New FTP: {self.newFtp}" + Units.power_to_weight_text(self.newFtp),
                     f"Difference: {self.newFtp - self.previousFtp:+d}" if self.previousFtp else "Difference: --"]
        else:
            lines = [f"Previous FTP: {previousText}", "New FTP: --", self.ftpTestReason]
        return "\n".join(lines)

    #the ⚙ menu's Measurements (or the weight) changed: speed converted to the new unit, the stats and labels again
    def units_changed(self):
        if self.showSpeed and Units.current.distance != self.speedUnit:
            factor = Units.KM_PER_MILE if Units.current.distance == "km" else 1/Units.KM_PER_MILE
            self.speedUnit = Units.current.distance
            self.processedSpeedVals[1][:] = [speed*factor for speed in self.processedSpeedVals[1]]
            self.speedPlot.setData(*WithDropoutsAsZero(*self.processedSpeedVals, endTime=self.endTime))
            xs, ys = self.speedPlot.getData()
            if xs is not None:
                self.plotSeries[self.speedSeriesIndex] = ("curve", self.synopsisGraphLabel.hrCadenceVBox, list(xs), list(ys))
            for marker in self.maxMarkers:
                if marker["vals"] is self.processedSpeedVals:
                    marker["fullY"] *= factor
                    marker["y"] *= factor
        self.synopsisGraphLabel.set_speed_label()
        if getattr(self, "ftpTestText", None) is not None:
            self.ftpTestText.setText(self.ftp_test_text())
        self.update_selection()

    def apply_ftp(self):
        #set like it was typed in on the setup screen, which saves it and rebuilds the test for the new FTP
        self.parent.setupWidget.apply_ftp(self.newFtp)
        self.previousFtp = self.newFtp
        self.applyFtpButton.setEnabled(False)

    def set_max_hr(self):
        #set like it was typed in on the setup screen, which saves it
        self.parent.setupWidget.apply_max_hr(self.workoutMaxHr)
        self.setMaxHrButton.setEnabled(False)

    def cadence_ride(self, ride=None):
        #a Musical Cadence Mode ride (the last one by default): its targets (and grades) are cadences
        return (ride or self.lastRide)["kind"] == "cadence"

    def target_vbox(self, ride=None):
        """Where a ride's (the last one's by default) target bars and block grades go: the power axis, or the
        cadence axis for a Musical Cadence Mode ride"""
        graph = self.synopsisGraphLabel
        return graph.hrCadenceVBox if self.cadence_ride(ride) else graph.powerVBox

    @staticmethod
    def graph_offset(ride):
        """What to add to one of a ride's AppState.secondsPassed times for the graph's (the session's seconds)"""
        return ride["start"] + ride["rideOffset"]

    def ride_of(self, record):
        """The ride a block grade record is from"""
        return next((ride for ride in self.rides if any(r is record for r in ride["blockGrades"].values())),
                    self.lastRide)

    ###The strips along the top: the rides' headings, the songs heard
    def add_strip(self, strip):
        self.strips.append(strip)
        self.synopsisGraphLabel.scene().addItem(strip)
        if len(self.strips) == 1:
            vb = self.synopsisGraphLabel.plotScreen.vb
            vb.sigResized.connect(self.layout_strips)
            vb.sigXRangeChanged.connect(self.layout_strips)
        self.layout_strips()

    def layout_strips(self, *_):
        """Each strip under the ones above it (their heights follow the max labels' size, see BandStrip.set_font)"""
        top = 0
        for strip in self.strips:
            strip.top = top
            strip.layout()
            top += strip.height

    def setup_ride_strip(self):
        rides = [(ride["start"], ride["start"] + ride["seconds"], Session.heading(ride)) for ride in self.rides]
        self.rideStrip = RideStrip(rides, self.synopsisGraphLabel.plotScreen.vb, self.ride_clicked)
        self.add_strip(self.rideStrip)

    def ride_clicked(self, index):
        start, end, _ = self.rideStrip.bands[index]
        self.zoom_to(start, end)

    ###Musical Cadence Mode's songs (AppState.musicPlayed, see HUDWindow.record_song)
    def setup_song_strip(self):
        songs = [(start + self.graph_offset(ride), end + self.graph_offset(ride), title)
                 for ride in self.rides for title, start, end in ride["songs"] if end > start]
        if not songs:
            return
        self.songStrip = SongStrip(songs, self.synopsisGraphLabel.plotScreen.vb, self.song_clicked)
        self.add_strip(self.songStrip)

    def song_clicked(self, index):
        start, end, _ = self.songStrip.songs[index]
        self.zoom_to(start, end)

    def zoom_to(self, start, end):
        """Zooms the graph to start - end (a ride or a song; no closer than the narrowest zoom), or back out to the
        whole session if it's already showing just that"""
        start, end = max(start, 0.0), min(end, self.totalSeconds)
        span = max(end - start, self.MIN_ZOOM_SECONDS)
        newStart = max(0.0, min(start - (span - (end - start))/2, self.totalSeconds - span))
        viewStart, viewEnd = self.view_range()
        if abs(viewStart - newStart) < 0.5 and abs(viewEnd - (newStart + span)) < 0.5:
            newStart, span = 0.0, self.totalSeconds
        self.synopsisGraphLabel.plotScreen.vb.setXRange(newStart, newStart + span, padding=0)

    @staticmethod
    def song_during(ride, start, end):
        """The title of the ride's song heard longest from start to end (its AppState.secondsPassed), or None"""
        overlaps = [(min(end, songEnd) - max(start, songStart), title) for title, songStart, songEnd in ride["songs"]]
        overlap, title = max(overlaps, default=(0, None))
        return title if overlap > 0 else None

    ###Block grades (see WorkoutGrading.py)
    def setup_grade_regions(self):
        """A clickable region over each graded block's span on the graph, on its ride's target axis, as tall as its
        highest target (free ride and low power blocks get a minimum height so they can still be clicked)"""
        #the open block's outline, in the selection color: one on each axis with blocks, by id(axis)
        self.gradeHighlights = {}
        for key, record in self.blockGrades.items():
            if record["end"] <= record["start"]:
                continue
            ride = self.rides[key[0]]
            vbox = self.target_vbox(ride)
            minHeight = 0.12*vbox.viewRange()[1][1]
            #(grades are timed in the ride's AppState.secondsPassed)
            rect = QRectF(record["start"] + self.graph_offset(ride), 0, record["end"] - record["start"],
                          max(record.get("top", 0), minHeight))
            region = GradeRegion(rect, key, vbox, self.grade_region_clicked)
            vbox.addItem(region, ignoreBounds=True)
            #clicks go to the highest item first: below everything, so the curves and max labels keep theirs
            region.setZValue(-1000)
            self.gradeRegions.append(region)
            if id(vbox) not in self.gradeHighlights:
                highlight = QGraphicsRectItem()
                pen = QPen(QColor(self.SELECTION_COLOR), 3)
                pen.setCosmetic(True)
                highlight.setPen(pen)
                highlight.hide()
                vbox.addItem(highlight, ignoreBounds=True)
                vbox.sigRangeChanged.connect(self.place_grade_popup)
                self.gradeHighlights[id(vbox)] = highlight
        self.synopsisGraphFrame.installEventFilter(self)

    def grade_region_clicked(self, region):
        #clicking the open block again closes it
        if region is self.gradePopupRegion:
            self.close_grade_popup()
        else:
            self.open_grade_popup(region)

    def open_grade_popup(self, region):
        self.close_grade_popup()
        self.gradePopupRegion = region
        highlight = self.gradeHighlights[id(region.vbox)]
        highlight.setRect(region.rect())
        highlight.show()
        record = self.blockGrades[region.key]
        grade = WorkoutGrading.letter(record["pct"])
        name = record["label"]
        rideIndex, key = region.key
        ride = self.rides[rideIndex]
        block = ride["workoutList"][key[0]]
        if block[0] == 3:
            name += f"  ·  rep {key[2] + 1} of {block[1]}"

        popup = QFrame(self.synopsisGraphFrame)
        popup.setObjectName("gradePopup")
        popup.setStyleSheet("#gradePopup { background-color:#bdd6de; border: 2px solid black; border-radius: 14px; }"
                            "QLabel { font: 13pt 'Segoe UI Black'; color: black; background-color: transparent; }")
        popup.setFixedWidth(round(440*AppTheme.ui_scale))
        layout = QVBoxLayout(popup)
        margin = round(12*AppTheme.ui_scale)
        layout.setContentsMargins(margin, margin, margin, margin)
        header = QHBoxLayout()
        header.setSpacing(round(10*AppTheme.ui_scale))
        badge = QLabel(grade)
        badge.setFixedSize(round(74*AppTheme.ui_scale), round(58*AppTheme.ui_scale))
        badge.setStyleSheet(grade_badge_css(grade, 20))
        header.addWidget(badge)
        pctText = "Not graded" if record["pct"] is None else f"{record['pct']:.1f}%"
        #rich text sizes aren't scaled with the stylesheet's, so scale them here
        title = QLabel(f"{name}<br><span style='font-size:{round(20*AppTheme.ui_scale)}pt'>{pctText}</span>")
        title.setWordWrap(True)
        header.addWidget(title, 1)
        closeButton = QPushButton("✕")
        closeButton.setStyleSheet("QPushButton { background: transparent; border: none; padding: 0px;"
                                  " font: 18pt 'Segoe UI Black'; color: black; }")
        closeButton.setCursor(Qt.CursorShape.PointingHandCursor)
        closeButton.clicked.connect(self.close_grade_popup)
        KeyShortcuts.add(closeButton, label="Esc")
        header.addWidget(closeButton, 0, Qt.AlignmentFlag.AlignTop)
        layout.addLayout(header)
        details = QLabel(self.grade_details_html(record, ride))
        details.setTextFormat(Qt.TextFormat.RichText)
        details.setWordWrap(True)
        layout.addWidget(details)
        self.gradePopup = popup
        popup.adjustSize()
        self.place_grade_popup()

    def grade_details_html(self, record, ride=None):
        ride = ride or self.ride_of(record)
        def pct(value):
            return f"{value:.1f}%"
        def points(value, sign):
            return f"{sign}{value:.1f}" if value >= 0.05 else "0.0"
        rows = []
        if record["pct"] is None:
            rows.append(("No power readings", "the whole block"))
        elif record["free"]:
            rows.append((f"Pedaling (over {WorkoutGrading.FREE_RIDE_MIN_WATTS} W)", pct(record["pedaling"])))
        else:
            rows.append(("Average vs target", pct(record["avg"])))
            if record["low"] is not None:
                rows.append(("Low (3 s average)", pct(record["low"])))
                rows.append(("High (3 s average)", pct(record["high"])))
            rows.append(("Avg over target", points(record["overAvg"], "−")))
            if record["low"] is not None:
                rows.append(("Surges", points(record["surge"], "−")))
            if record["penalty"] < record["overAvg"] + record["surge"] - 0.05:
                rows.append(("Over penalty capped at", points(record["penalty"], "−")))
            rows.append(("Intensity bonus", points(record["bonus"], "+")))
        if record["endedEarly"]:
            #only the part ridden before END was graded
            rows.append(("Workout ended at", f"{MinimizedDuration(round(record['ridden']))} of {MinimizedDuration(round(record['duration']))}"))
        rows.append(("Graded time", f"{MinimizedDuration(round(record['graded']))} of {MinimizedDuration(round(record['ridden']))}"))
        html = ""
        #Musical Cadence Mode: the block's song
        song = self.song_during(ride, record["start"], record["end"]) if self.cadence_ride(ride) else None
        if song:
            html += f"<div>♪ {escape(song)}</div>"
        html += "<table width='100%' cellspacing='0' cellpadding='1'>"
        html += "".join(f"<tr><td>{label}</td><td align='right'>{value}</td></tr>" for label, value in rows)
        html += "</table>"
        notes = []
        if record["grace"] >= 0.5:
            notes.append(f"{round(record['grace'])}s grace (block start/end, after a pause)")
        if record["dropped"] >= 0.5:
            notes.append(f"{round(record['dropped'])}s {'cadence' if self.cadence_ride(ride) else 'power'} dropout left out")
        if notes:
            html += f"<div style='font-size:{max(1, round(10*AppTheme.ui_scale))}pt'>" + "  ·  ".join(notes) + "</div>"
        return html

    def place_grade_popup(self, *_):
        """Keeps the open grade just above its block on the graph (below it if there's no room), inside the
        graph's frame; hidden while the block is scrolled out of view"""
        popup, region = self.gradePopup, self.gradePopupRegion
        if popup is None or region is None:
            return
        start, end = self.view_range()
        rect = region.rect()
        if rect.right() < start or rect.left() > end:
            popup.hide()
            return
        middle = (max(rect.left(), start) + min(rect.right(), end))/2
        scenePoint = region.vbox.mapViewToScene(QPointF(middle, rect.bottom()))
        point = self.synopsisGraphLabel.mapTo(self.synopsisGraphFrame,
                                              self.synopsisGraphLabel.mapFromScene(scenePoint))
        frame = self.synopsisGraphFrame.rect()
        width, height = popup.width(), popup.sizeHint().height()
        gap = round(12*AppTheme.ui_scale)
        left = min(max(point.x() - width//2, gap//2), frame.width() - width - gap//2)
        top = point.y() - height - gap
        if top < gap//2:
            top = min(point.y() + gap, frame.height() - height - gap//2)
        popup.setGeometry(left, max(gap//2, top), width, height)
        popup.show()
        popup.raise_()

    def close_grade_popup(self):
        if self.gradePopup is not None:
            self.gradePopup.hide()
            self.gradePopup.deleteLater()
        self.gradePopup = None
        self.gradePopupRegion = None
        for highlight in getattr(self, "gradeHighlights", {}).values():
            highlight.hide()

    def step_grade(self, step):
        """Left/Right: the next/previous graded block's grade opens (the first or last with none open), the graph
        scrolled to it if it's out of view"""
        regions = sorted(self.gradeRegions, key=lambda region: region.rect().left())
        if self.gradePopupRegion in regions:
            index = regions.index(self.gradePopupRegion) + step
            if not 0 <= index < len(regions):
                return
        else:
            index = 0 if step > 0 else len(regions) - 1
        region = regions[index]
        self.scroll_to(region.rect())
        self.open_grade_popup(region)

    def scroll_to(self, rect):
        #zoomed in with the block out of view: the same span, centered on it (its start, if it's longer than that)
        start, end = self.view_range()
        if rect.left() >= start and rect.right() <= end:
            return
        span = end - start
        newStart = rect.left() if rect.width() >= span else rect.center().x() - span/2
        newStart = max(0.0, min(newStart, self.totalSeconds - span))
        self.synopsisGraphLabel.plotScreen.vb.setXRange(newStart, newStart + span, padding=0)

    def block_grades_shown(self):
        return getattr(self, "blockGradesOverlay", None) is not None and self.blockGradesOverlay.isVisible()

    def close_overlays(self):
        """Esc: closes the Block Grades sheet, else an open grade; False if neither was open"""
        if self.block_grades_shown():
            self.show_block_grades(False)
            return True
        if self.gradePopup is not None:
            self.close_grade_popup()
            return True
        return False

    def show_block_grades(self, show):
        """Every block's grade over the synopsis (the Block Grades button), the same list the HUD shows once a
        workout is complete"""
        if not show:
            if self.blockGradesOverlay is not None:
                self.blockGradesOverlay.hide()
            return
        if self.blockGradesOverlay is None:
            overlay = QFrame(self)
            overlay.setObjectName("blockGradesOverlay")
            overlay.setStyleSheet("#blockGradesOverlay { background-color: rgba(20, 20, 50, 140); }")
            overlay.installEventFilter(self)
            sheet = QFrame(overlay)
            sheet.setObjectName("blockGradesSheet")
            sheet.setStyleSheet("#blockGradesSheet { border: 2px solid #fffffe; border-radius: 18px;"
                                " background-color: qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1); }")
            layout = QVBoxLayout(sheet)
            margin = round(16*AppTheme.ui_scale)
            layout.setContentsMargins(margin, margin, margin, margin)
            layout.setSpacing(round(8*AppTheme.ui_scale))
            header = QLabel("Block Grades")
            header.setStyleSheet("font: 900 22pt 'Orbitron'; color: #fffffe; background-color: transparent;")
            layout.addWidget(header)
            self.blockGradeList = GradeList()
            layout.addWidget(self.blockGradeList, 1)
            closeButton = QPushButton("CLOSE")
            closeButton.clicked.connect(lambda: self.show_block_grades(False))
            KeyShortcuts.add(closeButton, Qt.Key.Key_B, label="B")
            layout.addWidget(closeButton)
            self.blockGradesOverlay, self.blockGradesSheet = overlay, sheet
        self.blockGradeList.refresh(rides=self.rides)
        self.layout_block_grades()
        self.blockGradesOverlay.show()
        self.blockGradesOverlay.raise_()

    def layout_block_grades(self):
        if getattr(self, "blockGradesOverlay", None) is None:
            return
        self.blockGradesOverlay.setGeometry(self.rect())
        width = min(round(640*AppTheme.ui_scale), self.width() - 40)
        height = round(self.height()*0.86)
        self.blockGradesSheet.setGeometry((self.width() - width)//2, (self.height() - height)//2, width, height)

    def fix_size(self):
        #(a second after it's built: if it's been left already - RESTART, back to setup - it mustn't show itself again
        #over the screen that replaced it)
        if not self.isVisible():
            return
        self.parent.showMaximized()
        QTimer.singleShot(0, self.layout_max_labels)

    def add_max_marker(self, vbox, vals, index, value, fmt, curve):
        #vals is (times, values); skip when there's no data for this sensor. A star marks the max (see ClassMaxStar.py)
        if len(vals[0]) == 0:
            return
        x = vals[0][index]
        scene = self.synopsisGraphLabel.scene()
        line = QGraphicsLineItem()
        AppTheme.theme_pen(line, QPen(QColor(0, 0, 0, 160), 1))
        line.setZValue(999)
        scene.addItem(line)
        label = PlotLabel(self.marker_text(fmt, value))
        font = QFont("Orbitron", PlotLabel.FONT_POINTS)
        font.setBold(True)
        label.setFont(font)
        label.setZValue(1000)
        #clicking the label highlights its curve, the same as clicking the curve itself
        label.onClick = lambda: self.select_curve(curve)
        scene.addItem(label)
        star = MaxStar(curve, vbox, self.view_range)
        star.go_to(x, value)
        scene.addItem(star)
        self.maxMarkers.append({"label": label, "line": line, "star": star, "vbox": vbox, "vals": vals, "fmt": fmt,
                                "fullX": x, "fullY": value, "x": x, "y": value, "shown": True})

    @staticmethod
    def marker_text(fmt, value):
        #a format for the max value, or a function of it (for texts with units that can change)
        return fmt(value) if callable(fmt) else fmt.format(value)

    def view_range(self):
        return self.synopsisGraphLabel.plotScreen.vb.viewRange()[0]

    def is_zoomed(self, start, end):
        return end - start < self.totalSeconds - 0.5

    def set_tick_spacing(self, span):
        #major = minor spacing, coarser the more time is shown; under a minute the axis shows seconds too
        for longerThan, spacing in ((7200, 900), (3600, 600), (1800, 300), (300, 60), (120, 30), (0, 10)):
            if span > longerThan:
                self.synopsisGraphLabel.plotScreen.getAxis('bottom').setTickSpacing(spacing, spacing)
                return

    def update_selection(self):
        start, end = self.view_range()
        self.set_tick_spacing(end - start)
        self.datapointsTextbox.setText(self.stats_text(start, end))
        self.update_max_markers(start, end)
        self.sync_time_edits()
        self.layout_max_labels()

    def sync_time_edits(self):
        #show the graph's time span, except in a box that's being typed in
        start, end = self.view_range()
        for edit, seconds in ((self.startTimeEdit, start), (self.endTimeEdit, end)):
            if not edit.hasFocus():
                edit.blockSignals(True)
                edit.setTime(QTime(0, 0).addSecs(round(seconds)))
                edit.blockSignals(False)

    def time_edit_changed(self):
        start = QTime(0, 0).secsTo(self.startTimeEdit.time())
        end = QTime(0, 0).secsTo(self.endTimeEdit.time())
        #keep at least the narrowest zoom, moving whichever box wasn't edited
        if end - start < self.MIN_ZOOM_SECONDS:
            if self.sender() is self.startTimeEdit:
                end = min(start + self.MIN_ZOOM_SECONDS, self.totalSeconds)
                start = end - self.MIN_ZOOM_SECONDS
            else:
                start = max(end - self.MIN_ZOOM_SECONDS, 0)
                end = start + self.MIN_ZOOM_SECONDS
        self.synopsisGraphLabel.plotScreen.vb.setXRange(start, end, padding=0)

    def update_max_markers(self, start, end):
        #a max that's scrolled out of view is replaced by the max of the readings in view
        zoomed = self.is_zoomed(start, end)
        for marker in self.maxMarkers:
            times, values = marker["vals"]
            selection = zoomed and not start <= marker["fullX"] <= end
            if selection:
                i0, i1 = bisect.bisect_left(times, start), bisect.bisect_right(times, end)
                shown = i0 < i1
                if shown:
                    index = max(range(i0, i1), key=values.__getitem__)
                    marker["x"], marker["y"] = times[index], values[index]
            else:
                shown = True
                marker["x"], marker["y"] = marker["fullX"], marker["fullY"]
            marker["shown"] = marker["star"].shown = shown
            for item in (marker["label"], marker["line"]):
                item.setVisible(shown)
            if not shown:
                continue
            if selection:
                marker["label"].setText("(Selection) " + self.marker_text(marker["fmt"], marker["y"]))
                marker["label"].setBorderColor(QColor(self.SELECTION_COLOR), 2.5)
            else:
                marker["label"].setText(self.marker_text(marker["fmt"], marker["y"]))
                marker["label"].setBorderColor(PlotLabel.borderColor, 1.2)
            marker["star"].set_color(self.SELECTION_COLOR if selection else MaxStar.COLOR)
            marker["star"].go_to(marker["x"], marker["y"])

    def fit_star_room(self):
        #each graph's top high enough for its stars over the whole ride's maxes (a selection's are no higher), under
        #the strips along the top
        graph = self.synopsisGraphLabel
        reserved = sum(strip.height for strip in self.strips)
        for vbox, top in ((graph.powerVBox, self.powerTop), (graph.hrCadenceVBox, self.hrCadenceTop)):
            stars = [(marker["star"], marker["fullY"]) for marker in self.maxMarkers if marker["vbox"] is vbox]
            top = MaxStar.top_with_room(vbox, top, stars, reserved)
            if abs(vbox.viewRange()[1][1] - top) > 1e-6:
                vbox.setYRange(0, top, padding=0)

    def animate_stars(self):
        if not self.isVisible():
            return
        now = time.monotonic()
        for i, marker in enumerate(self.maxMarkers):
            marker["star"].step(now, i)

    def stats_text(self, start, end):
        #stats for the whole ride, or recalculated for just the readings in view when zoomed in
        zoomed = self.is_zoomed(start, end)

        def inView(vals):
            i0, i1 = bisect.bisect_left(vals[0], start), bisect.bisect_right(vals[0], end)
            return vals[0][i0:i1], vals[1][i0:i1]

        text = "(Selection)\n" if zoomed else ""
        if self.showSpeed:
            meters = sum(inView(self.speedDistance)[1]) if zoomed else self.totalMeters
            distance = meters * 0.000621371 #0.000621371 miles per meter
            text += f"Distance: {round(Units.distance(distance), 2)} {Units.distance_unit(long=False)}\n"
        rideSeconds = sum(ride["secondsPassed"] for ride in self.rides)
        text += "Total Time: " + MinimizedDuration(round(end - start) if zoomed else rideSeconds) + "\n"
        #averages over the recorded readings, only for sensors that recorded any
        def average(vals):
            values = inView(vals)[1]
            return sum(values) / len(values) if values else None
        averages = [("Average Power: {:.0f} W", average(self.processedPowerVals))]
        averagePower = averages[0][1]
        if self.showHR:
            averages.append(("Average HR: {:.0f} bpm", average(self.processedHRVals)))
        if self.showSpeed:
            averages.append(("Average Speed: {:.2f} " + Units.speed_unit(), average(self.processedSpeedVals)))
        if self.showCadence:
            averages.append(("Average Cadence: {:.0f} RPM", average(self.processedCadenceVals)))
        for fmt, value in averages:
            if value is not None:
                text += fmt.format(value) + (Units.power_to_weight_text(value) if value is averagePower else "") + "\n"
        #add the best power metrics; a selection only has the durations it spans
        bestPowerRange = self.bestPowerRange
        if zoomed:
            bestPowerRange = []
            powerInView = inView(self.processedPowerVals)
            for targetTime in self.BEST_POWER_SECONDS:
                bestAvePower = self.findBestPowerRange(targetTime, *powerInView)
                if bestAvePower is None:
                    break
                bestPowerRange.append(round(bestAvePower,2))
        text += "Best sustained power values:\n"
        for i, bestAvePower in enumerate(bestPowerRange):
            text +=  "   " + str(round(bestAvePower,2)) + self.bestText[i] + Units.power_to_weight_text(bestAvePower) + "\n"
        return text.rstrip("\n")

    def select_curve(self, curve, ev=None):
        #toggle: clicking the highlighted line again clears the highlight
        previous = self.selectedCurve
        if previous is not None:
            previous.setPen(self.curvePens[previous][0])
            previous.setShadowPen(self.curvePens[previous][1])
            previous.setZValue(0)
            self.selectedCurve = None
        if curve is not None and curve is not previous:
            pen = pg.mkPen(self.curvePens[curve][0])
            pen.setWidthF(4)
            curve.setPen(pen)
            curve.setShadowPen(pg.mkPen(FtpColors.halo_color(pen.color().name(), 0xff), width=8))
            curve.setZValue(10)
            self.selectedCurve = curve

    def plot_clicked(self, ev):
        #a left click that no curve took clears the highlight
        if not ev.isAccepted() and ev.button() == Qt.MouseButton.LeftButton:
            self.select_curve(None)
            #and closes an open block grade (a click on a block opens its own, see GradeRegion)
            self.close_grade_popup()

    def rect_hits_data(self, rect):
        #True if a scene rect would cover any plotted curve or any workout expected power column
        r = rect.adjusted(-3, -3, 3, 3)
        for kind, vbox, xs, ys in self.plotSeries:
            if len(xs) < 2 or len(ys) == 0:
                continue
            topLeft = vbox.mapSceneToView(r.topLeft())
            bottomRight = vbox.mapSceneToView(r.bottomRight())
            xl, xr = topLeft.x(), bottomRight.x()
            yTop, yBot = topLeft.y(), bottomRight.y()
            if kind == "columns":
                #column i spans xs[i]..xs[i+1] and is filled from 0 up to ys[i]
                i = max(bisect.bisect_right(xs, xl) - 1, 0)
                while i < len(ys) and xs[i] < xr:
                    if xs[i+1] > xl and ys[i] >= yBot and yTop >= 0:
                        return True
                    i += 1
            else:
                #check each line segment that crosses the rect's x range, clipped to that range
                i0 = max(bisect.bisect_left(xs, xl) - 1, 0)
                i1 = min(bisect.bisect_right(xs, xr), len(xs) - 1)
                for i in range(i0, i1):
                    x0, y0, x1, y1 = xs[i], ys[i], xs[i+1], ys[i+1]
                    if x1 < xl or x0 > xr:
                        continue
                    if x1 == x0:
                        lo, hi = min(y0, y1), max(y0, y1)
                    else:
                        ya = y0 + (y1 - y0) * (max(x0, xl) - x0) / (x1 - x0)
                        yb = y0 + (y1 - y0) * (min(x1, xr) - x0) / (x1 - x0)
                        lo, hi = min(ya, yb), max(ya, yb)
                    if hi >= yBot and lo <= yTop:
                        return True
        return False

    def layout_max_labels(self, *_):
        #place each max label in the free spot nearest its marker: inside the plot area, not overlapping
        #another label, and not covering any plot line or workout power column
        #(the strips along the top first: a new theme can change their size with the labels', see AppTheme.apply)
        self.layout_strips()
        self.fit_star_room()
        area = self.synopsisGraphLabel.plotScreen.vb.sceneBoundingRect()
        area.setTop(area.top() + sum(strip.height for strip in self.strips))
        if area.width() < 20 or area.height() < 20:
            return
        gap = 12
        #the stars' resting spots too, so no label covers one
        placed = [marker["star"].rest_rect(marker["vbox"].mapViewToScene(QPointF(marker["x"], marker["y"])))
                  for marker in self.maxMarkers if marker["shown"]]
        for marker in self.maxMarkers:
            if not marker["shown"]:
                continue
            label, line, vbox, x, y = marker["label"], marker["line"], marker["vbox"], marker["x"], marker["y"]
            #(the star marking it, resting over it)
            pt = marker["star"].rest_rect(vbox.mapViewToScene(QPointF(x, y))).center()
            br = label.boundingRect()
            w, h = br.width(), br.height()
            maxLeft = max(area.left(), area.right() - w)
            maxTop = max(area.top(), area.bottom() - h)

            def clampLeft(l):
                return min(max(l, area.left()), maxLeft)

            def clampTop(t):
                return min(max(t, area.top()), maxTop)

            #candidate positions: the spots hugging the marker plus a grid over the whole plot area
            xStep, yStep = max(w / 4, 10), max(h / 2, 6)
            lefts = {clampLeft(pt.x() + gap), clampLeft(pt.x() - gap - w)}
            lefts.update(area.left() + i * xStep for i in range(int((maxLeft - area.left()) / xStep) + 1))
            lefts.add(maxLeft)
            tops = {clampTop(pt.y() - gap - h), clampTop(pt.y() + gap)}
            tops.update(area.top() + i * yStep for i in range(int((maxTop - area.top()) / yStep) + 1))
            tops.add(maxTop)

            def distance(c):
                dx, dy = c.center().x() - pt.x(), c.center().y() - pt.y()
                return dx * dx + dy * dy

            candidates = sorted((QRectF(l, t, w, h) for l in lefts for t in tops), key=distance)
            notOnLabels = [c for c in candidates if not any(c.intersects(p) for p in placed)]
            rect = next((c for c in notOnLabels if not self.rect_hits_data(c)), None)
            if rect is None:
                #plot is too crowded for a fully clear spot; at least keep clear of the other labels
                rect = notOnLabels[0] if notOnLabels else candidates[0]
            placed.append(rect.adjusted(-2, -2, 2, 2))
            label.setPos(rect.left() - br.left(), rect.top() - br.top())
            #leader line from the marker to the nearest edge of its label
            nearest = QPointF(min(max(pt.x(), rect.left()), rect.right()),
                              min(max(pt.y(), rect.top()), rect.bottom()))
            line.setLine(QLineF(pt, nearest))
            line.setVisible(area.contains(pt))


    #get data from the workout as it was processed
    def refresh_ftp_colors(self):
        #the bars' colors (AppState.graphWorkoutBrushDark) are worked out again, each ride's in its own zones
        for bar in self.targetBars:
            bar["plot"].recolor()

    def process_synopsis_data(self):
        #each ride's, from where it starts in the session
        for ride in self.rides:
            self.process_ride(ride)
        #a sensor dropout mid-ride interleaves the two cadence sources, so put them back in time order
        if self.processedCadenceVals[0]:
            times, values = zip(*sorted(zip(*self.processedCadenceVals)))
            self.processedCadenceVals = (list(times), list(values))
        #plots get long dropouts as zero (see WithDropoutsAsZero); the processed lists stay real readings
        #only, for the best power and max value stats
        self.hrPlot = pg.PlotCurveItem(*WithDropoutsAsZero(*self.processedHRVals, endTime=self.endTime),
                                            **sensor_pens("hr"))
        self.powerProducedPlot = pg.PlotCurveItem(*WithDropoutsAsZero(*self.processedPowerVals, endTime=self.endTime),
                                            **sensor_pens("power"))
        self.cadencePlot = pg.PlotCurveItem(*WithDropoutsAsZero(*self.processedCadenceVals, endTime=self.endTime),
                                            **sensor_pens("cadence"))
        self.speedPlot = pg.PlotCurveItem(*WithDropoutsAsZero(*self.processedSpeedVals, endTime=self.endTime),
                                            **sensor_pens("speed"))

        #Get averages. Durations are increasing, so stop at the first one the
        #recorded power doesn't span - the stats text pairs bestPowerRange with
        #its labels by position, so only trailing entries may be left out
        for targetTime in self.BEST_POWER_SECONDS:
            bestAvePower = self.findBestPowerRange(targetTime)
            if bestAvePower is None:
                break
            self.bestPowerRange.append(round(bestAvePower,2))

    def process_ride(self, ride):
        """A ride's target bars and readings (see Session.py), its seconds from its start in the session"""
        offset = ride["start"]
        startTimestamp = ride["startTimestamp"]
        startSeconds = startTimestamp.timestamp()
        data = ride["data"]
        powerFrom, cadenceFrom = len(self.processedPowerVals[0]), len(self.processedCadenceVals[0])

        #the target bars, each target from when it was asked for (the times have one more: the first's start)
        _, targets, targetTimes = ride["powerRequests"]
        if targets and targetTimes:
            xs = [offset + (targetTimes[0] - startTimestamp).total_seconds()]
            ys = []
            for index in range(len(targets)):
                deltaSeconds = (targetTimes[index+1] - startTimestamp).total_seconds()
                if deltaSeconds > 0:
                    xs.append(offset + deltaSeconds)
                    ys.append(int(targets[index]))
            if ys:
                plot = SolidColoredColumns(x=xs, y=ys, ranges=ride["targetRanges"], colors=AppState.graphWorkoutBrushDark)
                self.targetBars.append({"ride": ride, "plot": plot, "vbox": self.target_vbox(ride), "xs": xs, "ys": ys})

        #replay the sensor history recorded during the ride (see WorkoutData.py)
        if ride["hrm"]:
            lastNonZeroVal = 0
            lastNonZeroValIndex = 0
            for timestamp, hr in data["heartRate"].rows():
                #(the same hold-last-value smoothing as the live gauge: a 0 is only held over for a few readings)
                val = hr
                if hr == 0:
                    if lastNonZeroValIndex < HOLD_HR_READINGS:
                        #possible misread, keep the last value for a few cycles
                        val = lastNonZeroVal
                        lastNonZeroValIndex += 1
                else:
                    #reset the counter
                    lastNonZeroValIndex = 0
                    lastNonZeroVal = hr
                deltaSeconds = timestamp - startSeconds
                if deltaSeconds > 0:
                    self.processedHRVals[0].append(offset + deltaSeconds)
                    self.processedHRVals[1].append(val)

        lastNonZeroVal = 0
        lastNonZeroValIndex = 0
        for timestamp, watts in data["power"].rows():
            #(as for heart rate above: coasting's 0s show as 0 once they've gone on for more than a few readings)
            val = watts
            if watts == 0:
                if lastNonZeroValIndex < HOLD_POWER_READINGS:
                    #possible misread, keep the last value for a few cycles
                    val = lastNonZeroVal
                    lastNonZeroValIndex += 1
            else:
                #reset the counter
                lastNonZeroValIndex = 0
                lastNonZeroVal = watts
            deltaSeconds = timestamp - startSeconds
            if deltaSeconds > 0:
                self.processedPowerVals[0].append(offset + deltaSeconds)
                self.processedPowerVals[1].append(val)

        #(each ride's counters start over, on its own wheel)
        self.speedLast = None
        self.cadenceLast = None
        self.wheelCircumference = ride["wheelCircumference"]
        self.lastNonZeroSpeed = 0
        self.lastNonZeroSpeedIndex = 0
        self.lastNonZeroCadence = 0
        self.lastNonZeroCadenceIndex = 0
        if ride["speedCadence"]:
            #combined (121) sensor: both counters arrive together in one row
            for timestamp, speedRevs, speedTime, cadenceRevs, cadenceTime in data["speedCadence"].rows():
                deltaSeconds = timestamp - startSeconds
                if deltaSeconds > 0:
                    self.process_speed_reading(offset + deltaSeconds, speedRevs, speedTime)
                    #Early Exit for odd error values (only ever seen on the combined sensor's cadence time)
                    if cadenceTime not in [513, 2305]:
                        self.process_cadence_reading(offset + deltaSeconds, cadenceRevs, cadenceTime)
            #discrete cadence (122) / speed (123) sensors
            for timestamp, cadenceRevs, cadenceTime in data["cadence"].rows():
                deltaSeconds = timestamp - startSeconds
                if deltaSeconds > 0:
                    self.process_cadence_reading(offset + deltaSeconds, cadenceRevs, cadenceTime)
            for timestamp, speedRevs, speedTime in data["speed"].rows():
                deltaSeconds = timestamp - startSeconds
                if deltaSeconds > 0:
                    self.process_speed_reading(offset + deltaSeconds, speedRevs, speedTime)
        #power meter cadence, used while there was no dedicated cadence sensor reading (and all of it in Musical
        #Cadence Mode without a speed/cadence sensor)
        for timestamp, cadence in data["fallbackCadence"].rows():
            deltaSeconds = timestamp - startSeconds
            if deltaSeconds > 0:
                self.add_cadence_value(offset + deltaSeconds, cadence)

        #a ride with no ring of its own (a free ride) gets one from its readings, for the next rides' spiral
        if ride["ring"] is None:
            power = ([t - offset for t in self.processedPowerVals[0][powerFrom:]], self.processedPowerVals[1][powerFrom:])
            cadence = sorted(zip(self.processedCadenceVals[0][cadenceFrom:], self.processedCadenceVals[1][cadenceFrom:]))
            cadence = ([t - offset for t, _ in cadence], [c for _, c in cadence])
            ride["ring"] = Session.ring_from_readings(ride["seconds"], power, cadence, ride["speedCadence"])

    def process_speed_reading(self, deltaSeconds, cumulativeSpeedRevolutions, speedEventTime):
        if self.speedLast is None:
            self.speedLast = (cumulativeSpeedRevolutions, speedEventTime)
        # Get Deltas; both counters are 16 bit and wrap at 65536 (event times every 64s), so
        # mask the difference - the same rollover-safe math AntDevicesPoller uses live
        deltaRevCount = (cumulativeSpeedRevolutions - self.speedLast[0]) & 0xFFFF
        deltaSpeedEventTime = (speedEventTime - self.speedLast[1]) & 0xFFFF
        # Update LAST values - before rejecting anything, the same as the live
        # gauge, so one bad reading (or a sensor's counters resetting) can't
        # poison every delta after it
        self.speedLast = (cumulativeSpeedRevolutions, speedEventTime)

        if deltaSpeedEventTime > 0:
            #speed in MPH, 2290.62656 constant is because time events are in 1/1024 seconds units x 2.23694mph=1m/s
            speed = (self.wheelCircumference * deltaRevCount * 2290.62656) / deltaSpeedEventTime
            if speed > 60: #remove any crazy values - and their revolutions from the distance, as live
                return
            #distance only counts revolutions from accepted readings, as live (in meters: each ride on its own wheel)
            meters = self.wheelCircumference * deltaRevCount
            self.totalMeters += meters
            self.speedDistance[0].append(deltaSeconds)
            self.speedDistance[1].append(meters)
            if speed < 5:
                if self.lastNonZeroSpeedIndex < HOLD_SPEED_READINGS:
                    #possible misread, keep the last value for a few cycles
                    #(same hold-last-value smoothing the live gauge uses,
                    #so the post-workout graph matches what was shown
                    #during the workout instead of showing raw dropouts)
                    speed = self.lastNonZeroSpeed
                    self.lastNonZeroSpeedIndex += 1
            else:
                #reset the counter
                self.lastNonZeroSpeed = speed
                self.lastNonZeroSpeedIndex = 0
            self.processedSpeedVals[0].append(deltaSeconds)
            self.processedSpeedVals[1].append(Units.speed(speed))

    def process_cadence_reading(self, deltaSeconds, cumulativeCadenceRevolutions, cadenceEventTime):
        if self.cadenceLast is None:
            self.cadenceLast = (cumulativeCadenceRevolutions, cadenceEventTime)
        # Get Deltas, rollover-safe as in process_speed_reading
        deltaCadenceRevCount = (cumulativeCadenceRevolutions - self.cadenceLast[0]) & 0xFFFF
        deltaCadenceEventTime = (cadenceEventTime - self.cadenceLast[1]) & 0xFFFF
        # Update LAST values first, see process_speed_reading
        self.cadenceLast = (cumulativeCadenceRevolutions, cadenceEventTime)

        if deltaCadenceEventTime > 0:
            #cadence revs/minute, 61440 time events are in 1/1024 x 60 seconds/minute
            #(add_cadence_value drops anything over 200)
            cadence = (61440 * deltaCadenceRevCount) / deltaCadenceEventTime
            self.add_cadence_value(deltaSeconds, cadence)

    def add_cadence_value(self, deltaSeconds, cadence):
        if cadence > 200: #remove any crazy values
            return
        if cadence < 10:
            if self.lastNonZeroCadenceIndex < HOLD_CADENCE_READINGS:
                #possible misread, keep the last value for a few cycles
                #(same hold-last-value smoothing the live gauge uses)
                cadence = self.lastNonZeroCadence
                self.lastNonZeroCadenceIndex += 1
        else:
            #reset the counter
            self.lastNonZeroCadence = cadence
            self.lastNonZeroCadenceIndex = 0
        self.processedCadenceVals[0].append(deltaSeconds)
        self.processedCadenceVals[1].append(cadence)

    def findBestPowerRange(self, targetTime, times=None, watts=None):
        """Highest average power over any stretch of the session at least
        targetTime seconds long, or None when the recorded power doesn't
        span targetTime (sensor connected late, dropped out, short ride).
        Searches the whole session's power unless given (times, watts).
        A stretch is unbroken readings of one ride: the next ride, or a
        sensor dropout (readings further apart than DROPOUT_ZERO_SECONDS,
        where the graph drops to zero too) ends one - except in a 20m or 1h
        FTP test's ride (keepAverageSpans), where the power is taken to have
        stayed at the stretch's average through a dropout"""
        if times is None:
            times, watts = self.processedPowerVals
        best = None
        total = 0
        startIndex = 0
        starts = self.rideStarts
        nextStart = bisect.bisect_right(starts, times[0]) if len(times) else 0
        for endIndex in range(len(times)):
            #(the first reading of the next ride)
            newRide = False
            while nextStart < len(starts) and starts[nextStart] <= times[endIndex]:
                nextStart += 1
                newRide = True
            dropout = endIndex > 0 and times[endIndex] - times[endIndex - 1] > DROPOUT_ZERO_SECONDS and \
                not any(start <= times[endIndex] < end for start, end in self.keepAverageSpans)
            if endIndex > startIndex and (newRide or dropout):
                #the next stretch starts with this reading
                startIndex, total = endIndex, 0
            total += watts[endIndex]
            #drop readings off the front while the window would still span targetTime without them
            while endIndex > startIndex and times[endIndex] - times[startIndex+1] >= targetTime:
                total -= watts[startIndex]
                startIndex += 1
            if times[endIndex] - times[startIndex] >= targetTime:
                average = total/(endIndex - startIndex + 1)
                if best is None or average > best:
                    best = average
        return best

    def update_battery_warning(self):
        warnings = self.parent.startExe.low_battery_warnings()
        self.batteryWarningLabel.setText("\n".join(warnings))
        self.batteryWarningLabel.setVisible(bool(warnings))

    def restart_action(self):
        self.parent.workoutHudWidget.restart_workout()
        self.parent.go_back_to_setup()
        self.hide()

    def confirm_exit(self):
        self.w = ConfirmExitWindow(self)
        self.w.show()