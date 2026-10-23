"""
ClassGradeWidgets.py

What shows the block grades (see WorkoutGrading.py):

  GradeOverlay  "Grade" and the letter in the middle of the HUD's spiral when a block ends, fading out; or, during an
                FTP test's effort, its average power (show_value)
  GradeList     each ridden block's grade beside its usual colored block label (Musical Cadence Mode: each song's,
                its blocks beneath it), with the workout's average grade pinned at the bottom: the HUD's left
                column once the workout is complete, and the synopsis's Block Grades view (there, every ride of the
                session under its own heading, see Session.py)

Both draw themselves at the layout scale (AppTheme.ui_scale), in the theme's fonts and colors.
"""

import time

from PyQt6.QtCore import QPointF, QRectF, Qt, QTimer
from PyQt6.QtGui import QBrush, QColor, QFont, QFontMetricsF, QLinearGradient, QPainter, QPainterPath, QPen
from PyQt6.QtWidgets import QWidget

import AppTheme
import FtpColors
from AppState import AppState
import Session
import WorkoutGrading
from WorkoutHelpers import TargetsFromText, IsSongHeader


def theme_color(hexColor):
    return QColor(AppTheme.THEME.map_hex(hexColor))


def theme_font(family, points, weight=QFont.Weight.Black):
    """A font in the theme's family for Orbitron/Segoe UI Black, at the layout scale"""
    font = QFont(family)
    font.setPointSizeF(points)
    font.setWeight(weight)
    return AppTheme.THEME.map_font(font, AppTheme.ui_scale)


#fitted_font's results, by (font, text, width, height): what's drawn every frame fits its text once
_fittedFonts = {}


def fitted_font(font, text, width, height):
    """font, shrunk if needed so text fits width x height"""
    key = (font.key(), text, width, height)
    fitted = _fittedFonts.get(key)
    if fitted is None:
        fitted = QFont(font)
        for _ in range(40):
            metrics = QFontMetricsF(fitted)
            if (metrics.horizontalAdvance(text) <= width and metrics.height() <= height) or fitted.pointSizeF() <= 5:
                break
            fitted.setPointSizeF(fitted.pointSizeF()*0.92)
        if len(_fittedFonts) >= 256: #(a resize or new theme makes new ones; the old ones are dropped now and then)
            _fittedFonts.clear()
        _fittedFonts[key] = fitted
    return QFont(fitted)


def label_gradient_colors(text, ranges=None):
    """A block label's background colors (start, end), the same as its label everywhere else (see
    FtpColors.block_css): by the watts (or RPM) in its text in these zones (AppState.ftpRanges by default), or the
    free ride colors"""
    targets = TargetsFromText(text)
    if targets is None:
        start, end = FtpColors.free_ride_colors()
    else:
        start, end = FtpColors.block_colors(*targets, ranges=ranges)
    return theme_color(start), theme_color(end)


def draw_badge(painter, rect, grade, points):
    """A grade's rounded, colored badge with its letter"""
    scale = AppTheme.ui_scale
    radius = 12*scale
    painter.setPen(QPen(QColor(0, 0, 0, 140), max(1.0, 2*scale)))
    painter.setBrush(theme_color(WorkoutGrading.grade_color(grade)))
    painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), radius, radius)
    painter.setPen(QColor("black"))
    painter.setFont(fitted_font(theme_font("Orbitron", points), grade, rect.width()*0.86, rect.height()*0.9))
    painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, grade)


