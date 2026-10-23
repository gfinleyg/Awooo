"""
ClassPowerSpiral.py

The HUD's spiral, round and centered in the widget, every lane the same width with SPACE_PX between them, and a hole in
the middle for the grade (ClassGradeWidgets.GradeOverlay asks inner_circle() where it is):

  the ring      the outer lane, a circle: the workout, filled clockwise from North until it meets itself at the end;
                the session's rides before it first (see Session.py), each drawn with its own power zones and a tick
                where it ends
  the spiral    inside it, BLOCK_TURNS turns winding in clockwise from North: the block being ridden, the same length
                whatever the block (an interval's every rep of every sub-block along it, one after the other)

Both are drawn with what was ridden along them, a plot of the cadence across the lane (the inner edge at
CADENCE_MIN_RPM or under, the outer edge from CADENCE_MAX_RPM; the outer edge where there was no cadence to show): a
line, filled from the lane's inner edge up to it in the color of the power's zone (FtpColors.zone_color, power zones in
Musical Cadence Mode too) and see-through above it.

The HUD sets how far along the workout and the block are (set_progress) and adds the power and cadence ridden as it
goes (add_sample); each keeps them in bins, BINS a turn.
Finished stretches are drawn once, CHUNK_BINS at a time; only the stretch the end's in is drawn every frame.
"""

import bisect
import itertools
import math

from PyQt6.QtCore import QPointF, QSize, Qt
from PyQt6.QtGui import QBrush, QColor, QConicalGradient, QPainter, QPainterPath, QPen, QPixmap, QPolygonF
from PyQt6.QtWidgets import QWidget

import AppTheme
import FtpColors
import Session

BINS = 1440 #a turn's, one every quarter degree
CHUNK_BINS = 8 #the finished bins drawn again (and kept) this many at a time
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
#the cadence line, in the theme's text color
LINE_COLOR = "#1E2230"
LINE_OPACITY = 0.9
LINE_PX = 1.5
#the ticks across the ring where the session's earlier rides end, in the cadence line's color
TICK_PX = 2.5
#an end has to move this far (px) before it's drawn again
REDRAW_PX = 0.5


