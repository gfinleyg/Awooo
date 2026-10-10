"""Standalone RGBA color picker (Google-style) for choosing QColor(r, g, b, a) values.

Usage:  python ColorPicker.py [r g b a]
"""
import sys

from PyQt6.QtCore import Qt, pyqtSignal, QPointF, QRectF
from PyQt6.QtGui import QColor, QPainter, QLinearGradient, QPen, QGuiApplication
from PyQt6.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout, QLabel,
    QSpinBox, QPushButton, QLineEdit,
)


class SVSquare(QWidget):
    """Saturation (x) / value (y) square for the current hue; click or drag to pick."""
    changed = pyqtSignal(float, float)  # saturation, value in 0..1

    def __init__(self, parent=None):
        super().__init__(parent)
        self.hue, self.sat, self.val = 0.0, 0.0, 1.0
        self.setMinimumSize(300, 200)
        self.setCursor(Qt.CursorShape.CrossCursor)

    def set_hsv(self, h, s, v):
        self.hue, self.sat, self.val = h, s, v
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect())
        # white -> pure hue, left to right
        g = QLinearGradient(r.topLeft(), r.topRight())
        g.setColorAt(0, QColor(255, 255, 255))
        g.setColorAt(1, QColor.fromHsvF(self.hue, 1, 1))
        p.fillRect(r, g)
        # transparent -> black, top to bottom
        g = QLinearGradient(r.topLeft(), r.bottomLeft())
        g.setColorAt(0, QColor(0, 0, 0, 0))
        g.setColorAt(1, QColor(0, 0, 0, 255))
        p.fillRect(r, g)
        # selection ring
        pt = QPointF(self.sat * (r.width() - 1), (1 - self.val) * (r.height() - 1))
        p.setPen(QPen(QColor(255, 255, 255), 2))
        p.setBrush(QColor.fromHsvF(self.hue, self.sat, self.val))
        p.drawEllipse(pt, 8, 8)
        p.end()

    def _pick(self, pos):
        s = min(max(pos.x() / max(1, self.width() - 1), 0.0), 1.0)
        v = 1 - min(max(pos.y() / max(1, self.height() - 1), 0.0), 1.0)
        self.sat, self.val = s, v
        self.update()
        self.changed.emit(s, v)

    def mousePressEvent(self, e):
        self._pick(e.position())

    def mouseMoveEvent(self, e):
        if e.buttons() & Qt.MouseButton.LeftButton:
            self._pick(e.position())


class Bar(QWidget):
    """Horizontal slider bar drawn as a gradient (hue rainbow or alpha ramp)."""
    changed = pyqtSignal(float)  # 0..1

    def __init__(self, kind, parent=None):
        super().__init__(parent)
        self.kind = kind  # "hue" or "alpha"
        self.pos = 0.0
        self.color = QColor(0, 0, 0)
        self.setFixedHeight(16)
        self.setMinimumWidth(200)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def set_value(self, pos, color=None):
        self.pos = pos
        if color is not None:
            self.color = QColor(color)
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(8, 2, -8, -2)
        g = QLinearGradient(r.topLeft(), r.topRight())
        if self.kind == "hue":
            for i in range(7):
                g.setColorAt(i / 6, QColor.fromHsvF((i / 6) % 1.0, 1, 1))
        else:
            # checkerboard behind the alpha ramp so transparency is visible
            p.save()
            p.setClipRect(r)
            cell = 6
            for yi, y in enumerate(range(int(r.top()), int(r.bottom()) + 1, cell)):
                for xi, x in enumerate(range(int(r.left()), int(r.right()) + 1, cell)):
                    p.fillRect(x, y, cell, cell,
                               QColor(200, 200, 200) if (xi + yi) % 2 else QColor(255, 255, 255))
            p.restore()
            c0, c1 = QColor(self.color), QColor(self.color)
            c0.setAlpha(0)
            c1.setAlpha(255)
            g.setColorAt(0, c0)
            g.setColorAt(1, c1)
        p.fillRect(r, g)
        # knob
        x = r.left() + self.pos * r.width()
        p.setPen(QPen(QColor(90, 90, 90), 1))
        p.setBrush(QColor(255, 255, 255))
        p.drawEllipse(QPointF(x, self.height() / 2), 7, 7)
        p.end()

    def _pick(self, pos):
        w = self.width() - 16
        v = min(max((pos.x() - 8) / max(1, w), 0.0), 1.0)
        self.pos = v
        self.update()
        self.changed.emit(v)

    def mousePressEvent(self, e):
        self._pick(e.position())

    def mouseMoveEvent(self, e):
        if e.buttons() & Qt.MouseButton.LeftButton:
            self._pick(e.position())


