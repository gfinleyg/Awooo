"""
FtpColorsEditor.py

LayoutViewer.py's FTP Colors tab and its mockup window (not part of AntWorkout): the colors that follow
the power zones (FtpColors.py) on the power gauge, the workout block labels, the workout graph's bars and
the spiral, edited on the tab and shown on the mockup for any FTP (40 - 500 W) and power.
"""

from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import QColor, QFont, QPainter, QPen
from PyQt6.QtWidgets import (QColorDialog, QGridLayout, QGroupBox, QHBoxLayout, QLabel, QPushButton,
                             QScrollArea, QSizePolicy, QSlider, QSpinBox, QVBoxLayout, QWidget)

import AppTheme
import FtpColors
from AppState import AppState
from ClassAnalogGauge import Gauge
from ClassSpiralProgressBar import SpiralProgressBar

#the zones' limits as % of FTP (ClassWorkoutSetupWindow.py sets AppState.ftpRanges the same way)
ZONE_LIMITS = (36, 87, 119, 187)
ZONE_NAMES = {"0": f"Zone 1  (under {ZONE_LIMITS[0]}%)", "1": f"Zone 2  ({ZONE_LIMITS[0]}–{ZONE_LIMITS[1]}%)",
              "2": f"Zone 3  ({ZONE_LIMITS[1]}–{ZONE_LIMITS[2]}%)", "3": f"Zone 4  ({ZONE_LIMITS[2]}–{ZONE_LIMITS[3]}%)",
              "4": f"Zone 5  ({ZONE_LIMITS[3]}% and up)", "free": "Free ride  (0 W)"}
#the spiral's other lines (ClassHUDWindow.py)
SPIRAL_PROGRESS = "#1f15e1"
GRAPH_BACKGROUND = "#bdd6de" #ClassWorkoutGraph.py
DEFAULT_FTP = 220


def ranges_for(ftp):
    return [ftp*limit/100 for limit in ZONE_LIMITS]


def set_sheet(widget, sheet):
    #straight to Qt: these show the zone colors themselves, not the theme's color replacements
    AppTheme.original_set_stylesheet(widget, sheet)


class Swatch(QPushButton):
    """A color button: click to pick a new color (applied live while picking)"""
    def __init__(self, tip, onPick):
        super().__init__()
        self.onPick = onPick
        self.color = "#000000"
        self.setFixedSize(40, 24)
        self.setToolTip(tip)
        self.clicked.connect(self.pick)

    def set_color(self, color):
        self.color = color
        set_sheet(self, f"background-color:{color}; border:1px solid #444; border-radius:4px;")

    def pick(self):
        before = self.color
        dialog = QColorDialog(QColor(before), self)
        dialog.setWindowTitle(self.toolTip())
        dialog.setOption(QColorDialog.ColorDialogOption.DontUseNativeDialog)
        dialog.currentColorChanged.connect(lambda c: self.onPick(c.name()))
        if not dialog.exec():
            self.onPick(before)


class GradientBar(QWidget):
    """Two colors blended, as the part shows them"""
    def __init__(self):
        super().__init__()
        self.colors = ("#ffffff", "#ffffff")
        self.setMinimumSize(90, 24)

    def set_colors(self, colors):
        self.colors = tuple(colors)
        self.update()

    def paintEvent(self, event):
        from PyQt6.QtGui import QLinearGradient
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        gradient = QLinearGradient(rect.topLeft(), rect.topRight())
        for i, color in enumerate(self.colors):
            gradient.setColorAt(i/max(1, len(self.colors) - 1), QColor(color))
        painter.setBrush(gradient)
        painter.setPen(QPen(QColor("#444444")))
        painter.drawRoundedRect(rect, 4, 4)