###----------------------------------------------------------------------------------------------
### The spiral's grade
###----------------------------------------------------------------------------------------------
class GradeOverlay(QWidget):
    """Covers the spiral (the HUD's PowerSpiral, or a QSpiralProgressBar), drawing the last block's grade inside its
    innermost ring"""
    MAX_SECONDS = 5.0
    MAX_FADE_SECONDS = 1.0

    def __init__(self, spiral):
        super().__init__(spiral)
        self.spiral = spiral
        self.grade = None
        self.title = "Grade"
        self.valueColor = None #a value's color (show_value); a grade is colored by its letter
        self.shownAt = 0.0
        self.showSeconds = 0.0
        self.opacity = 0.0
        #the drawn text's (path, outline width), see draw_outlined
        self.shapes = {}
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.timer = QTimer(self)
        self.timer.setInterval(30)
        self.timer.timeout.connect(self.step)
        spiral.installEventFilter(self)
        self.setGeometry(spiral.rect())
        self.hide()

    def eventFilter(self, obj, event):
        if obj is self.spiral and event.type() == event.Type.Resize:
            self.setGeometry(self.spiral.rect())
        return False

    def show_grade(self, grade, nextBlockSeconds=None):
        """Shows the grade for MAX_SECONDS, or the next block's length if that's shorter, fading out over
        the last of it; a grade already showing is replaced"""
        self.showSeconds = self.MAX_SECONDS if nextBlockSeconds is None else min(self.MAX_SECONDS, nextBlockSeconds)
        if self.showSeconds <= 0:
            return
        self.grade = grade
        self.title = "Grade"
        self.valueColor = None
        self.shownAt = time.monotonic()
        self.opacity = 1.0
        self.setGeometry(self.spiral.rect())
        self.raise_()
        self.show()
        self.timer.start()
        self.update()

    def show_value(self, title, value):
        """Shows title over value until cleared or replaced (no fade); updating the value just redraws it"""
        if self.isVisible() and self.valueColor is not None and (self.title, self.grade) == (title, value):
            return
        self.timer.stop()
        self.title = title
        self.grade = value
        self.valueColor = QColor("white")
        self.opacity = 1.0
        self.setGeometry(self.spiral.rect())
        self.raise_()
        self.show()
        self.update()

    def clear(self):
        self.timer.stop()
        self.grade = None
        self.hide()

    def step(self):
        elapsed = time.monotonic() - self.shownAt
        fade = min(self.MAX_FADE_SECONDS, self.showSeconds/2)
        if elapsed >= self.showSeconds:
            self.clear()
            return
        self.opacity = 1.0 if elapsed < self.showSeconds - fade else (self.showSeconds - elapsed)/fade
        self.update()

    def inner_circle(self):
        """(center, radius) of the space inside the spiral's innermost ring"""
        s = self.spiral
        if hasattr(s, "inner_circle"): #(the HUD's PowerSpiral says where its hole is)
            center, radius = s.inner_circle()
            if radius > 10:
                return center, radius
        try:
            ring = s.noProgBar - 1
            offset = s.spb_cngSize*ring*s.spb_gap
            size = s.spb_Size - s.sizeFactor - 2*offset
            left = s.positionX + s.posFactor + offset
            top = s.positionY + s.posFactor + offset
            center = QPointF(left + size/2, top + size/2)
            radius = size/2 - s.lineWidth
            if radius > 10:
                return center, radius
        except AttributeError: #not painted yet
            pass
        rect = QRectF(self.rect())
        return rect.center(), min(rect.width(), rect.height())*0.3

    def paintEvent(self, event):
        if not self.grade:
            return
        center, radius = self.inner_circle()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setOpacity(self.opacity)
        #"Grade" across the top of the circle, the letter as big as fits below it
        wordRect = QRectF(center.x() - radius*0.8, center.y() - radius*0.78, radius*1.6, radius*0.42)
        letterRect = QRectF(center.x() - radius*0.85, center.y() - radius*0.36, radius*1.7, radius*1.05)
        self.draw_outlined(painter, self.title, wordRect, QColor("white"), 26)
        fill = self.valueColor or theme_color(WorkoutGrading.grade_color(self.grade))
        self.draw_outlined(painter, self.grade, letterRect, fill, 110)

    def draw_outlined(self, painter, text, rect, fill, points):
        #(the overlay repaints with the spiral under it, every frame: the text's shape is only worked out again when
        #the text, its room or the theme's font changes)
        family = AppTheme.THEME.fonts[AppTheme.ROLE_MAIN]["family"]
        key = (text, rect.x(), rect.y(), rect.width(), rect.height(), points, family)
        shape = self.shapes.get(key)
        if shape is None:
            shape = self.text_shape(text, rect, points, family)
            if len(self.shapes) >= 16:
                self.shapes.clear()
            self.shapes[key] = shape
        path, outline = shape
        painter.setPen(QPen(QColor(0, 0, 0, 220), outline*2, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap,
                            Qt.PenJoinStyle.RoundJoin))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawPath(path)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(fill)
        painter.drawPath(path)

    @staticmethod
    def text_shape(text, rect, points, family):
        """(path, outline width) of text centered in rect, as big as fits up to points"""
        font = QFont(family)
        font.setWeight(QFont.Weight.Black)
        font.setPointSizeF(points)
        font = fitted_font(font, text, rect.width(), rect.height())
        metrics = QFontMetricsF(font)
        width = metrics.horizontalAdvance(text)
        #baseline so the text's capitals sit centered in rect
        baseline = rect.center().y() + metrics.capHeight()/2
        path = QPainterPath()
        path.addText(QPointF(rect.center().x() - width/2, baseline), font, text)
        path.setFillRule(Qt.FillRule.WindingFill) #a "+" is two overlapping strokes; don't leave a hole where they cross
        return path, max(1.5, font.pointSizeF()*0.06)


