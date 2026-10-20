"""
FtpColorsEditor.py

LayoutViewer.py's FTP Colors tab and its mockup window (not part of AntWorkout): the power zones' colors
(FtpColors.py), edited on the tab and shown on the mockup (the power gauge, the workout block labels and the
workout graph's bars) for any FTP (40 - 500 W) and power.
"""

from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import QColor, QFont, QPainter, QPen
from PyQt6.QtWidgets import (QColorDialog, QGridLayout, QGroupBox, QHBoxLayout, QLabel, QPushButton,
                             QScrollArea, QSizePolicy, QSlider, QSpinBox, QVBoxLayout, QWidget)

import AppTheme
import FtpColors
from AppState import AppState
from ClassAnalogGauge import Gauge

#the zones' limits as % of FTP (ClassWorkoutSetupWindow.py sets AppState.ftpRanges the same way)
ZONE_LIMITS = (36, 87, 119, 187)
ZONE_NAMES = {"0": f"Zone 1  (under {ZONE_LIMITS[0]}%)", "1": f"Zone 2  ({ZONE_LIMITS[0]}–{ZONE_LIMITS[1]}%)",
              "2": f"Zone 3  ({ZONE_LIMITS[1]}–{ZONE_LIMITS[2]}%)", "3": f"Zone 4  ({ZONE_LIMITS[2]}–{ZONE_LIMITS[3]}%)",
              "4": f"Zone 5  ({ZONE_LIMITS[3]}% and up)", "free": "Free ride  (0 W)"}
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
    """Each power zone's color, and free ride's"""
    def __init__(self):
        super().__init__()
        self.setWidgetResizable(True)
        self.refreshers = [] #redraw the rows from FtpColors
        body = QWidget()
        layout = QVBoxLayout(body)
        note = QLabel("One color per power zone (by % of FTP; RPM in Musical Cadence Mode). Each zone fades from "
                      "pale where it starts to its color where it ends, on the gauges, spiral, block labels and "
                      "workout graph. A color too close to the theme's background is moved until it stands out "
                      "(adjusted). The FTP Colors window shows them on the parts for any FTP and power. "
                      "Saved with the theme.")
        note.setWordWrap(True)
        layout.addWidget(note)
        layout.addWidget(self.build_zones())
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

    def build_zones(self):
        box = QGroupBox("Zone colors")
        grid = QGridLayout(box)
        for col, header in enumerate(("Color", "Looks like (on this theme)", "")):
            grid.addWidget(QLabel(f"<small>{header}</small>"), 0, col + 1)
        for row, zone in enumerate(FtpColors.ZONES, start=1):
            swatch = Swatch(ZONE_NAMES[zone], lambda c, z=zone: FtpColors.set_value("zones", c, z))
            bar = GradientBar()
            adjusted = QLabel()
            def refresh(zone=zone, swatch=swatch, bar=bar, adjusted=adjusted):
                swatch.set_color(FtpColors.get("zones")[zone])
                pale, color, moved = FtpColors.palette()[int(zone)]
                bar.set_colors((pale, color))
                adjusted.setText("<small>adjusted</small>" if moved else "")
            self.refreshers.append(refresh)
            grid.addWidget(QLabel(ZONE_NAMES[zone]), row, 0)
            grid.addWidget(swatch, row, 1)
            grid.addWidget(bar, row, 2)
            grid.addWidget(adjusted, row, 3)
            grid.addWidget(self.reset_button("zones", zone), row, 4)
        row = len(FtpColors.ZONES) + 1
        swatch = Swatch(ZONE_NAMES["free"], lambda c: FtpColors.set_value("free", c))
        bar = GradientBar()
        def refresh():
            swatch.set_color(FtpColors.get("free"))
            bar.set_colors(FtpColors.free_ride_colors())
        self.refreshers.append(refresh)
        grid.addWidget(QLabel(ZONE_NAMES["free"]), row, 0)
        grid.addWidget(swatch, row, 1)
        grid.addWidget(bar, row, 2)
        grid.addWidget(self.reset_button("free"), row, 4)
        grid.setColumnStretch(2, 1)
        return box


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
        ranges = ranges_for(DEFAULT_FTP)
        #four bars a zone, from a quarter of the way through it to its end
        samples = [(zone, start + (end - start)*k/4) for zone in range(5)
                   for start, end in [FtpColors.zone_span(zone, ranges)] for k in range(1, 5)]
        barWidth = (rect.width() - labelWidth - 30)/len(samples)
        painter.setFont(QFont(self.font().family(), 10))
        for row, state in enumerate(FtpColors.GRAPH_STATES):
            top = rect.top() + 10 + row*rowHeight
            painter.setPen(QColor("#000000"))
            painter.drawText(QRectF(rect.left() + 12, top, labelWidth, rowHeight), Qt.AlignmentFlag.AlignVCenter,
                             state.title())
            painter.setPen(Qt.PenStyle.NoPen)
            for i, (zone, watts) in enumerate(samples):
                height = (rowHeight - 12)*(0.3 + 0.175*zone)
                left = rect.left() + labelWidth + 10 + i*barWidth
                painter.setBrush(FtpColors.graph_color(watts, ranges, state))
                painter.drawRect(QRectF(left, top + rowHeight - 6 - height, barWidth - 1, height))


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
        parts.addWidget(self.titled("Workout blocks", blocks), 0, 1, 2, 1)

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

        #"point at" buttons, one where each zone ends (at its color)
        while self.stopButtons.count():
            item = self.stopButtons.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        for zone in range(5):
            end = FtpColors.zone_span(zone, ranges)[1]
            watts = round(end) - (1 if zone < 4 else 0)
            color = FtpColors.palette()[zone][1]
            button = QPushButton(f"{end/ftp*100:.0f}%")
            button.setToolTip(f"{watts} W: the needle where {ZONE_NAMES[str(zone)].split('  ')[0]} is its color" +
                              ("" if watts <= 1000 else " (past the dial)"))
            set_sheet(button, f"QPushButton {{ border:2px solid {color}; border-radius:4px; padding:2px 8px; }}")
            button.setEnabled(watts <= 1000)
            button.clicked.connect(lambda _, w=watts: self.powerSpin.setValue(w))
            self.stopButtons.addWidget(button)
        self.stopButtons.addStretch(1)

        ##gauge
        widget = self.gauge.ui.widget
        widget.set_value_colors(FtpColors.zone_stops(ranges))
        widget.setValue(power)
        c0, c1 = FtpColors.gauge_arc(power, ranges)
        widget.arcCenterValue = power
        widget.newArcColors = [[0, QColor("#88" + c0)], [.5, QColor("#88" + c1)]]
        widget.setEnableDrawNewArc()
        widget.outer_circle_bg = [[0.25, QColor(0, 0, 0, 0)], [0.18, QColor("#44" + FtpColors.gauge_ring(power, ranges))]]
        widget.update()

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