###----------------------------------------------------------------------------------------------
### The tab
###----------------------------------------------------------------------------------------------
class FtpColorsTab(QScrollArea):
    """Every power zone color, grouped by the part that shows it"""
    def __init__(self):
        super().__init__()
        self.setWidgetResizable(True)
        self.refreshers = [] #redraw the rows from FtpColors
        body = QWidget()
        layout = QVBoxLayout(body)
        note = QLabel("Colors that follow the power zones (by % of FTP). The FTP Colors window shows them "
                      "on the parts for any FTP and power. Saved with the theme.")
        note.setWordWrap(True)
        layout.addWidget(note)
        layout.addWidget(self.build_gauge_band())
        layout.addWidget(self.build_pairs("gaugeArc", "Power gauge: Ride At target arc",
                                          ("Arc start", "Arc end"), "The arc around the target power on the gauge"))
        layout.addWidget(self.build_singles("gaugeRing", "Power gauge: ring behind the reading",
                                            "By the power being ridden (shown see-through)"))
        layout.addWidget(self.build_blocks())
        layout.addWidget(self.build_graph())
        layout.addWidget(self.build_pairs("spiral", "Spiral: current block's line",
                                          ("Line", "Blinking"), "The block's line, and its color while it blinks "
                                          "(shown see-through)", preview=False))
        resetAll = QPushButton("Reset all FTP colors")
        resetAll.clicked.connect(self.reset_all)
        layout.addWidget(resetAll)
        layout.addStretch(1)
        self.setWidget(body)
        FtpColors.watch(self, "refresh")
        self.refresh()

    def refresh(self):
        for refresh in self.refreshers:
            refresh()

    def reset_all(self):
        for key in FtpColors.DEFAULTS:
            AppTheme.THEME.ftp.pop(key, None)
        FtpColors.notify()

    def reset_button(self, key, zone=None):
        button = QPushButton("↺")
        button.setFixedWidth(28)
        button.setToolTip("Back to the original")
        button.clicked.connect(lambda: FtpColors.set_value(key, None, zone))
        self.refreshers.append(lambda: button.setEnabled(not FtpColors.is_default(key, zone)))
        return button

    def build_gauge_band(self):
        box = QGroupBox("Power gauge: colored band")
        grid = QGridLayout(box)
        note = QLabel("Each color sits exactly where the needle points at its % of FTP, for any FTP. "
                      "When a color is past the end of the dial (1000 W), the end of the dial is that color.")
        note.setWordWrap(True)
        grid.addWidget(note, 0, 0, 1, 4)
        spins, swatches = [], []
        def changed(index, pct=None, color=None):
            stops = FtpColors.get("gaugeScale")
            if pct is not None:
                stops[index][0] = pct
            if color is not None:
                stops[index][1] = color
            FtpColors.set_value("gaugeScale", stops)
        for i in range(len(FtpColors.DEFAULTS["gaugeScale"])):
            spin = QSpinBox()
            spin.setRange(1, 500)
            spin.setSuffix("% of FTP")
            spin.setMinimumWidth(130)
            spin.valueChanged.connect(lambda v, i=i: changed(i, pct=v))
            swatch = Swatch(f"Band color {i + 1}", lambda c, i=i: changed(i, color=c))
            grid.addWidget(spin, i + 1, 0)
            grid.addWidget(swatch, i + 1, 1)
            spins.append(spin)
            swatches.append(swatch)
        grid.addWidget(self.reset_button("gaugeScale"), 1, 3)
        grid.setColumnStretch(2, 1)
        def refresh():
            for (pct, color), spin, swatch in zip(FtpColors.get("gaugeScale"), spins, swatches):
                spin.blockSignals(True)
                spin.setValue(int(pct))
                spin.blockSignals(False)
                swatch.set_color(color)
        self.refreshers.append(refresh)
        return box

    def zone_rows(self, title, note, key, headers, zones, makeCells):
        box = QGroupBox(title)
        grid = QGridLayout(box)
        row = 0
        if note:
            label = QLabel(note)
            label.setWordWrap(True)
            grid.addWidget(label, row, 0, 1, len(headers) + 3)
            row += 1
        for col, header in enumerate(headers):
            grid.addWidget(QLabel(f"<small>{header}</small>"), row, col + 1)
        row += 1
        for zone in zones:
            grid.addWidget(QLabel(ZONE_NAMES[zone]), row, 0)
            col = 1
            for widget in makeCells(zone):
                grid.addWidget(widget, row, col)
                col += 1
            grid.addWidget(self.reset_button(key, zone), row, col)
            row += 1
        grid.setColumnStretch(0, 1)
        return box, grid, row

    def build_pairs(self, key, title, headers, note, preview=True):
        zones = list(FtpColors.DEFAULTS[key])
        def cells(zone):
            swatches = [Swatch(f"{ZONE_NAMES[zone]}: {header}", lambda c, z=zone, i=i: self.set_part(key, z, i, c))
                        for i, header in enumerate(headers)]
            bar = GradientBar() if preview else None
            def refresh():
                colors = FtpColors.get(key)[zone]
                for swatch, color in zip(swatches, colors):
                    swatch.set_color(color)
                if bar:
                    bar.set_colors(colors)
            self.refreshers.append(refresh)
            return swatches + ([bar] if bar else [])
        box, _, _ = self.zone_rows(title, note, key, list(headers) + (["Looks like"] if preview else []), zones, cells)
        return box

    def build_singles(self, key, title, note):
        def cells(zone):
            swatch = Swatch(ZONE_NAMES[zone], lambda c, z=zone: FtpColors.set_value(key, c, z))
            self.refreshers.append(lambda: swatch.set_color(FtpColors.get(key)[zone]))
            return [swatch]
        box, _, _ = self.zone_rows(title, note, key, ["Color"], list(FtpColors.DEFAULTS[key]), cells)
        return box

    def build_blocks(self):
        def cells(zone):
            swatches = [Swatch(f"{ZONE_NAMES[zone]}: {header}", lambda c, z=zone, i=i: self.set_part("blocks", z, i, c))
                        for i, header in enumerate(("start", "end"))]
            bar = GradientBar()
            def refresh():
                colors = FtpColors.get("blocks")[zone]
                for swatch, color in zip(swatches, colors):
                    swatch.set_color(color)
                bar.set_colors(colors)
            self.refreshers.append(refresh)
            return swatches + [bar]
        box, grid, row = self.zone_rows(
            "Workout blocks", "A power range blends from its low power's start to its high power's end; a "
            "single power from its start to the single block end below.", "blocks",
            ["Start", "End", "Range looks like"], list(FtpColors.DEFAULTS["blocks"]), cells)
        grid.addWidget(QLabel("Single block end"), row, 0)
        swatch = Swatch("Single block end", lambda c: FtpColors.set_value("blockSingleEnd", c))
        self.refreshers.append(lambda: swatch.set_color(FtpColors.get("blockSingleEnd")))
        grid.addWidget(swatch, row, 1)
        grid.addWidget(self.reset_button("blockSingleEnd"), row, 4)
        return box

    def build_graph(self):
        def cells(zone):
            swatches = [Swatch(f"{ZONE_NAMES[zone]}: {state}", lambda c, z=zone, i=i: self.set_part("graph", z, i, c))
                        for i, state in enumerate(FtpColors.GRAPH_STATES)]
            def refresh():
                for swatch, color in zip(swatches, FtpColors.get("graph")[zone]):
                    swatch.set_color(color)
            self.refreshers.append(refresh)
            return swatches
        box, _, _ = self.zone_rows("Workout graph bars", "Shown see-through. Paused is also before the start; "
                                   "the synopsis graph uses past.", "graph",
                                   ["Paused", "Upcoming", "Past"], list(FtpColors.DEFAULTS["graph"]), cells)
        return box

    def set_part(self, key, zone, index, color):
        colors = FtpColors.get(key)[zone]
        colors[index] = color
        FtpColors.set_value(key, colors, zone)