def cadence_share(rpm):
    """How far across its lane the line is at a cadence: None (no cadence to show) the outer edge"""
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
        self._themeColors = (TRACK_COLOR, LINE_COLOR) #(LayoutViewer lists it with the colors a theme can change)
        self.blink = "default"
        self.drawnEnds = None #the ends (px along) when last drawn, see set_progress
        #what's been worked out, until it changes: {name: (key, value)}; all but the spiral's moving turn, drawn; and
        #that turn's finished stretch, drawn
        self.cache = {}
        self.binColors = {} #(timeline serial, colors key): (its version, [power], [QColor]) a bin, see bin_colors
        self.still = (None, None)
        self.movingDone = (None, None)
        self.clear()

    ###what's shown
    def clear(self, workoutDuration=1.0, history=()):
        """Nothing ridden: the ring and the spiral empty. history: the session's rides before this workout, each
        (seconds long, [(seconds, power, cadence)], power ranges), along the ring one after the other before it"""
        self.historySeconds = 0.0
        self.historyRanges = [] #(start, end, power ranges) of each ride in history, in the ring's seconds
        samples = []
        for seconds, rideSamples, ranges in history:
            start = self.historySeconds
            samples += [(start + min(max(at, 0.0), seconds), power, cadence) for at, power, cadence in rideSamples]
            self.historyRanges.append((start, start + seconds, ranges))
            self.historySeconds += seconds
        self.workout = Timeline(self.historySeconds + workoutDuration)
        for sample in samples:
            self.workout.add(*sample)
        self.historyCount = len(samples)
        self.workout.seconds = self.historySeconds
        self.block = Timeline(1.0, BINS*BLOCK_TURNS)
        self.blink = "default"
        self.update()

    def ridden(self):
        """(seconds, [(seconds, power, cadence)] one a second) of this workout's part of the ring, for the session's
        next rides (see Session.py)"""
        samples = [(at - self.historySeconds, power, cadence)
                   for at, power, cadence in self.workout.samples[self.historyCount:]]
        return (max(0.0, self.workout.seconds - self.historySeconds), Session.per_second(samples))

    def set_block(self, duration):
        """A new block, duration seconds long: the spiral empty"""
        self.block = Timeline(duration, BINS*BLOCK_TURNS)
        self.update()

    def set_progress(self, workoutSeconds, workoutDuration, blockSeconds, blockDuration):
        """How far along the workout and the block are (seconds), and how long they are"""
        for timeline, seconds, duration in ((self.workout, self.historySeconds + workoutSeconds,
                                             self.historySeconds + workoutDuration),
                                            (self.block, blockSeconds, blockDuration)):
            timeline.set_duration(duration)
            timeline.seconds = seconds
        #redrawn only once an end has moved enough to see (the HUD sets this every frame)
        if self.ends(self.geometry_now()) != self.drawnEnds:
            self.update()

    def add_sample(self, workoutSeconds, blockSeconds, power, cadence):
        """The power (watts) and cadence (rpm, None: no cadence to show) ridden, workoutSeconds into the workout and
        blockSeconds into the block"""
        self.workout.add(self.historySeconds + workoutSeconds, power, cadence)
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

    def heights(self, g, timeline):
        """heightOf(bin): how far across the lane timeline's bin's line is (None: nothing ridden there yet)"""
        values, lane, last = timeline.values(), g["lane"], timeline.bins - 1
        def heightOf(j):
            power, cadence = values[min(j, last)]
            return None if power is None else lane*cadence_share(cadence)
        return heightOf

    def plot(self, g, radiusAt, start, end, heightOf):
        """(the shape under the line, the line) from angle start to end (radians clockwise from North, 2 pi a turn): a
        point at each bin's middle and at either end, heightOf(bin) px out from the lane's inner edge, the lane's
        middle radiusAt(angle) from the center"""
        center, half = g["center"], g["lane"]/2
        def at(theta, offset):
            r = radiusAt(theta) + offset
            return QPointF(center.x() + r*math.sin(theta), center.y() - r*math.cos(theta))
        step = 2*math.pi/BINS
        middles = [(j + 0.5)*step for j in range(int(start/step), int(end/step) + 1)]
        angles = [start] + [t for t in middles if start + 1e-9 < t < end - 1e-9] + [end]
        bins = [int(start/step + 1e-9)] + [int(t/step) for t in angles[1:-1]] + [max(0, int(end/step - 1e-9))]
        heights = [heightOf(j) for j in bins]
        area, line = QPainterPath(), QPainterPath()
        if end <= start or None in heights:
            return area, line
        top = [at(t, h - half) for t, h in zip(angles, heights)]
        area.addPolygon(QPolygonF(top + [at(t, -half) for t in reversed(angles)]))
        area.closeSubpath()
        line.addPolygon(QPolygonF(top))
        return area, line

    def bin_ranges(self, timeline, ranges):
        """rangesOf(bin): the power ranges a bin's colored by, the ride's it's in (the session's earlier rides keep
        the FTP they were ridden at), else ranges"""
        if timeline is not self.workout or not self.historyRanges:
            return lambda j: ranges
        step = timeline.duration/timeline.bins
        ends = [end for _, end, _ in self.historyRanges]
        def rangesOf(j):
            k = bisect.bisect_right(ends, (j + 0.5)*step)
            return self.historyRanges[k][2] if k < len(ends) else ranges
        return rangesOf

    def bin_colors(self, timeline, ranges, colorsKey):
        """Each of timeline's bins' power's zone color, worked out again only for the bins whose power changed (a new
        sample changes a few; there are BINS a turn)"""
        #(a new length moves the bins along the session's rides, each with its own ranges)
        key = (timeline.serial, colorsKey, timeline.duration)
        version, powers, colors = self.binColors.get(key, (None, [], []))
        if version == timeline.version:
            return colors
        fitted = None
        rangesOf = self.bin_ranges(timeline, ranges)
        for j, (power, _) in enumerate(timeline.values()):
            power = 0 if power is None else power
            if j < len(powers) and powers[j] == power:
                continue
            fitted = fitted or FtpColors.palette()
            color = QColor(FtpColors.zone_hex(power, rangesOf(j), fitted))
            if j < len(powers):
                powers[j], colors[j] = power, color
            else:
                powers.append(power)
                colors.append(color)
        #(only the ring's and the spiral's kept)
        self.binColors = {k: v for k, v in self.binColors.items()
                          if (k[0], k[2]) in ((self.workout.serial, self.workout.duration),
                                              (self.block.serial, self.block.duration))}
        self.binColors[key] = (timeline.version, powers, colors)
        return colors

    def power_brush(self, g, timeline, firstBin, fromBin, toBin, ranges, colorsKey):
        """A turn's colors round the circle, each bin its power's zone color, from timeline's bins from firstBin on, the
        turn's bins fromBin to toBin (Qt's conical gradient runs anticlockwise from North, so a point a fraction f of
        the way round clockwise is at 1 - f; the colors change where one bin meets the next, and carry on past either
        end)"""
        colors = self.bin_colors(timeline, ranges, colorsKey)
        gradient = QConicalGradient(g["center"], 90)
        edge = 0.05/BINS
        stops = []
        #(lowest first: the last bin's first)
        for j in reversed(range(max(0, fromBin), max(fromBin + 1, min(BINS, toBin)))):
            color = colors[min(firstBin + j, timeline.bins - 1)]
            stops += [(1 - (j + 1)/BINS + edge, color), (1 - j/BINS - edge, color)]
        gradient.setStops(stops)
        return QBrush(gradient)

    def done_bins(self, fraction):
        """The bins on a turn fraction of the way round that are drawn and kept: whole CHUNK_BINS, all of them at the
        turn's end"""
        if fraction >= 1:
            return BINS
        return int(fraction*BINS/CHUNK_BINS + 1e-9)*CHUNK_BINS

    def turn_fraction(self, k):
        """How far round the spiral's turn k the block's got"""
        return min(1.0, max(0.0, self.block.fraction()*BLOCK_TURNS - k))

    def turn_key(self, g, k, ends):
        """What the spiral's turn k's drawing depends on: where its end is (what's ridden only changes near the end)"""
        fraction = self.turn_fraction(k)
        return (g["key"], self.block.serial, self.block.duration, k, ends[1] if 0 < fraction < 1 else fraction)

    def paint_ridden(self, painter, name, g, timeline, radiusAt, turn, fraction, colorsKey, ranges, linePen,
                     parts=("done", "tip")):
        """timeline's plot on its turn (the ring's one, or one of the spiral's), fraction of the way round it: the
        finished stretch ("done", up to the middle of its last bin, only worked out again once there's another
        CHUNK_BINS) and the rest ("tip", from a bin before that so the two overlap and no seam shows), each with its
        own bins' colors"""
        if fraction <= 0:
            return
        step = 2*math.pi/BINS
        start, end = 2*math.pi*turn, 2*math.pi*(turn + fraction)
        done = self.done_bins(fraction)
        doneEnd = end if done >= BINS else min(end, start + (done - 0.5)*step) if done else start
        base = (g["key"], timeline.serial, timeline.duration, turn, done)
        heights = self.heights(g, timeline)
        pieces = []
        if "done" in parts and doneEnd > start:
            pieces.append(self.cached((name, "done"), base + (colorsKey,), lambda: (
                self.power_brush(g, timeline, turn*BINS, 0, done, ranges, colorsKey),
                *self.plot(g, radiusAt, start, doneEnd, heights))))
        if "tip" in parts and doneEnd < end:
            pieces.append((self.power_brush(g, timeline, turn*BINS, done - 2, math.ceil(fraction*BINS), ranges, colorsKey),
                           *self.plot(g, radiusAt, max(start, doneEnd - step), end, heights)))
        for brush, area, line in pieces:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(brush)
            painter.drawPath(area)
            painter.setPen(linePen)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawPath(line)
        painter.setPen(Qt.PenStyle.NoPen)

    def paint_ring(self, painter, g, colorsKey, ranges, linePen, parts=("done", "tip")):
        #(all the way round at the end: the end meets the start)
        painter.setOpacity(BLINK_OPACITY.get(self.blink, BLINK_OPACITY["default"])[0])
        self.paint_ridden(painter, "ring", g, self.workout, lambda theta: g["ringR"], 0, self.workout.fraction(),
                          colorsKey, ranges, linePen, parts)

    def paint_ride_ticks(self, painter, g, lineColor):
        """A tick across the ring where each of the session's earlier rides ends"""
        if not self.historyRanges:
            return
        painter.setOpacity(BLINK_OPACITY.get(self.blink, BLINK_OPACITY["default"])[0])
        pen = QPen(lineColor, TICK_PX)
        pen.setCapStyle(Qt.PenCapStyle.FlatCap)
        painter.setPen(pen)
        center, inner, outer = g["center"], g["ringR"] - g["lane"]/2, g["ringR"] + g["lane"]/2
        for _, end, _ in self.historyRanges:
            theta = 2*math.pi*end/self.workout.duration
            out = QPointF(math.sin(theta), -math.cos(theta))
            painter.drawLine(center + out*inner, center + out*outer)
        painter.setPen(Qt.PenStyle.NoPen)

    def paint_turn(self, painter, g, k, colorsKey, ranges, linePen, parts=("done", "tip")):
        """The spiral's turn k, as far as the block's got along it"""
        painter.setOpacity(BLINK_OPACITY.get(self.blink, BLINK_OPACITY["default"])[1])
        self.paint_ridden(painter, ("turn", k), g, self.block, self.spiral_radius(g), k, self.turn_fraction(k),
                          colorsKey, ranges, linePen, parts)

    def layer(self, ratio):
        """A see-through picture the widget's size to draw into, and its painter"""
        picture = QPixmap(round(self.width()*ratio), round(self.height()*ratio))
        picture.setDevicePixelRatio(ratio)
        picture.fill(Qt.GlobalColor.transparent)
        painter = QPainter(picture)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        return picture, painter

    def paintEvent(self, event):
        g = self.geometry_now()
        if g["lane"] < 0.5:
            return
        ends = self.ends(g)
        ranges = FtpColors.power_ranges()
        colorsKey = FtpColors.colors_key(ranges)
        trackColor = QColor(AppTheme.THEME.map_hex(TRACK_COLOR))
        trackColor.setAlphaF(TRACK_OPACITY)
        lineColor = QColor(AppTheme.THEME.map_hex(LINE_COLOR))
        lineColor.setAlphaF(LINE_OPACITY)
        linePen = QPen(lineColor, LINE_PX)
        linePen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        linePen.setCapStyle(Qt.PenCapStyle.FlatCap)
        #everything but the ring and the spiral's turn being ridden (they're what move) drawn once, until any of it
        #changes
        ratio = self.devicePixelRatioF()
        active = min(BLOCK_TURNS - 1, int(self.block.fraction()*BLOCK_TURNS))
        stillKey = (g["key"], ratio, self.blink, active, colorsKey, trackColor.rgba(), lineColor.rgba(),
                    tuple(self.turn_key(g, k, ends) for k in range(BLOCK_TURNS) if k != active))
        if self.still[0] != stillKey:
            still, painter = self.layer(ratio)
            painter.setBrush(trackColor)
            lane = g["lane"]
            painter.drawPath(self.cached("track", g["key"], lambda: self.full_ring(g, lambda t: lane).united(
                self.ribbon(g, self.spiral_radius(g), 0, 2*math.pi*BLOCK_TURNS, lambda t: lane, True, True))))
            for k in range(BLOCK_TURNS):
                if k != active:
                    self.paint_turn(painter, g, k, colorsKey, ranges, linePen)
            painter.end()
            self.still = (stillKey, still)
        #and the ring's and the turn being ridden's finished stretches, until another CHUNK_BINS are: only the rest
        #drawn every time
        doneKey = (g["key"], ratio, self.blink, colorsKey, lineColor.rgba(), self.workout.serial,
                   self.workout.duration, self.done_bins(self.workout.fraction()), self.block.serial,
                   self.block.duration, active, self.done_bins(self.turn_fraction(active)))
        if self.movingDone[0] != doneKey:
            movingDone, painter = self.layer(ratio)
            self.paint_ring(painter, g, colorsKey, ranges, linePen, ("done",))
            self.paint_turn(painter, g, active, colorsKey, ranges, linePen, ("done",))
            painter.end()
            self.movingDone = (doneKey, movingDone)
        painter = QPainter(self)
        painter.drawPixmap(0, 0, self.still[1])
        painter.drawPixmap(0, 0, self.movingDone[1])
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        self.paint_ring(painter, g, colorsKey, ranges, linePen, ("tip",))
        self.paint_ride_ticks(painter, g, lineColor)
        self.paint_turn(painter, g, active, colorsKey, ranges, linePen, ("tip",))
        painter.end()
        self.drawnEnds = ends
