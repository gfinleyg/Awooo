"""MaxStar: a tiny star over the highest point of a graph curve. When the max moves (zooming or panning the synopsis,
a new high reading or the old max scrolling off the workout screen) it rides the curve as drawn to the new max: still
at both ends and fastest halfway (smoothstep, whose speed is a parabola), hugging the line on the way, then settling
back just over the max, as close as it can without touching the line (or a marker there), where it twinkles. Its
owner says where the max is (go_to), steps it about every frame (step) and keeps room for it at the graph's top
(top_with_room). MaxBubble: the max's value in a small box beside a star at rest, popping when the max moves."""

import math
import time

import numpy as np
import pyqtgraph as pg

from PyQt6.QtCore import QPointF, QRectF, Qt
from PyQt6.QtGui import QColor, QFont, QFontMetricsF, QPainter, QPainterPath, QPen
from PyQt6.QtWidgets import QGraphicsItem, QGraphicsPathItem

import AppTheme


#how often the owner steps its stars, in ms (about every frame)
STEP_MS = 16


class MaxStar(QGraphicsPathItem):
    #(at the app's design size: scaled with the rest of the screen, see AppTheme.ui_scale)
    RADIUS = 8
    #the inner corners' share of RADIUS: plumper in the middle than a classic star's ~0.38
    INNER = 0.58
    #its lowest points' depth below its center, as a share of RADIUS (the two lower tips, 54 degrees down)
    BELOW = math.sin(math.radians(54))
    #how much bigger it gets at the top of a twinkle
    TWINKLE = 0.14
    EDGE_WIDTH = 1.5
    #the gap left between it and the line (or marker) under it, in pixels
    GAP = 1.0
    COLOR = "#ffd23f"
    EDGE_COLOR = "#3a2a00"
    #a trip takes TRIP_SECONDS plus TRIP_SECONDS_PER_PX for each pixel of curve, within TRIP_MIN - TRIP_MAX seconds
    TRIP_SECONDS = 0.45
    TRIP_SECONDS_PER_PX = 0.0009
    TRIP_MIN, TRIP_MAX = 0.5, 1.4

    def __init__(self, curve, vbox, viewRange):
        """curve: the PlotCurveItem it rides; vbox: the viewbox that's drawn in; viewRange(): the graph's shown
        (start, end) times. Add it to the graph's scene"""
        path = QPainterPath()
        for i in range(10):
            angle = -math.pi / 2 + i * math.pi / 5
            radius = self.RADIUS * (self.INNER if i % 2 else 1)
            point = QPointF(radius * math.cos(angle), radius * math.sin(angle))
            if i:
                path.lineTo(point)
            else:
                path.moveTo(point)
        path.closeSubpath()
        super().__init__(path)
        pen = QPen(QColor(self.EDGE_COLOR), self.EDGE_WIDTH)
        pen.setCosmetic(True) #(stays this wide while it twinkles)
        AppTheme.theme_pen(self, pen)
        self.color = self.COLOR
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        self.setZValue(1001) #(over the synopsis' max labels too)
        self.setVisible(False)
        self.curve, self.vbox, self.viewRange = curve, vbox, viewRange
        #how far a marker drawn on the max reaches above it, in pixels (0: none); the owner keeps it up to date
        self.markerReach = 0.0
        #the max it's over or riding to (graph coordinates), None until it has one; hidden while not shown
        self.x = self.y = None
        self.shown = True
        #its trip: from tripFrom to x (times), from tripStart (time.monotonic()) for tripSeconds, lifting off from
        #tripLift (see lift); at: the time it's at now
        self.tripFrom = self.at = None
        self.tripStart, self.tripSeconds, self.tripLift = 0.0, 1.0, 1.0
        #how far above the max it rests now (pixels): eases to rest_offset() when that changes, rather than hopping
        self.offset = None

    def set_color(self, color):
        if color != self.color:
            self.color = color
            self.update()

    def paint(self, painter, option, widget=None):
        #filled in the theme's color for its color, whatever theme is loaded now
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(self.pen())
        painter.setBrush(QColor(AppTheme.THEME.map_hex(self.color)))
        painter.drawPath(self.path())

    def line_reach(self):
        #how far the curve's line (its halo included) reaches above the points it's drawn through, in pixels
        widths = [pg.mkPen(self.curve.opts[name]).widthF() for name in ("pen", "shadowPen")
                  if self.curve.opts.get(name) is not None]
        return max(widths + [1.0]) / 2

    def rest_offset(self):
        """How far above its max its center rests, in pixels: its lowest tips (at a twinkle's biggest) just clear of
        the line and any marker there"""
        below = self.RADIUS * self.BELOW * AppTheme.ui_scale * (1 + self.TWINKLE) + self.EDGE_WIDTH / 2
        return below + max(self.line_reach(), self.markerReach) + self.GAP

    def headroom(self):
        """How far above its max it reaches, in pixels: the room it needs at the top of the graph"""
        return self.rest_offset() + self.RADIUS * AppTheme.ui_scale * (1 + self.TWINKLE) + self.EDGE_WIDTH / 2 + self.GAP

    def rest_rect(self, pt):
        """Where it rests over a max at scene point pt, with a little room around it"""
        radius = self.RADIUS * AppTheme.ui_scale * 1.3
        center = QPointF(pt.x(), pt.y() - self.rest_offset())
        return QRectF(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius)

    @staticmethod
    def top_with_room(vbox, top, starValues, reserved=0.0):
        """A y range top (from 0) of at least top, high enough for each (star or its MaxBubble, value): that resting
        over a max of value fits under it, and under reserved pixels along the graph's top"""
        #(the height the view's range is drawn over, in pixels)
        height = vbox.mapViewToScene(QPointF(0, 0)).y() - vbox.mapViewToScene(QPointF(0, vbox.viewRange()[1][1])).y()
        for star, value in starValues:
            room = star.headroom() + reserved
            if value > 0 and height > room:
                top = max(top, value * height / (height - room))
        return top

    @staticmethod
    def ease(t):
        #smoothstep: its speed, 6t(1-t), is a parabola: still at both maxes, fastest halfway
        return t * t * (3 - 2 * t)

    @staticmethod
    def lift(fromLift, t):
        #how far up toward its resting spot (1) from the line (0) it is: it hugs the line while travelling
        return fromLift * (1 - t) ** 4 + t ** 4

    def go_to(self, x, y):
        """The max is at (x, y) now: off there along the curve if it moved (straight there the first time)"""
        if self.x is None:
            self.x, self.y = x, y
            self.tripFrom = self.at = x
            return
        moved = x != self.x
        self.x, self.y = x, y
        if moved:
            #from wherever it is (mid-trip too), lifting off from as high as it was; longer trips on screen take longer
            now = time.monotonic()
            t = self.progress(now)
            self.tripLift = self.lift(self.tripLift, t) if t < 1 else 1.0
            self.tripFrom = self.at
            xs, ys = self.trip_path()
            length = float(np.hypot(np.diff(xs), np.diff(ys)).sum())
            self.tripSeconds = min(self.TRIP_MAX, max(self.TRIP_MIN, self.TRIP_SECONDS + length * self.TRIP_SECONDS_PER_PX))
            self.tripStart = now

    def progress(self, now):
        return min(1.0, max(0.0, (now - self.tripStart) / self.tripSeconds))

    def view_to_scene(self):
        #the viewbox's view -> scene mapping as scene = origin + view * scale: (originX, scaleX, originY, scaleY)
        origin, unit = self.vbox.mapViewToScene(QPointF(0, 0)), self.vbox.mapViewToScene(QPointF(1, 1))
        return origin.x(), unit.x() - origin.x(), origin.y(), unit.y() - origin.y()

    def trip_path(self):
        """The curve as drawn from the trip's start to its max, in scene coordinates at about a point per pixel; from
        the graph's edge when the start is out of view"""
        a, b = self.tripFrom, self.x
        originX, scaleX, originY, scaleY = self.view_to_scene()
        xs, ys = self.curve.getData()
        if xs is None or len(xs) == 0:
            return np.array([originX + b * scaleX]), np.array([originY + self.y * scaleY])
        start, end = self.viewRange()
        lo, hi = max(min(a, b), start), min(max(a, b), end)
        if hi <= lo:
            times = np.array([min(max(b, start), end)])
        else:
            inner = xs[np.searchsorted(xs, lo, "right"):np.searchsorted(xs, hi, "left")]
            step = max(1, int(len(inner) / max(abs(scaleX) * (hi - lo), 1)))
            times = np.concatenate(([lo], inner[::step], [hi]))
            if a > b:
                times = times[::-1]
        return originX + times * scaleX, originY + np.interp(times, xs, ys) * scaleY

    def step(self, now, phase=0):
        """Moves it to where it is at now (time.monotonic()); phase: its twinkle's, to keep stars out of step"""
        if self.x is None or not self.shown:
            self.setVisible(False)
            return
        target = self.rest_offset()
        self.offset = target if self.offset is None else self.offset + (target - self.offset) * 0.3
        t = self.progress(now)
        if t < 1:
            #as far along the curve on screen as smoothstep is along the trip
            xs, ys = self.trip_path()
            lengths = np.concatenate(([0.0], np.cumsum(np.hypot(np.diff(xs), np.diff(ys)))))
            along = self.ease(t) * lengths[-1]
            pt = QPointF(float(np.interp(along, lengths, xs)), float(np.interp(along, lengths, ys)))
            lift = self.lift(self.tripLift, t)
            self.setScale(AppTheme.ui_scale)
            originX, scaleX, _, _ = self.view_to_scene()
            self.at = (pt.x() - originX) / scaleX
        else:
            pt = self.vbox.mapViewToScene(QPointF(self.x, self.y))
            lift = 1.0
            self.setScale(AppTheme.ui_scale * (1 + self.TWINKLE * math.sin(now * 3.6 + phase * 2.1)))
            self.at = self.x
        #(no higher than the graph's top, should its owner not have made room for it there)
        area = self.vbox.sceneBoundingRect()
        y = max(pt.y() - self.offset * lift, area.top() + self.RADIUS * AppTheme.ui_scale)
        self.setPos(pt.x(), y)
        self.setVisible(area.contains(pt))


