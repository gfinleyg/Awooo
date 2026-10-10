from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QDialog, QGridLayout, QLabel, QPushButton, QSlider, QVBoxLayout

from ClassCustomTitleBar import CustomTitleBar
import Performance


###The ⚙ menu's Performance: how smoothly the gauge needles move, against how much CPU they use.
###Takes effect (and is saved) as the slider moves, gauges on screen included, see Performance.py
class PerformanceWindow(QDialog):
    def __init__(self, parent=None):
        super(PerformanceWindow, self).__init__(parent)
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Dialog)
        self.setWindowTitle("   Performance   ")
        self.setStyleSheet("""
            PerformanceWindow {background-color:#9fc5d1;}
            QLabel {font: 900 14pt 'Orbitron';}
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
        grid.addWidget(QLabel("Gauge needles:"), 0, 0, 1, 3, alignment=Qt.AlignmentFlag.AlignCenter)
        #one stop per choice, lightest on the left
        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setRange(0, len(Performance.GAUGE_FPS_CHOICES) - 1)
        self.slider.setPageStep(1)
        self.slider.setTickPosition(QSlider.TickPosition.TicksBelow)
        self.slider.setTickInterval(1)
        self.slider.setMinimumWidth(320)
        self.slider.setValue(Performance.GAUGE_FPS_CHOICES.index(Performance.gauge_fps()))
        ends = []
        for text in ("Lighter", "Smoother"):
            end = QLabel(text)
            end.setStyleSheet("font: 900 10pt 'Orbitron';")
            ends.append(end)
        grid.addWidget(ends[0], 1, 0)
        grid.addWidget(self.slider, 1, 1)
        grid.addWidget(ends[1], 1, 2)
        self.valueLabel = QLabel()
        self.valueLabel.setStyleSheet("font: 900 12pt 'Orbitron';")
        grid.addWidget(self.valueLabel, 2, 0, 1, 3, alignment=Qt.AlignmentFlag.AlignCenter)
        layout.addLayout(grid)
        self.slider.valueChanged.connect(self.slider_moved)
        self.show_value()

        note = QLabel("How smoothly the gauge needles glide between readings.\n"
                      "Lower it if the workout screen is sluggish on this computer.")
        note.setStyleSheet("font: 900 10pt 'Orbitron';")
        note.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(note)
        doneButton = QPushButton("Done")
        doneButton.clicked.connect(self.accept)
        layout.addWidget(doneButton, alignment=Qt.AlignmentFlag.AlignCenter)

    def slider_moved(self, index):
        Performance.set_gauge_fps(Performance.GAUGE_FPS_CHOICES[index])
        self.show_value()

    def show_value(self):
        self.valueLabel.setText(Performance.gauge_fps_text(Performance.GAUGE_FPS_CHOICES[self.slider.value()]))