class Preview(QWidget):
    """Plain swatch of the chosen color."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.color = QColor(0, 0, 0)
        self.setFixedSize(56, 56)

    def set_color(self, c):
        self.color = QColor(c)
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(QPen(QColor(120, 120, 120), 1))
        p.setBrush(self.color)
        p.drawRoundedRect(QRectF(self.rect()).adjusted(1, 1, -1, -1), 8, 8)
        p.end()


class ColorPicker(QWidget):
    def __init__(self, r=200, g=200, b=200, a=130):
        super().__init__()
        self.setWindowTitle("QColor Picker")
        self._updating = False
        self.h, self.s, self.v, self.a = 0.0, 0.0, 1.0, 1.0

        root = QVBoxLayout(self)

        self.square = SVSquare()
        self.square.changed.connect(self.on_sv)
        root.addWidget(self.square, 1)

        row = QHBoxLayout()
        self.preview = Preview()
        row.addWidget(self.preview)
        bars = QVBoxLayout()
        self.hue_bar = Bar("hue")
        self.hue_bar.changed.connect(self.on_hue)
        self.alpha_bar = Bar("alpha")
        self.alpha_bar.changed.connect(self.on_alpha)
        bars.addWidget(self.hue_bar)
        bars.addWidget(self.alpha_bar)
        row.addLayout(bars, 1)
        root.addLayout(row)

        fields = QGridLayout()
        self.hex_edit = QLineEdit()
        self.hex_edit.setMaxLength(9)
        self.hex_edit.setMinimumWidth(90)
        self.hex_edit.editingFinished.connect(self.on_hex)
        fields.addWidget(QLabel("HEX"), 0, 0, alignment=Qt.AlignmentFlag.AlignCenter)
        fields.addWidget(self.hex_edit, 1, 0)
        self.spins = []
        for i, name in enumerate(("R", "G", "B", "A")):
            sp = QSpinBox()
            sp.setRange(0, 255)
            sp.setMinimumWidth(70)
            sp.setAlignment(Qt.AlignmentFlag.AlignCenter)
            sp.valueChanged.connect(self.on_spin)
            fields.addWidget(QLabel(name), 0, i + 1, alignment=Qt.AlignmentFlag.AlignCenter)
            fields.addWidget(sp, 1, i + 1)
            self.spins.append(sp)
        root.addLayout(fields)

        out = QHBoxLayout()
        self.text = QLineEdit()
        self.text.setReadOnly(True)
        font = self.text.font()
        font.setFamily("Orbitron")
        font.setPointSize(11)
        self.text.setFont(font)
        copy_btn = QPushButton("Copy")
        copy_btn.clicked.connect(lambda: QGuiApplication.clipboard().setText(self.text.text()))
        out.addWidget(self.text, 1)
        out.addWidget(copy_btn)
        root.addLayout(out)

        self.resize(360, 460)
        self.set_color(QColor(r, g, b, a))

    # ------------------------------------------------------------------
    def color(self):
        c = QColor.fromHsvF(self.h, self.s, self.v)
        c.setAlpha(round(self.a * 255))
        return c

    def set_color(self, c):
        h, s, v, a = c.getHsvF()
        if h >= 0:  # keep previous hue for grays
            self.h = h
        self.s, self.v, self.a = s, v, a
        self.refresh(exact=c)

    def refresh(self, exact=None):
        c = exact if exact is not None else self.color()
        self._updating = True
        self.square.set_hsv(self.h, self.s, self.v)
        self.hue_bar.set_value(self.h)
        self.alpha_bar.set_value(self.a, c)
        self.preview.set_color(c)
        for sp, val in zip(self.spins, c.getRgb()):
            sp.setValue(val)
        self.hex_edit.setText(c.name(QColor.NameFormat.HexArgb) if c.alpha() < 255
                              else c.name())
        self.text.setText("QColor({},{},{},{})".format(*c.getRgb()))
        self._updating = False

    def on_sv(self, s, v):
        self.s, self.v = s, v
        self.refresh()

    def on_hue(self, h):
        self.h = min(h, 0.9999)
        self.refresh()

    def on_alpha(self, a):
        self.a = a
        self.refresh()

    def on_spin(self, *_):
        if not self._updating:
            self.set_color(QColor(*[sp.value() for sp in self.spins]))

    def on_hex(self):
        c = QColor(self.hex_edit.text().strip())
        if c.isValid():
            self.set_color(c)
        else:
            self.refresh()


def main():
    app = QApplication(sys.argv)
    # Fusion keeps spin box digits visible; the Windows 11 style's wide arrow buttons crowd them out
    app.setStyle("Fusion")
    args = [int(x) for x in sys.argv[1:5]] if len(sys.argv) >= 5 else [200, 200, 200, 130]
    w = ColorPicker(*args)
    w.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