###----------------------------------------------------------------------------------------------
### The mockup window
###----------------------------------------------------------------------------------------------
class GraphMock(QWidget):
    """The workout graph's bars, every zone in each state"""
    def __init__(self):
        super().__init__()
        self.setMinimumSize(420, 260)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(GRAPH_BACKGROUND))
        painter.drawRoundedRect(rect, 15, 15)
        labelWidth = 110
        rowHeight = (rect.height() - 20)/len(FtpColors.GRAPH_STATES)
        barWidth = (rect.width() - labelWidth - 30)/5
        painter.setFont(QFont(self.font().family(), 10))
        for row, state in enumerate(FtpColors.GRAPH_STATES):
            top = rect.top() + 10 + row*rowHeight
            painter.setPen(QColor("#000000"))
            painter.drawText(QRectF(rect.left() + 12, top, labelWidth, rowHeight), Qt.AlignmentFlag.AlignVCenter,
                             state.title())
            painter.setPen(Qt.PenStyle.NoPen)
            for zone, brush in enumerate(FtpColors.graph_brushes(state)):
                height = (rowHeight - 12)*(0.3 + 0.175*zone)
                left = rect.left() + labelWidth + 10 + zone*barWidth
                painter.setBrush(brush)
                painter.drawRect(QRectF(left, top + rowHeight - 6 - height, barWidth - 4, height))


