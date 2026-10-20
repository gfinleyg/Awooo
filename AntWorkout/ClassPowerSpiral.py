"""
ClassPowerSpiral.py

The HUD's spiral, round and centered in the widget, every lane the same width with SPACE_PX between them, and a hole in
the middle for the grade (ClassGradeWidgets.GradeOverlay asks inner_circle() where it is):

  the ring      the outer lane, a circle: the workout, filled clockwise from North until it meets itself at the end
  the spiral    inside it, BLOCK_TURNS turns winding in clockwise from North: the block being ridden, the same length
                whatever the block (an interval's every rep of every sub-block along it, one after the other)

Both are drawn with what was ridden along them: as thick as the cadence (nothing at CADENCE_MIN_RPM or under, the whole
lane from CADENCE_MAX_RPM; the whole lane where there was no cadence to show), and colored by the power, its power zone's color
(FtpColors.zone_color, power zones in Musical Cadence Mode too).

The HUD sets how far along the workout and the block are (set_progress) and adds the power and cadence ridden as it
goes (add_sample); each keeps them in bins, BINS a turn.
"""

import itertools
import math

from PyQt6.QtCore import QPointF, QSize, Qt
from PyQt6.QtGui import QBrush, QColor, QConicalGradient, QPainter, QPainterPath, QPixmap, QPolygonF
from PyQt6.QtWidgets import QWidget

import AppTheme
import FtpColors

BINS = 180 #a turn's, one every 2 degrees
BLOCK_TURNS = 3
CADENCE_MIN_RPM = 40
CADENCE_MAX_RPM = 105
SPACE_PX = 1.0
HOLE = 0.36 #the hole's radius, as a share of the spiral's
#the lanes, faint, in the theme's text color
TRACK_COLOR = "#000000"
TRACK_OPACITY = 0.09
#blinking (see set_blink): (the ring's, the spiral's) opacity
BLINK_OPACITY = {"default": (1.0, 1.0), "block": (1.0, 0.13), "workout": (0.13, 0.53)}
#an end has to move this far (px) before it's drawn again
REDRAW_PX = 0.5
#a finished spiral turn runs this far on under the next one, so no seam shows where they meet
OVERLAP = math.radians(0.5)


def cadence_share(rpm):
    """How much of its lane the line fills at a cadence: None (no cadence to show) the whole lane"""
    if rpm is None:
        return 1.0
    return min(1.0, max(0.0, (rpm - CADENCE_MIN_RPM)/(CADENCE_MAX_RPM - CADENCE_MIN_RPM)))


class Timeline:
    """A length of time (seconds), how far along it is and what was ridden along it, in bins"""
    _serials = itertools.count()

    def __init__(self, duration, bins=BINS):
        self.serial = next(Timeline._serials) #(which one it is, for what's drawn of it: a new block's isn't the last's)
        self.bins = bins
        self.duration = max(float(duration), 1e-6)
        self.seconds = 0.0
        self.samples = [] #(seconds, power, cadence), to bin again if the length changes (a ramp test grows)
        self.sums = [[0.0, 0.0, 0, 0] for _ in range(bins)] #power sum, cadence sum, samples, of them with a cadence
        self.version = 0
        self._values = (None, None)

    def fraction(self):
        return min(1.0, max(0.0, self.seconds/self.duration))

    def set_duration(self, duration):
        duration = max(float(duration), 1e-6)
        if duration == self.duration:
            return
        self.duration = duration
        self.sums = [[0.0, 0.0, 0, 0] for _ in range(self.bins)]
        for sample in self.samples:
            self._bin(*sample)
        self.version += 1

    def add(self, seconds, power, cadence):
        sample = (min(max(seconds, 0.0), self.duration), power, cadence)
        self.samples.append(sample)
        self._bin(*sample)
        self.version += 1

    def _bin(self, seconds, power, cadence):
        sums = self.sums[min(self.bins - 1, int(seconds/self.duration*self.bins))]
        sums[0] += power
        sums[2] += 1
        if cadence is not None:
            sums[1] += cadence
            sums[3] += 1

    def values(self):
        """(power, cadence) per bin: each bin's average, a bin nothing landed in the one before's, the bins before the
        first sample (it comes a tick in) the first's; (None, None) all along with nothing ridden, a cadence None
        where none was shown"""
        version, values = self._values
        if version == self.version:
            return values
        values, last = [], (None, None)
        for powerSum, cadenceSum, count, cadenceCount in self.sums:
            if count:
                last = (powerSum/count, cadenceSum/cadenceCount if cadenceCount else None)
                if values and values[0][0] is None:
                    values = [last]*len(values)
            values.append(last)
        self._values = (self.version, values)
        return values