class MaxBubble(QGraphicsItem):
    """A small rounded box beside a MaxStar at rest with its max's value (e.g. "Max Pwr: 466 W"): as tall as the star
    and level with it, so it's as clear of the line as the star is, and a little apart from the star. When the max
    moves it pops, and a new one appears beside the star once that has ridden to the new max. Its owner steps it
    after the stars (step)"""
    #pixels between it and its star
    GAP = 3
    #its text's side padding, and its corners' radius, as shares of its height
    PAD = 0.3
    CORNER = 0.3
    POP_SECONDS = 0.22
    #how much bigger it gets by the end of its pop
    POP_GROWTH = 0.35
    APPEAR_SECONDS = 0.28

    def __init__(self, star, fmt):
        """star: the MaxStar it goes with; fmt: its text, a format for the max's value. Add it to the graph's scene"""
        super().__init__()
        self.star, self.fmt = star, fmt
        self.text = ""
        self.font = QFont()
        self.width = self.height = 0.0
        #"appearing", "shown", "popping" or "hidden", since stateStart (time.monotonic())
        self.state, self.stateStart = "hidden", 0.0
        #the max it shows (graph coordinates), and which side of the star it's on (1: right, -1: left)
        self.x = self.y = None
        self.side = 1
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        self.setZValue(1000) #(just under the stars)
        self.setVisible(False)

    def boundingRect(self):
        return QRectF(-self.width / 2 - 1, -self.height / 2 - 1, self.width + 2, self.height + 2)

    def paint(self, painter, option, widget=None):
        #in the theme's colors for the plot labels: its background, and black for the border and text
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        black = QColor(AppTheme.THEME.map_hex("#000000"))
        border = QColor(black)
        border.setAlpha(215)
        painter.setPen(QPen(border, 1))
        painter.setBrush(QColor(AppTheme.THEME.map_hex(AppTheme.PLOT_LABEL_BG)))
        rect = QRectF(-self.width / 2, -self.height / 2, self.width, self.height)
        corner = self.height * self.CORNER
        painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), corner, corner)
        painter.setPen(black)
        painter.setFont(self.font)
        painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, self.text)

    def fit(self, text):
        #the text in the theme's plot label font, sized to fill the star's height
        self.prepareGeometryChange()
        self.text = text
        self.height = self.star.RADIUS * AppTheme.ui_scale * (1 + self.star.BELOW)
        font = QFont("Orbitron")
        font.setBold(True)
        self.font = AppTheme.THEME.map_font(font)
        self.font.setPixelSize(max(6, round(self.height * 0.72)))
        self.width = QFontMetricsF(self.font).horizontalAdvance(text) + 2 * self.PAD * self.height

    def headroom(self):
        """How far above its max it and its star reach, in pixels (it at a pop's biggest): the room they need at the
        top of the graph (see MaxStar.top_with_room)"""
        scale = self.star.RADIUS * AppTheme.ui_scale
        height = scale * (1 + self.star.BELOW)
        above = self.star.rest_offset() - scale * (self.star.BELOW - 1) / 2 + (1 + self.POP_GROWTH) * height / 2
        return max(self.star.headroom(), above + 1 + self.star.GAP)

    def step(self, now, avoid):
        """Places it at now (time.monotonic()) beside its star, on a side with room that keeps clear of the rects
        in avoid (scene coordinates: the other stars and bubbles), and adds its own rect to avoid"""
        star = self.star
        if star.x is None or not star.shown:
            self.state = "hidden"
            self.setVisible(False)
            return
        if self.state in ("appearing", "shown") and (star.x, star.y) != (self.x, self.y):
            self.state, self.stateStart = "popping", now
        if self.state == "popping" and now - self.stateStart >= self.POP_SECONDS:
            self.state = "hidden"
        if self.state == "hidden":
            if star.progress(now) < 1:
                self.setVisible(False)
                return
            #the star has arrived: a new one over its max
            self.x, self.y = star.x, star.y
            self.fit(self.fmt.format(self.y))
            self.state, self.stateStart = "appearing", now
        if self.state == "appearing" and now - self.stateStart >= self.APPEAR_SECONDS:
            self.state = "shown"

        #level with where the star rests over its max (the old one while popping; held down from the graph's top as
        #the star is, see MaxStar.step), the star's height top to bottom
        pt = star.vbox.mapViewToScene(QPointF(self.x, self.y))
        scale = star.RADIUS * AppTheme.ui_scale
        area = star.vbox.sceneBoundingRect()
        offset = star.rest_offset() if star.offset is None else star.offset
        centerY = max(pt.y() - offset, area.top() + scale) + scale * (star.BELOW - 1) / 2
        reach = scale * math.cos(math.radians(18)) + star.EDGE_WIDTH / 2 + self.GAP #(the star's half width)

        def rect_on(side):
            centerX = pt.x() + side * (reach + self.width / 2)
            return QRectF(centerX - self.width / 2, centerY - self.height / 2, self.width, self.height)

        sides = (self.side, -self.side)
        fits = [side for side in sides if area.contains(rect_on(side))] or [self.side]
        self.side = next((side for side in fits if not any(rect_on(side).intersects(r) for r in avoid)), fits[0])
        rect = rect_on(self.side)
        avoid.append(rect)
        self.setPos(rect.center())

        t = min(1.0, (now - self.stateStart) / (self.POP_SECONDS if self.state == "popping" else self.APPEAR_SECONDS))
        if self.state == "popping":
            #swells and fades away
            self.setScale(1 + self.POP_GROWTH * t)
            self.setOpacity(1 - t)
        elif self.state == "appearing":
            #grows in, a little past its size and back (ease out back)
            back = 1.7
            self.setScale(0.5 + 0.5 * (1 + (back + 1) * (t - 1) ** 3 + back * (t - 1) ** 2))
            self.setOpacity(min(1.0, 2 * t))
        else:
            self.setScale(1.0)
            self.setOpacity(1.0)
        self.setVisible(area.contains(pt))