class FtpMockupWindow(QWidget):
    """The four parts colored by power zone, for any FTP and power"""
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Layout Viewer FTP Colors")
        self._layoutExempt = True #the layout scale doesn't apply here
        if AppState.defaultFont is not None:
            self.setFont(AppState.defaultFont)
        outer = QVBoxLayout(self)

        ##FTP and power
        controls = QGridLayout()
        controls.addWidget(QLabel("FTP"), 0, 0)
        self.ftpSpin = QSpinBox()
        self.ftpSpin.setRange(40, 500)
        self.ftpSpin.setSuffix(" W")
        self.ftpSpin.setMinimumWidth(110)
        controls.addWidget(self.ftpSpin, 0, 1)
        self.ftpSlider = QSlider(Qt.Orientation.Horizontal)
        self.ftpSlider.setRange(40, 500)
        controls.addWidget(self.ftpSlider, 0, 2)
        controls.addWidget(QLabel("Power"), 1, 0)
        self.powerSpin = QSpinBox()
        self.powerSpin.setRange(0, 1000)
        self.powerSpin.setSuffix(" W")
        self.powerSpin.setMinimumWidth(110)
        controls.addWidget(self.powerSpin, 1, 1)
        self.powerSlider = QSlider(Qt.Orientation.Horizontal)
        self.powerSlider.setRange(0, 1000)
        controls.addWidget(self.powerSlider, 1, 2)
        self.powerNote = QLabel()
        controls.addWidget(self.powerNote, 1, 3)
        self.stopButtons = QHBoxLayout()
        controls.addWidget(QLabel("Point at"), 2, 0)
        controls.addLayout(self.stopButtons, 2, 1, 1, 3)
        controls.setColumnStretch(2, 1)
        outer.addLayout(controls)
        for spin, slider in ((self.ftpSpin, self.ftpSlider), (self.powerSpin, self.powerSlider)):
            spin.valueChanged.connect(slider.setValue)
            slider.valueChanged.connect(spin.setValue)
            spin.valueChanged.connect(self.refresh)

        ##the parts
        parts = QGridLayout()
        self.gauge = Gauge()
        widget = self.gauge.ui.widget
        widget.setNeedleColor(*QColor(AppState.gaugeRed).getRgb())
        widget.setGaugeTheme(3)
        widget.setMouseTracking(False)
        widget.setEnableBarGraph(False)
        widget.units = "Watts"
        self.gauge.setMinimumSize(420, 420)
        parts.addWidget(self.titled("Power gauge (target and power ridden both at the power above)", self.gauge), 0, 0, 2, 1)

        spirals = QWidget()
        spiralRow = QHBoxLayout(spirals)
        spiralRow.setContentsMargins(0, 0, 0, 0)
        self.spirals = []
        for title in ("Line", "Blinking"):
            spiral = SpiralProgressBar()
            spiral.ui.widget.spb_setNoProgressBar(2)
            spiral.ui.widget.spb_setGap(20)
            spiral.ui.widget.pathWidth = 1
            spiral.ui.widget.spb_setValue((65, 40))
            spiral.setMinimumSize(190, 190)
            self.spirals.append(spiral)
            spiralRow.addWidget(self.titled(title, spiral))
        parts.addWidget(self.titled("Spiral (workout progress, then the current block)", spirals), 0, 1)

        blocks = QWidget()
        blockColumn = QVBoxLayout(blocks)
        blockColumn.setContentsMargins(0, 0, 0, 0)
        blockColumn.setSpacing(4)
        self.blockLabels = []
        for _ in range(8):
            label = QLabel()
            label.setFixedHeight(36)
            label.setFont(QFont(AppTheme.THEME.fonts[AppTheme.ROLE_MAIN]["family"], 13))
            blockColumn.addWidget(label)
            self.blockLabels.append(label)
        parts.addWidget(self.titled("Workout blocks", blocks), 1, 1)

        self.graph = GraphMock()
        parts.addWidget(self.titled("Workout graph bars", self.graph), 2, 0, 1, 2)
        outer.addLayout(parts, 1)

        FtpColors.watch(self, "refresh")
        ftp = int(AppState.ftp) if str(AppState.ftp).isdigit() and 40 <= int(AppState.ftp) <= 500 else DEFAULT_FTP
        self.ftpSpin.setValue(ftp)
        self.powerSpin.setValue(round(ftp*0.92))
        self.refresh()

    def titled(self, title, widget):
        box = QGroupBox(title)
        layout = QVBoxLayout(box)
        layout.addWidget(widget)
        return box

    def refresh(self):
        ftp, power = self.ftpSpin.value(), self.powerSpin.value()
        ranges = ranges_for(ftp)
        zone = FtpColors.zone_of(power, ranges)
        self.powerNote.setText(f"{power/ftp*100:.0f}% of FTP, {ZONE_NAMES[zone].split('  ')[0]}")

        #"point at" buttons, one per band color
        while self.stopButtons.count():
            item = self.stopButtons.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        for pct, color in sorted(FtpColors.get("gaugeScale")):
            watts = round(ftp*pct/100)
            button = QPushButton(f"{pct}%")
            button.setToolTip(f"{watts} W: the needle on this band color" + ("" if watts <= 1000 else " (past the dial)"))
            set_sheet(button, f"QPushButton {{ border:2px solid {color}; border-radius:4px; padding:2px 8px; }}")
            button.setEnabled(watts <= 1000)
            button.clicked.connect(lambda _, w=watts: self.powerSpin.setValue(w))
            self.stopButtons.addWidget(button)
        self.stopButtons.addStretch(1)

        ##gauge
        widget = self.gauge.ui.widget
        widget.set_value_colors(FtpColors.gauge_scale_stops(ftp))
        widget.setValue(power)
        c0, c1 = FtpColors.gauge_arc(power, ranges)
        widget.arcCenterValue = power
        widget.newArcColors = [[0, QColor("#88" + c0)], [.5, QColor("#88" + c1)]]
        widget.setEnableDrawNewArc()
        widget.outer_circle_bg = [[0.25, QColor(0, 0, 0, 0)], [0.18, QColor("#44" + FtpColors.gauge_ring(power, ranges))]]
        widget.update()

        ##spirals: as the HUD draws them (ClassHUDWindow.updateSpiralColors / updateSpiralColorsBlockBlinkOn)
        line, blink = FtpColors.spiral(power, ranges)
        self.spirals[0].ui.widget.spb_lineColor((SPIRAL_PROGRESS, "#" + line))
        self.spirals[1].ui.widget.spb_lineColor((SPIRAL_PROGRESS, "#22" + blink))
        for spiral in self.spirals:
            spiral.ui.widget.update()

        ##block labels: a single block per zone, a range and free ride
        samples = [(round(ftp*pct/100), 0) for pct in (25, 60, 100, 150, 200)]
        samples += [(round(ftp*0.5), round(ftp*1.1))]
        texts = [f"Zone {i + 1}:  {w} Watts" for i, (w, _) in enumerate(samples[:5])]
        texts += [f"Range:  {samples[5][0]}-{samples[5][1]} Watts"]
        for label, text, (p0, p1) in zip(self.blockLabels, texts, samples):
            label.setText(text)
            set_sheet(label, FtpColors.block_css(p0, p1, ranges) + "border-radius:15px; padding-left:14px;")
            label.show()
        self.blockLabels[6].setText("Free ride")
        set_sheet(self.blockLabels[6], FtpColors.free_ride_css() + "border-radius:15px; padding-left:14px;")
        self.blockLabels[7].hide()

        self.graph.update()