class PowerSpiral(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumSize(QSize(60, 60))
        self._themeColors = (TRACK_COLOR,) #(LayoutViewer lists it with the colors a theme can change)
        self.blink = "default"
        self.drawnEnds = None #the ends (px along) when last drawn, see set_progress
        #what's been worked out, until it changes: {name: (key, value)}; and all but the spiral's moving turn, drawn
        self.cache = {}
        self.still = (None, None)
        self.clear()

    ###what's shown
    def clear(self, workoutDuration=1.0):
        """Nothing ridden: the ring and the spiral empty"""
        self.workout = Timeline(workoutDuration)
        self.block = Timeline(1.0, BINS*BLOCK_TURNS)
        self.blink = "default"
        self.update()

    def set_block(self, duration):
        """A new block, duration seconds long: the spiral empty"""
        self.block = Timeline(duration, BINS*BLOCK_TURNS)
        self.update()

    def set_progress(self, workoutSeconds, workoutDuration, blockSeconds, blockDuration):
        """How far along the workout and the block are (seconds), and how long they are"""
        for timeline, seconds, duration in ((self.workout, workoutSeconds, workoutDuration),
                                            (self.block, blockSeconds, blockDuration)):
            timeline.set_duration(duration)
            timeline.seconds = seconds
        #redrawn only once an end has moved enough to see (the HUD sets this every frame)
        if self.ends(self.geometry_now()) != self.drawnEnds:
            self.update()

    def add_sample(self, workoutSeconds, blockSeconds, power, cadence):
        """The power (watts) and cadence (rpm, None: no cadence to show) ridden, workoutSeconds into the workout and
        blockSeconds into the block"""
        self.workout.add(workoutSeconds, power, cadence)
        self.block.add(blockSeconds, power, cadence)

    def set_blink(self, state):
        """"default", "block" (the spiral fades) or "workout" (the ring fades, the spiral some)"""
        if state != self.blink:
            self.blink = state
            self.update()

    ###where things are
    def geometry_now(self):
        """The center, the hole's radius, the distance from one lane to the next (pitch), a lane's width, the ring's
        middle radius and the spiral's where it starts: the ring's outer edge touches the widget, the spiral's last
        lane's inner edge the hole"""
        size = min(self.width(), self.height())
        rMax = size/2
        rInner = rMax*HOLE
        pitch = max(1.0, (rMax - rInner + SPACE_PX)/(BLOCK_TURNS + 2))
        lane = max(0.0, pitch - SPACE_PX)
        ringR = rMax - lane/2
        return {"center": QPointF(self.width()/2, self.height()/2), "rInner": rInner, "pitch": pitch, "lane": lane,
                "ringR": ringR, "spiralR": ringR - pitch, "key": (self.width(), self.height())}

    def inner_circle(self):
        """(center, radius) of the hole in the middle"""
        g = self.geometry_now()
        return g["center"], g["rInner"] - SPACE_PX

    def spiral_radius(self, g):
        return lambda theta: g["spiralR"] - g["pitch"]*theta/(2*math.pi)

    def ends(self, g):
        """How far along (px, in REDRAW_PX steps) the ring's and the spiral's ends are: drawn again once they move"""
        return (round(self.workout.fraction()*2*math.pi*g["ringR"]/REDRAW_PX),
                round(self.block.fraction()*BLOCK_TURNS*2*math.pi*g["spiralR"]/REDRAW_PX))

    def cached(self, name, key, make):
        entry = self.cache.get(name)
        if entry is None or entry[0] != key:
            entry = (key, make())
            self.cache[name] = entry
        return entry[1]

    ###drawing
    def ribbon(self, g, radiusAt, start, end, widthAt, capStart, capEnd):
        """The line's outline from angle start to end (radians clockwise from North, 2 pi a turn), its middle
        radiusAt(angle) from the center, widthAt(angle) px wide, with a round end at either end if asked"""
        center = g["center"]
        def at(theta, offset):
            r = radiusAt(theta) + offset
            return QPointF(center.x() + r*math.sin(theta), center.y() - r*math.cos(theta))
        def cap(theta, width, forward):
            #half a circle round the end: from one edge to the other through the way the line goes (or came)
            middle = at(theta, 0)
            out = QPointF(math.sin(theta), -math.cos(theta))
            along = QPointF(math.cos(theta), math.sin(theta))*(1 if forward else -1)
            sign = 1 if forward else -1
            return [middle + (out*math.cos(math.pi*k/12)*sign + along*math.sin(math.pi*k/12))*(width/2)
                    for k in range(1, 12)]
        steps = max(2, int((end - start)/math.radians(1)))
        angles = [start + (end - start)*k/steps for k in range(steps + 1)]
        widths = [widthAt(t) for t in angles]
        points = [at(t, w/2) for t, w in zip(angles, widths)]
        if capEnd and widths[-1] > 0.5:
            points += cap(end, widths[-1], True)
        points += [at(t, -w/2) for t, w in zip(reversed(angles), reversed(widths))]
        if capStart and widths[0] > 0.5:
            points += cap(start, widths[0], False)
        path = QPainterPath()
        path.addPolygon(QPolygonF(points))
        path.closeSubpath()
        return path

    def full_ring(self, g, widthAt):
        """The whole ring, its end meeting its start at North: its outer edge all the way round and its inner edge,
        filled between them"""
        center, ringR = g["center"], g["ringR"]
        angles = [2*math.pi*k/360 for k in range(360)]
        path = QPainterPath()
        path.setFillRule(Qt.FillRule.OddEvenFill)
        for side in (1, -1):
            path.addPolygon(QPolygonF([QPointF(center.x() + (ringR + side*widthAt(t)/2)*math.sin(t),
                                               center.y() - (ringR + side*widthAt(t)/2)*math.cos(t)) for t in angles]))
            path.closeSubpath()
        return path

    def width_along(self, g, timeline):
        """widthAt(angle) along timeline, a turn of its bins each time round the circle"""
        values, lane, last = timeline.values(), g["lane"], timeline.bins - 1
        def widthAt(theta):
            #between the middles of the bins either side
            x = min(float(last), max(0.0, theta/(2*math.pi)*BINS - 0.5))
            j = min(last - 1, int(x))
            a, b = values[j], values[j + 1]
            if a[0] is None: #nothing ridden here yet
                return 0.0
            if b[0] is None:
                return lane*cadence_share(a[1])
            return lane*(cadence_share(a[1])*(1 - (x - j)) + cadence_share(b[1])*(x - j))
        return widthAt

    def power_brush(self, g, timeline, firstBin, fraction, ranges):
        """A turn's colors round the circle, from timeline's bins from firstBin on, fraction of the way round (Qt's
        conical gradient runs anticlockwise from North, so a point a fraction f of the way round clockwise is at
        1 - f)"""
        values = timeline.values()
        gradient = QConicalGradient(g["center"], 90)
        lastPower = next((p for p, _ in values if p is not None), 0)
        for j in range(max(1, min(BINS, math.ceil(fraction*BINS))) + 1):
            power = values[min(firstBin + j, timeline.bins - 1)][0]
            lastPower = lastPower if power is None else power
            gradient.setColorAt(min(1.0, max(0.0, 1 - min(fraction, j/BINS))), FtpColors.zone_color(lastPower, ranges))
        return QBrush(gradient)

    def ring_key(self, g, ends):
        return (g["key"], self.workout.serial, self.workout.duration, ends[0])

    def turn_fraction(self, k):
        """How far round the spiral's turn k the block's got"""
        return min(1.0, max(0.0, self.block.fraction()*BLOCK_TURNS - k))

    def turn_key(self, g, k, ends):
        """What the spiral's turn k's drawing depends on: where its end is (what's ridden only changes near the end)"""
        fraction = self.turn_fraction(k)
        return (g["key"], self.block.serial, self.block.duration, k, ends[1] if 0 < fraction < 1 else fraction)

    def paint_ring(self, painter, g, ends, colorsKey, ranges):
        fraction = self.workout.fraction()
        if fraction <= 0:
            return
        key = self.ring_key(g, ends)
        def make():
            widthAt = self.width_along(g, self.workout)
            if fraction >= 1:
                return self.full_ring(g, widthAt)
            #(no round start: the end meets it)
            return self.ribbon(g, lambda theta: g["ringR"], 0, 2*math.pi*fraction, widthAt, False, True)
        painter.setOpacity(BLINK_OPACITY.get(self.blink, BLINK_OPACITY["default"])[0])
        painter.setBrush(self.cached("ringBrush", key + (colorsKey,),
                                     lambda: self.power_brush(g, self.workout, 0, fraction, ranges)))
        painter.drawPath(self.cached("ring", key, make))

    def paint_turn(self, painter, g, k, ends, colorsKey, ranges):
        """The spiral's turn k, as far as the block's got along it"""
        fraction = self.turn_fraction(k)
        if fraction <= 0:
            return
        key = self.turn_key(g, k, ends)
        last = k == BLOCK_TURNS - 1
        def make():
            start, end = 2*math.pi*k, 2*math.pi*(k + fraction)
            if fraction >= 1 and not last:
                end += OVERLAP
            return self.ribbon(g, self.spiral_radius(g), start, end, self.width_along(g, self.block), k == 0,
                               fraction < 1 or last)
        painter.setOpacity(BLINK_OPACITY.get(self.blink, BLINK_OPACITY["default"])[1])
        painter.setBrush(self.cached(("turnBrush", k), key + (colorsKey,),
                                     lambda: self.power_brush(g, self.block, k*BINS, fraction, ranges)))
        painter.drawPath(self.cached(("turn", k), key, make))

    def paintEvent(self, event):
        g = self.geometry_now()
        if g["lane"] < 0.5:
            return
        ends = self.ends(g)
        ranges = FtpColors.power_ranges()
        colorsKey = FtpColors.colors_key(ranges)
        trackColor = QColor(AppTheme.THEME.map_hex(TRACK_COLOR))
        trackColor.setAlphaF(TRACK_OPACITY)
        #everything but the spiral's turn being ridden (it's what moves) drawn once, until any of it changes
        ratio = self.devicePixelRatioF()
        active = min(BLOCK_TURNS - 1, int(self.block.fraction()*BLOCK_TURNS))
        stillKey = (g["key"], ratio, self.blink, active, colorsKey, trackColor.rgba(), self.ring_key(g, ends),
                    tuple(self.turn_key(g, k, ends) for k in range(BLOCK_TURNS) if k != active))
        if self.still[0] != stillKey:
            still = QPixmap(round(self.width()*ratio), round(self.height()*ratio))
            still.setDevicePixelRatio(ratio)
            still.fill(Qt.GlobalColor.transparent)
            painter = QPainter(still)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(trackColor)
            lane = g["lane"]
            painter.drawPath(self.cached("track", g["key"], lambda: self.full_ring(g, lambda t: lane).united(
                self.ribbon(g, self.spiral_radius(g), 0, 2*math.pi*BLOCK_TURNS, lambda t: lane, True, True))))
            self.paint_ring(painter, g, ends, colorsKey, ranges)
            #(each turn under the one after it: a finished turn runs on under the next)
            for k in range(BLOCK_TURNS):
                if k != active:
                    self.paint_turn(painter, g, k, ends, colorsKey, ranges)
            painter.end()
            self.still = (stillKey, still)
        painter = QPainter(self)
        painter.drawPixmap(0, 0, self.still[1])
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        self.paint_turn(painter, g, active, ends, colorsKey, ranges)
        painter.end()
        self.drawnEnds = ends
