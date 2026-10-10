from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtWidgets import QApplication, QHBoxLayout, QPlainTextEdit, QPushButton, QVBoxLayout, QWidget

from ClassAnalogGauge import *
from ClassLongVerticalLabel import *
from ClassLongVerticalLabelSeries import *
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *

import ClassAntLogger


###Show Logs QWidget Window - the unified antWorkoutLog.txt viewer opened
###from the gear menu on the setup/workout/synopsis pages' shared title bar.
class ShowLogsWindow(QWidget):
    def __init__(self):
        super(ShowLogsWindow, self).__init__()

        self.setWindowTitle("   " + ClassAntLogger.LOG_FILE + "   ")

        self.setStyleSheet("""
                            ShowLogsWindow {background-color:#9fc5d1;}
                          """)

        self.setGeometry(500, 80, 1100, 900)

        #Frameless window
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)

        #main window layout
        logsWindowLayout = QVBoxLayout()

        #Titlebar
        title_bar = CustomTitleBar(self, False)
        title_bar.setStyleSheet("background-color: #9fc5d1")

        #Log text view - QPlainTextEdit rather than a QLabel so the text
        #is natively selectable/copyable (click-drag, Ctrl+A, Ctrl+C all
        #work out of the box) and scrolls on its own without a separate
        #QScrollArea.
        self.logTextEdit = QPlainTextEdit()
        self.logTextEdit.setReadOnly(True)
        self.logTextEdit.setStyleSheet("""
                                        QPlainTextEdit {
                                            font: 10pt 'Consolas';
                                            background-color: white;
                                            border: 2px solid #bdd6de;
                                            border-radius: 8px;
                                        }
                                        """)
        self.logTextEdit.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.read_log()

        #button row: copy all, clear (rewrites the log via ClassAntLogger,
        #same as the gear menu's "Clear Logs" action) and exit
        buttonRow = QHBoxLayout()
        copyLogsButton = QPushButton("Copy All")
        copyLogsButton.clicked.connect(self.copy_all)
        buttonRow.addWidget(copyLogsButton)
        clearLogsButton = QPushButton("Clear Logs")
        clearLogsButton.clicked.connect(self.clear_logs)
        buttonRow.addWidget(clearLogsButton)
        exitLogsButton = QPushButton("Exit")
        exitLogsButton.clicked.connect(self.hide_logs_window)
        buttonRow.addWidget(exitLogsButton)

        #add the widgets to the layout and the layout to the window
        logsWindowLayout.addWidget(title_bar)
        logsWindowLayout.addWidget(self.logTextEdit)
        logsWindowLayout.addLayout(buttonRow)
        self.setLayout(logsWindowLayout)

        #Refreshes while open so a log viewed during an active workout stays current
        self.timer = QTimer()
        self.timer.timeout.connect(self.read_log)
        self.timer.setInterval(1000)
        self.timer.start()

    def read_log(self):
        #setPlainText() unconditionally clears any active text selection
        #and resets the cursor, which was wiping out a selection made for
        #copying mid-refresh. Two guards against that: skip the refresh
        #entirely while text is actively selected (so a copy in progress
        #is never disturbed, even if new log lines arrive during it), and
        #skip it when the content hasn't actually changed (the common
        #case on a 1s timer - most ticks have nothing new to show, so
        #there's no reason to touch the widget at all).
        if self.logTextEdit.textCursor().hasSelection():
            return

        newText = ClassAntLogger.read_log()
        if newText == self.logTextEdit.toPlainText():
            return

        #Capped to the most recent lines (see ClassAntLogger.read_log) so
        #this stays responsive regardless of how close the log file is to
        #its own 1MB rotation size. setPlainText resets the scrollbar, so
        #this sticks to the bottom across refreshes only when the person
        #was already reading the latest line - if they've scrolled up to
        #read history, a live refresh won't yank the view out from under
        #them.
        scrollbar = self.logTextEdit.verticalScrollBar()
        wasAtBottom = scrollbar.value() >= scrollbar.maximum() - 4

        self.logTextEdit.setPlainText(newText)

        if wasAtBottom:
            scrollbar.setValue(scrollbar.maximum())

    def copy_all(self):
        QApplication.clipboard().setText(self.logTextEdit.toPlainText())

    def clear_logs(self):
        ClassAntLogger.clear_log()
        self.read_log()

    def hide_logs_window(self):
        self.timer.stop()
        self.hide()