###----------------------------------------------------------------------------------------------
### Each block's grade
###----------------------------------------------------------------------------------------------
class GradeList(QWidget):
    """Rows of grade badge + block label for the blocks ridden, in workout order (see WorkoutGrading.workout_rows), scrollable
    by wheel or drag with "more..." rows where some are out of view (like the HUD's block lists), and
    the workout's average grade pinned below them"""
    ROW_HEIGHT = 42
    ROW_GAP = 4
    BADGE_WIDTH = 78
    INDENT = 18
    AVERAGE_HEIGHT = 64
    MORE_TEXT = "more..."
    LABEL_POINTS = 14
    PANEL_COLOR = "#9fc5d1"
    AVERAGE_PANEL_COLOR = "#bdd6de"
    #a song's label: the song header's colors in the workout lists (WorkoutHelpers.SONG_HEADER_CSS)
    SONG_COLORS = ("#28275e", "#9fc5d1")
    #a ride's heading (the synopsis's session): darker, the same way round
    RIDE_COLORS = ("#121230", "#4f7fa0")

    def __init__(self):
        super().__init__()
        self.rows = []
        self.average = None
        self.averageText = "Workout Average"
        self.scrollOffset = 0
        self.wheelRemainder = 0
        self.dragStartY = None
        self.dragStartOffset = 0
        self.setMinimumHeight(200)
        FtpColors.watch(self, "update")

    def refresh(self, grades=None, rides=None):
        """Shows these grades (AppState.blockGrades by default), or the session's rides' (see Session.py): each
        under a heading with its own average, the graded ones' blocks with the zones it was ridden in, and the
        average of all of them at the bottom"""
        self.scrollOffset = 0
        if rides is not None:
            self.rows, records = [], []
            for ride in rides:
                average = Session.average(ride)
                self.rows.append({"kind": "ride", "label": Session.heading(ride), "pct": average,
                                  "grade": WorkoutGrading.letter(average), "note": Session.note(ride),
                                  "graded": ride["graded"]})
                if ride["graded"]:
                    rows = WorkoutGrading.workout_rows(ride["workoutList"], ride["blockGrades"], ride["songHeaders"])
                    for row in rows:
                        row["ranges"] = ride["targetRanges"]
                    self.rows += rows
                    records += ride["blockGrades"].values()
            self.average = WorkoutGrading.ridden_average(records)
            graded = sum(ride["graded"] for ride in rides)
            self.averageText = "Session Average" if graded > 1 else "Workout Average"
            self.update()
            return
        grades = AppState.blockGrades if grades is None else grades
        #(Musical Cadence Mode's song headers are "<length> : <Artist> - <Title>")
        songs = {index: header.split(" : ", 1)[1] for index, header in AppState.musicSongStarts.items()
                 if IsSongHeader(header)}
        self.rows = WorkoutGrading.workout_rows(AppState.workoutList, grades, songs)
        self.average = WorkoutGrading.workout_average(grades)
        self.averageText = "Workout Average"
        self.update()

    def scaled(self, value):
        return value*AppTheme.ui_scale

    def row_step(self):
        return self.scaled(self.ROW_HEIGHT + self.ROW_GAP)

    def list_height(self):
        return self.height() - self.scaled(self.AVERAGE_HEIGHT + 2*self.ROW_GAP)

    def shown_rows(self):
        """The rows in view (with "more..." in the top/bottom slot when rows are out of view that way)"""
        maxRows = max(1, int((self.list_height() + self.scaled(self.ROW_GAP))/self.row_step()))
        if len(self.rows) <= maxRows or maxRows < 3:
            self.scrollOffset = 0
            return self.rows[:maxRows]
        #furthest scroll leaves the last row in the bottom slot, beneath a top "more..."
        self.scrollOffset = max(0, min(self.scrollOffset, len(self.rows) - maxRows + 1))
        shown = [{"kind": "more"}] if self.scrollOffset > 0 else []
        available = maxRows - len(shown)
        remaining = self.rows[self.scrollOffset:]
        if len(remaining) > available:
            shown += remaining[:available - 1] + [{"kind": "more"}]
        else:
            shown += remaining
        return shown

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        width = self.width()
        rowHeight = self.scaled(self.ROW_HEIGHT)
        badgeWidth = self.scaled(self.BADGE_WIDTH)
        gap = self.scaled(6)
        labelFont = theme_font("Orbitron", self.LABEL_POINTS)
        y = 0.0
        for row in self.shown_rows():
            labelLeft = badgeWidth + gap + (self.scaled(self.INDENT) if row["kind"] == "sub" or row.get("indent") else 0)
            labelRect = QRectF(labelLeft, y, width - labelLeft, rowHeight)
            if row["kind"] == "more":
                self.draw_label(painter, QRectF(badgeWidth + gap, y, width - badgeWidth - gap, rowHeight), self.MORE_TEXT,
                                labelFont, theme_color(self.PANEL_COLOR), theme_color(self.PANEL_COLOR))
            elif row["kind"] == "interval":
                painter.setPen(QColor("white"))
                painter.setFont(theme_font("Orbitron", 20))
                painter.drawText(labelRect.adjusted(self.scaled(8), 0, 0, 0),
                                 Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, row["label"])
            elif row["kind"] == "ride":
                #(one not graded has no badge: its heading takes the badge's room too)
                if row["graded"]:
                    draw_badge(painter, QRectF(0, y, badgeWidth, rowHeight), row["grade"], 17)
                rect = QRectF(badgeWidth + gap if row["graded"] else 0, y, 0, rowHeight)
                rect.setRight(width)
                start, end = (theme_color(color) for color in self.RIDE_COLORS)
                self.draw_label(painter, rect, row["label"], labelFont, start, end, row["note"],
                                textColor=QColor("white"))
            elif row["kind"] == "song":
                draw_badge(painter, QRectF(0, y, badgeWidth, rowHeight), row["grade"], 17)
                start, end = (theme_color(color) for color in self.SONG_COLORS)
                self.draw_label(painter, labelRect, "♪ " + row["label"], labelFont, start, end, textColor=QColor("white"))
            else:
                draw_badge(painter, QRectF(0, y, badgeWidth, rowHeight), row["grade"], 17)
                start, end = label_gradient_colors(row["label"], row.get("ranges"))
                self.draw_label(painter, labelRect, row["label"], labelFont, start, end, row["note"])
            y += self.row_step()
        self.draw_average(painter)

    def draw_label(self, painter, rect, text, font, start, end, note="", textColor=None):
        """A block label: rounded, the zone gradient behind its text, a note at the right (each rep's grade, ...)"""
        gradient = QLinearGradient(rect.topLeft(), rect.bottomRight())
        gradient.setColorAt(0, start)
        gradient.setColorAt(1, end)
        radius = self.scaled(15)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QBrush(gradient))
        painter.drawRoundedRect(rect, radius, radius)
        inner = rect.adjusted(self.scaled(14), 0, -self.scaled(10), 0)
        painter.setPen(textColor or QColor("black"))
        if note:
            noteFont = theme_font("Orbitron", 10)
            noteWidth = min(QFontMetricsF(noteFont).horizontalAdvance(note), inner.width()*0.45)
            painter.setFont(noteFont)
            painter.drawText(inner, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight,
                             QFontMetricsF(noteFont).elidedText(note, Qt.TextElideMode.ElideRight, noteWidth))
            inner.setRight(inner.right() - noteWidth - self.scaled(8))
        painter.setFont(font)
        painter.drawText(inner, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                         QFontMetricsF(font).elidedText(text, Qt.TextElideMode.ElideRight, inner.width()))

    def draw_average(self, painter):
        height = self.scaled(self.AVERAGE_HEIGHT)
        top = self.height() - height
        #a light rule above the average
        painter.setPen(QPen(QColor(255, 255, 255, 150), max(1.0, self.scaled(3))))
        painter.drawLine(QPointF(0, top - self.scaled(self.ROW_GAP)), QPointF(self.width(), top - self.scaled(self.ROW_GAP)))
        badgeWidth = self.scaled(self.BADGE_WIDTH)
        grade = WorkoutGrading.letter(self.average)
        draw_badge(painter, QRectF(0, top, badgeWidth, height), grade, 24)
        rect = QRectF(badgeWidth + self.scaled(6), top, self.width() - badgeWidth - self.scaled(6), height)
        panel = theme_color(self.AVERAGE_PANEL_COLOR)
        radius = self.scaled(15)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(panel)
        painter.drawRoundedRect(rect, radius, radius)
        inner = rect.adjusted(self.scaled(14), 0, -self.scaled(14), 0)
        font = theme_font("Orbitron", 16)
        painter.setFont(font)
        painter.setPen(QColor("black"))
        pctText = "" if self.average is None else f"{self.average:.1f}%"
        painter.drawText(inner, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight, pctText)
        inner.setRight(inner.right() - QFontMetricsF(font).horizontalAdvance(pctText) - self.scaled(10))
        painter.drawText(inner, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                         QFontMetricsF(font).elidedText(self.averageText, Qt.TextElideMode.ElideRight, inner.width()))

    #scrolling, the same as the HUD's block lists
    def scroll_to(self, offset):
        if offset != self.scrollOffset:
            self.scrollOffset = max(0, offset) #shown_rows clamps the bottom end
            self.update()

    def wheelEvent(self, event):
        self.wheelRemainder += event.angleDelta().y()
        steps = int(self.wheelRemainder/120)
        if steps:
            self.wheelRemainder -= steps*120
            self.scroll_to(self.scrollOffset - steps)
        event.accept()

    def mousePressEvent(self, event):
        self.dragStartY = event.position().y()
        self.dragStartOffset = self.scrollOffset
        event.accept()

    def mouseMoveEvent(self, event):
        if self.dragStartY is not None:
            self.scroll_to(self.dragStartOffset + int((self.dragStartY - event.position().y())/self.row_step()))
        event.accept()

    def mouseReleaseEvent(self, event):
        self.dragStartY = None
        event.accept()
