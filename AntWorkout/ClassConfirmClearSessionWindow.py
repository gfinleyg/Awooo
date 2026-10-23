from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QPushButton, QVBoxLayout, QWidget

import AppLayout
import KeyShortcuts

from ClassCustomTitleBar import *


###Confirm Clear Session QWidget Window: the setup screen's Clear Session (see Session.py), which can't be undone
class ConfirmClearSessionWindow(QWidget):
    def __init__(self, parent):
        super(ConfirmClearSessionWindow, self).__init__()

        self.parent = parent

        self.setWindowTitle("   Clear the session's rides?   ")
        self.clearSessionConfirmButton = QPushButton("Yes, Clear Session")
        self.clearSessionCancelButton = QPushButton("No")

        self.setStyleSheet("""
                            ConfirmClearSessionWindow {background-color:#9fc5d1;}
                          """)

        self.clearSessionConfirmButton.clicked.connect(self.clear_session)
        self.clearSessionCancelButton.clicked.connect(self.hide)

        clearLayout = QVBoxLayout()
        #Frameless window
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        #Titlebar
        self.title_bar = CustomTitleBar(self, False)
        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        self.title_bar.setFixedHeight(36)
        clearLayout.addWidget(self.title_bar)
        clearLayout.addWidget(self.clearSessionConfirmButton)
        clearLayout.addWidget(self.clearSessionCancelButton)
        self.setLayout(clearLayout)

        #keys (Ctrl shows them)
        KeyShortcuts.add(self.clearSessionConfirmButton, Qt.Key.Key_Y, label="Y")
        KeyShortcuts.add(self.clearSessionCancelButton, Qt.Key.Key_N, label="N")

        #scaled for this screen, its text fitted to it (see AppLayout.py)
        AppLayout.fit_dialog(self)

    def clear_session(self):
        self.hide()
        self.parent.clear_session()
