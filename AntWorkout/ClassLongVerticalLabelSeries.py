import math

from PyQt6.QtWidgets import QLabel, QWidget, QVBoxLayout
from PyQt6.QtCore import pyqtSignal, QMetaObject, QCoreApplication, QSize, QTimer

import FtpColors
from WorkoutHelpers import TargetsFromText, IsSongHeader, SONG_HEADER_CSS

#Column of colored block labels that fits the widget height, scrollable by wheel or drag
#shows a "more..." block at the top/bottom when blocks are scrolled out of view above/below
class LongVLabelSerial(QWidget):

    textChanged = pyqtSignal(str)

    def __init__(self, top, ftpStr="170", ftpRanges=[85,136,221,272]):
        super(LongVLabelSerial,self).__init__()
        self.top = top #boolean storing if the top of the text is shown (True) or the bottom (False)
        self.ftp = int(ftpStr)
        self.ftpRanges = ftpRanges
        self.fullText = ""
        self.moreText = "more..."
        self.showAlmostDone = True #an empty list shows an "Almost done!" block
        self.scrollOffset = 0 #index of the first line shown
        self.wheelRemainder = 0
        self.dragStartY = None
        self.dragStartOffset = 0
        #two layouts in the widget, one to measure available size and one to store added widgets
        self.layoutMain = QVBoxLayout()
        self.layoutMain.setContentsMargins(0, 0, 0, 0) #keep labels flush with the Up Next box
        self.layoutH = QVBoxLayout()
        #start with 100 widgets, hidden
        self.widgetCount = 30
        self.qlabels = [QLabel() for x in range(self.widgetCount)]
        for x in self.qlabels:
            x.setFixedSize(500,42)
            self.layoutH.addWidget(x)
            x.hide()
        self.layoutMain.addLayout(self.layoutH)
        self.setLayout(self.layoutMain)

        #the lines are laid out once the widget has its size for the new text (and again on each resize): a
        #render queued for the event loop, however many changes come in before it runs
        self.renderTimer = QTimer(self)
        self.renderTimer.setSingleShot(True)
        self.renderTimer.setInterval(0)
        self.renderTimer.timeout.connect(self.render_lines)
        self.textChanged.connect(self.make_reduced_lines_text)
        #redrawn when the power zone colors change (see FtpColors.py)
        FtpColors.watch(self, "render_lines")
        for label in self.qlabels:
            label._ftpColored = True

    def row_height(self):
        #label height plus layout spacing, at the current layout scale (see AppLayout.py)
        return self.qlabels[0].minimumHeight() + max(self.layoutH.spacing(), 0)

    def minimumSizeHint(self):
        #shows as many rows as fit, so it needs room for just one: the rows it's showing mustn't keep it (and
        #the screen) from getting smaller, e.g. on a smaller screen
        return QSize(super().minimumSizeHint().width(), math.ceil(self.row_height()))

    def make_reduced_lines_text(self):
        #(not right away: the layout may still be resizing this, see renderTimer)
        self.renderTimer.start()

    #show the window of lines starting at scrollOffset, with a "more..." block in the top/bottom
    #slot whenever there are lines scrolled out of view above/below
    def render_lines(self):
        for x in self.qlabels:
            x.hide()
        maxLines = min(int(self.geometry().height()/self.row_height()), self.widgetCount)
        lines = [line for line in self.fullText.split("\n") if line.strip() != ""]
        if self.fullText == "":
            shown = ["Almost done!"] if self.showAlmostDone else []
        elif self.fullText == "DONE":
            geo = self.geometry()
            self.setFixedSize(geo.width(), geo.height())
            return
        elif len(lines) <= maxLines or maxLines < 3:
            #all the text fits (or there's no room for "more..." blocks), no scrolling
            self.scrollOffset = 0
            shown = lines[:maxLines]
        else:
            #furthest scroll leaves the last line in the bottom slot, beneath a top "more..."
            self.scrollOffset = max(0, min(self.scrollOffset, len(lines) - maxLines + 1))
            shown = []
            if self.scrollOffset > 0:
                shown.append(self.moreText)
            available = maxLines - len(shown)
            remaining = lines[self.scrollOffset:]
            if len(remaining) > available:
                shown += remaining[:available-1] + [self.moreText]
            else:
                shown += remaining
        for i, txt in enumerate(shown):
            self.qlabels[i].setStyleSheet(self.getLabelColor(txt))
            self.qlabels[i].setText(txt)
            self.qlabels[i].show()

    def scroll_to(self, offset):
        if offset != self.scrollOffset:
            self.scrollOffset = max(0, offset) #render_lines clamps the bottom end
            self.render_lines()

    def wheelEvent(self, event):
        #one line per wheel notch (120), touchpads send smaller steps that accumulate
        self.wheelRemainder += event.angleDelta().y()
        steps = int(self.wheelRemainder / 120)
        if steps:
            self.wheelRemainder -= steps * 120
            self.scroll_to(self.scrollOffset - steps)
        event.accept()

    #drag (mouse or touch) to scroll, one line per row height dragged
    def mousePressEvent(self, event):
        self.dragStartY = event.position().y()
        self.dragStartOffset = self.scrollOffset
        event.accept()

    def mouseMoveEvent(self, event):
        if self.dragStartY is not None:
            lines = int((self.dragStartY - event.position().y()) / self.row_height())
            self.scroll_to(self.dragStartOffset + lines)
        event.accept()

    def mouseReleaseEvent(self, event):
        self.dragStartY = None
        event.accept()

    def getLabelColor(self, txt):
        if txt in ["Almost done!", self.moreText]:
            return "background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #9fc5d1 stop:1 #9fc5d1);"
        if IsSongHeader(txt):
            return SONG_HEADER_CSS
        targets = TargetsFromText(txt)
        if targets is None:
            #free ride
            return FtpColors.free_ride_css()
        return FtpColors.block_css(targets[0], targets[1], self.ftpRanges)

    def setFullText(self, textInput):
        self.fullText = textInput
        self.scrollOffset = 0 #new list, start back at the top
        self.textChanged.emit(str(textInput))

    def resizeEvent(self, event):
        self.make_reduced_lines_text()


class Ui_Form(object):
    def setupUi(self, Form, top, ftpStr, ftpRanges):
        Form.setObjectName("Form")
        # Form.resize(400, 300)
        self.vLayout = QVBoxLayout(Form)
        self.vLayout.setObjectName("vLayout")
        self.vLayout.setContentsMargins(0, 0, 0, 0)
        self.widget = LongVLabelSerial(top, ftpStr, ftpRanges)
        self.widget.setObjectName("widget")
        self.vLayout.addWidget(self.widget)

        self.retranslateUi(Form)

        QMetaObject.connectSlotsByName(Form)

    def retranslateUi(self, Form):
        _translate = QCoreApplication.translate
        Form.setWindowTitle(_translate("Form", "Form"))

class LongVLabels(QWidget):
    def __init__(self, top, ftpStr, ftpRanges):
        super().__init__()
        #Setup the UI main window
        self.ui = Ui_Form()
        self.ui.setupUi(self, top, ftpStr, ftpRanges)

