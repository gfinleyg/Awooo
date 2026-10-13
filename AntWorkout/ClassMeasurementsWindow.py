from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QButtonGroup, QDialog, QGridLayout, QLabel, QPushButton, QRadioButton, QVBoxLayout

import AppLayout
import KeyShortcuts

from ClassCustomTitleBar import CustomTitleBar
import Units


###The ⚙ menu's Measurements: weight in lbs or kg, power to weight in W/kg or W/lb, distance in miles or km.
###Each choice takes effect (and is saved) as it's clicked, see Units.py
class MeasurementsWindow(QDialog):
    ROWS = (("Weight", "weight", Units.WEIGHT_UNITS, {"lbs": "lbs", "kg": "kg"}),
            ("Power to weight", "powerWeight", Units.POWER_WEIGHT_UNITS, {"W/kg": "W/kg", "W/lb": "W/lb"}),
            ("Distance", "distance", Units.DISTANCE_UNITS, {"mi": "Miles (MPH)", "km": "Km (KM/H)"}))

    #the key that steps through each row's choices
    KEYS = {"weight": Qt.Key.Key_W, "powerWeight": Qt.Key.Key_P, "distance": Qt.Key.Key_D}

    def __init__(self, parent=None):
        super(MeasurementsWindow, self).__init__(parent)
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Dialog)
        self.setWindowTitle("   Measurements   ")
        self.setStyleSheet("""
            MeasurementsWindow {background-color:#9fc5d1;}
            QLabel {font: 900 14pt 'Orbitron';}
            QRadioButton {font: 900 13pt 'Orbitron'; padding: 4px 10px;}
            QPushButton {font: 12pt 'Segoe UI Black'; padding: 6px 18px; border-radius: 12px; border: 2px outset black;
                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #dadbde);}
            QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #1f15e1);}
            """)
        layout = QVBoxLayout(self)
        self.title_bar = CustomTitleBar(self, False)
        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        self.title_bar.setFixedHeight(36)
        layout.addWidget(self.title_bar)

        grid = QGridLayout()
        grid.setHorizontalSpacing(12)
        self.groups = {}
        for row, (label, key, units, names) in enumerate(self.ROWS):
            rowLabel = QLabel(label + ":")
            grid.addWidget(rowLabel, row, 0, alignment=Qt.AlignmentFlag.AlignRight)
            group = QButtonGroup(self)
            for column, unit in enumerate(units, 1):
                radio = QRadioButton(names[unit])
                radio.unit = unit
                radio.setChecked(getattr(Units.current, key) == unit)
                group.addButton(radio)
                grid.addWidget(radio, row, column)
            group.buttonClicked.connect(lambda radio, k=key: Units.set_units(**{k: radio.unit}))
            self.groups[key] = group
            #a key steps through the row's choices
            letter = self.KEYS[key]
            KeyShortcuts.add(rowLabel, letter, label=chr(letter), action=lambda g=group: self.next_choice(g))
        layout.addLayout(grid)

        note = QLabel("Your weight is set on the setup screen (0 leaves it out).\n"
                      "With a weight, the synopsis shows power to weight too.")
        note.setStyleSheet("font: 900 10pt 'Orbitron';")
        note.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(note)
        doneButton = QPushButton("Done")
        doneButton.clicked.connect(self.accept)
        layout.addWidget(doneButton, alignment=Qt.AlignmentFlag.AlignCenter)
        KeyShortcuts.add(doneButton, KeyShortcuts.ENTER, label="Enter")

        #scaled for this screen, its text fitted to it (see AppLayout.py)
        AppLayout.fit_dialog(self)

    @staticmethod
    def next_choice(group):
        #the row's next choice, wrapping (a click, so it applies as a mouse click does)
        radios = group.buttons()
        checked = group.checkedButton()
        radios[(radios.index(checked) + 1) % len(radios) if checked in radios else 0].click()
