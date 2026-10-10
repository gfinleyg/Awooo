from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import QColor, QPainter
from PyQt6.QtWidgets import QProgressBar

#QProgressBar that paints its own fill so it can sit between pixels, letting the bar glide instead of stepping
#a pixel at a time; setFraction() drives the smooth position, setValue()/setRange() still work as usual
class SmoothProgressBar(QProgressBar):
    def __init__(self, chunkColor="#1f15e1", barColor="#bdd6de"):
        super(SmoothProgressBar, self).__init__()
        self.chunkColor = QColor(chunkColor)
        self.barColor = QColor(barColor)
        self.fraction = None #smooth 0-1 position, None falls back to value()
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent) #the whole rect is painted every time

    def setBarColors(self, chunkColor, barColor):
        self.chunkColor = QColor(chunkColor)
        self.barColor = QColor(barColor)
        self.update()

    def setFraction(self, fraction):
        fraction = min(max(fraction, 0.0), 1.0)
        if fraction != self.fraction:
            self.fraction = fraction
            self.update()

    #a hard set (block change, reset, workout end) overrides any smooth position until the next setFraction()
    def setValue(self, value):
        self.fraction = None
        super(SmoothProgressBar, self).setValue(value)
        self.update()

    def setRange(self, minimum, maximum):
        self.fraction = None
        super(SmoothProgressBar, self).setRange(minimum, maximum)
        self.update()

    def paintEvent(self, event):
        fraction = self.fraction
        if fraction is None:
            span = self.maximum() - self.minimum()
            fraction = (self.value() - self.minimum())/span if span > 0 else 0.0
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect())
        painter.fillRect(rect, self.barColor)
        #drawRect (not fillRect) so the antialiasing blends the fractional pixel at the leading edge
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self.chunkColor)
        painter.drawRect(QRectF(rect.x(), rect.y(), rect.width()*fraction, rect.height()))
        painter.end()
