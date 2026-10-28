"""
ClassZoneLegend.py

The HUD's zone legend, under the coming up blocks and as wide as them: the zones' colors in a bar, each zone the same
width and fading the way the power gauge's band does (FtpColors.zone_stops), with the watts (Musical Cadence Mode: the
RPM) where each zone ends under the edge between it and the next.

The numbers are in the theme's text color. Where that doesn't stand out from the window's gradient behind them
(contrast under MIN_CONTRAST: a mid-tone gradient reads poorly with dark and light text alike), they get a halo, light
under dark text and dark under light (the graph's sensor lines' halo colors, FtpColors.halo_color).
"""

from PyQt6.QtCore import QPointF, QRectF, Qt
from PyQt6.QtGui import QColor, QFont, QFontMetricsF, QLinearGradient, QPainter, QPainterPath, QPen
from PyQt6.QtWidgets import QWidget

import AppTheme
import FtpColors
from AppState import AppState
from ClassGradeWidgets import fitted_font
from ClassLongVerticalLabelSeries import BLOCK_LABEL_WIDTH

#design px: the bar, then the numbers under it
LEGEND_HEIGHT = 46
BAR_HEIGHT = 20
TEXT_GAP = 3
LABEL_POINTS = 11
#the bar's fade steps in each zone (as the gauges' bands)
STOP_STEPS = 6
TEXT_COLOR = "#000000"
#WCAG AA for text: numbers that contrast less than this with the window's gradient behind them get a halo, HALO_PX
#(design px) out from their outline
MIN_CONTRAST = 4.5
HALO_PX = 2.5
HALO_ALPHA = 0xd0


class ZoneLegend(QWidget):
    def __init__(self):
        super().__init__()
        self.setFixedSize(BLOCK_LABEL_WIDTH, LEGEND_HEIGHT)
        self._themeColors = (TEXT_COLOR,) #(LayoutViewer lists it with the colors a theme can change)
        #redrawn when the power zone colors change
        FtpColors.watch(self, "update")

    @staticmethod
    def ranges():
        return AppState.ftpRanges or FtpColors.power_ranges()

    def label_texts(self):
        """Where each zone but the top one ends, as the block labels show a target"""
        unit = "W" if AppState.target_unit() == "Watts" else AppState.target_unit().lower()
        return [f"{round(limit)}{unit}" for limit in self.ranges()]

    def bar_gradient(self, rect):
        """The zones side by side, each fading from its pale end to its color, a hard edge where the next starts"""
        stops = FtpColors.zone_stops(self.ranges(), STOP_STEPS)
        zones = len(FtpColors.ZONES)
        gradient = QLinearGradient(rect.left(), 0, rect.right(), 0)
        for zone in range(zones):
            for i in range(STOP_STEPS + 1):
                at = (zone + i/STOP_STEPS)/zones
                if i == STOP_STEPS and zone < zones - 1:
                    at -= 1e-4
                gradient.setColorAt(at, stops[zone*(STOP_STEPS + 1) + i][1])
        return gradient

    def background_at(self, point):
        """The window's background (its gradient corner to corner, through the theme) behind a point of this"""
        window = self.window()
        at = self.mapTo(window, point)
        t = (at.x()/max(1, window.width()) + at.y()/max(1, window.height()))/2
        start, end = (AppTheme.THEME.map_hex(color) for color in FtpColors.WINDOW_GRADIENT)
        return FtpColors.mix(start, end, min(1.0, max(0.0, t)))

    def label_row(self):
        """(the centers of the numbers, the top and height of the row they're in)"""
        scale = AppTheme.widget_scale(self)
        top = BAR_HEIGHT*scale + TEXT_GAP*scale
        rowHeight = self.height() - top
        zones = len(FtpColors.ZONES)
        return [QPointF(self.width()*k/zones, top + rowHeight/2) for k in range(1, zones)], top, rowHeight

    def haloed(self):
        """Whether the numbers are drawn over a halo (with the theme as it is and where this is in its window)"""
        color = QColor(AppTheme.THEME.map_hex(TEXT_COLOR))
        centers = self.label_row()[0]
        return min(AppTheme.contrast(color, self.background_at(center)) for center in centers) < MIN_CONTRAST

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        scale = AppTheme.widget_scale(self)
        bar = QRectF(0, 0, self.width(), BAR_HEIGHT*scale)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self.bar_gradient(bar))
        painter.drawRoundedRect(bar, bar.height()/2, bar.height()/2)

        texts = self.label_texts()
        centers, textTop, textHeight = self.label_row()
        font = QFont("Orbitron")
        font.setPointSizeF(LABEL_POINTS)
        font.setWeight(QFont.Weight.Black)
        font = AppTheme.THEME.map_font(font, scale)
        #(all the same size: as big as the widest fits in a zone's width)
        widest = max(texts, key=QFontMetricsF(font).horizontalAdvance)
        font = fitted_font(font, widest, self.width()/len(FtpColors.ZONES)*0.92, textHeight)
        metrics = QFontMetricsF(font)
        baseline = textTop + (textHeight - metrics.height())/2 + metrics.ascent()
        path = QPainterPath()
        for text, center in zip(texts, centers):
            path.addText(QPointF(center.x() - metrics.horizontalAdvance(text)/2, baseline), font, text)

        color = QColor(AppTheme.THEME.map_hex(TEXT_COLOR))
        if self.haloed():
            painter.setPen(QPen(FtpColors.halo_color(color.name(), HALO_ALPHA), 2*HALO_PX*scale, Qt.PenStyle.SolidLine,
                                Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawPath(path)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(color)
        painter.drawPath(path)
