import time

from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtWidgets import (QGridLayout, QHBoxLayout, QLabel, QLineEdit, QPushButton,
                             QStackedWidget, QVBoxLayout, QWidget)

import AppLayout

from ClassCustomTitleBar import CustomTitleBar
from ClassAntDevices import ROLE_POWER
import ClassAntLogger
import PowerCalibration

DEVICE_TYPE_POWER_METER = 11
REST_MEASURE_SECONDS = 5
RIDE_MEASURE_SECONDS = 30


###Calibrate Power QWidget Window
###Walks through calibrating the connected power meter:
###  1. ANT+ manual zero offset - the meter's own calibration, sent over ANT+
###  2. At-rest check - the meter's reading with no load becomes an offset
###  3. Accuracy check (optional) - a steady ride compared against a reference
###     wattage gives a scale factor
###  4. Review and save - stored per meter in PowerCalibration.py, applied to
###     every power reading during workouts
class CalibratePowerWindow(QWidget):
    def __init__(self, bridge):
        super(CalibratePowerWindow, self).__init__()
        self.bridge = bridge
        self.setWindowTitle("   Calibrate Power   ")
        self.setStyleSheet("""
                            CalibratePowerWindow {background-color:#9fc5d1;}
                            QLabel {font: 900 14pt 'Orbitron';}
                          """)
        self.setGeometry(500, 120, 1000, 760)
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)

        device = self.bridge.selected_device(ROLE_POWER)
        self.deviceType = device.device_type if device is not None else None
        self.deviceNumber = device.device_number if device is not None else None
        self.saved = PowerCalibration.get(self.deviceType, self.deviceNumber) if device is not None else None

        #results gathered by the steps, applied on Save
        self.offsetWatts = 0.0
        self.scale = 1.0
        self.antZeroOffset = self.saved.get("ant_zero_offset") if self.saved else None
        #live sample collection
        self.samples = []
        self.measuring = None #"rest" / "ride" / None
        self.measureEnd = 0.0
        self.lastPages = -1

        layout = QVBoxLayout()
        titleBar = CustomTitleBar(self, False)
        titleBar.setStyleSheet("background-color: #9fc5d1")
        layout.addWidget(titleBar)

        self.liveLabel = QLabel()
        self.liveLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.liveLabel.setStyleSheet("background-color:#bdd6de; border-radius: 10px; padding: 6px;")
        layout.addWidget(self.liveLabel)

        self.pages = QStackedWidget()
        self.pages.addWidget(self.build_intro_page())
        self.pages.addWidget(self.build_zero_offset_page())
        self.pages.addWidget(self.build_rest_page())
        self.pages.addWidget(self.build_ride_page())
        self.pages.addWidget(self.build_review_page())
        layout.addWidget(self.pages, 1)

        nav = QHBoxLayout()
        self.cancelButton = QPushButton("Cancel")
        self.cancelButton.clicked.connect(self.close_window)
        self.backButton = QPushButton("Back")
        self.backButton.clicked.connect(lambda: self.go_to_page(self.pages.currentIndex() - 1))
        self.nextButton = QPushButton("Next")
        self.nextButton.clicked.connect(self.next_page)
        nav.addWidget(self.cancelButton)
        nav.addStretch()
        nav.addWidget(self.backButton)
        nav.addWidget(self.nextButton)
        layout.addLayout(nav)
        self.setLayout(layout)

        #scaled for this screen, its text fitted to it (see AppLayout.py)
        AppLayout.fit_dialog(self)
        #(each page's text fitted as it's shown too)
        for i in range(self.pages.count()):
            AppLayout.watch_screen(self.pages.widget(i))

        self.bridge.calibrationUpdate.connect(self.on_calibration_update)
        self.timer = QTimer()
        self.timer.timeout.connect(self.poll_power)
        self.timer.start(100)
        self.go_to_page(0)
        self.poll_power()

    #--- pages ---
    def page_with(self, text):
        page = QWidget()
        page.setLayout(QVBoxLayout())
        label = QLabel(text)
        label.setWordWrap(True)
        label.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
        page.layout().addWidget(label)
        return page, label

    def status_label(self, page):
        label = QLabel()
        label.setWordWrap(True)
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        page.layout().addWidget(label)
        return label

    def build_intro_page(self):
        if self.deviceNumber is None:
            text = "No power meter is connected.\n\nConnect your power meter on the setup screen first, then open Calibrate Power again."
        else:
            text = (f"Calibrating power device {self.deviceNumber}.\n"
                    f"Current settings: {PowerCalibration.describe(self.saved)}\n\n"
                    "This takes about a minute:\n"
                    "   1. Zero offset - the power meter's own calibration\n"
                    "   2. At-rest check - measures what the meter reads with no load\n"
                    "   3. Accuracy check (optional) - compare a steady ride against a reference\n"
                    "   4. Review and save\n\n"
                    "Put the bike on the trainer and have the cranks free to turn.")
        page, _ = self.page_with(text)
        page.layout().addStretch()
        return page

    def build_zero_offset_page(self):
        if self.deviceType == DEVICE_TYPE_POWER_METER:
            text = ("Step 1: Zero offset\n\n"
                    "   - Spin the cranks backwards a few times to wake the power meter\n"
                    "   - Unclip and stop the cranks with the drive side crank pointing straight down\n"
                    "   - Take all weight off the pedals and don't touch the cranks\n\n"
                    "Then press Send Zero Offset. The meter usually answers within a few seconds.")
        else:
            text = ("Step 1: Zero offset\n\n"
                    "This power source is a smart trainer, which calibrates with its own spin down "
                    "from the trainer maker's app rather than a zero offset. Press Next to continue.")
        page, _ = self.page_with(text)
        self.sendZeroButton = QPushButton("Send Zero Offset")
        self.sendZeroButton.clicked.connect(self.send_zero_offset)
        self.sendZeroButton.setVisible(self.deviceType == DEVICE_TYPE_POWER_METER)
        page.layout().addWidget(self.sendZeroButton, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.zeroStatus = self.status_label(page)
        page.layout().addStretch()
        return page

    def build_rest_page(self):
        page, _ = self.page_with(
            "Step 2: At-rest check\n\n"
            "Keep your feet off the pedals and the cranks still. Press Measure and the reading "
            f"is averaged for {REST_MEASURE_SECONDS} seconds. Anything above 0 W becomes an offset "
            "subtracted from every reading. Most meters read 0 W at rest, which means no offset is needed.")
        self.measureRestButton = QPushButton("Measure")
        self.measureRestButton.clicked.connect(lambda: self.start_measure("rest"))
        page.layout().addWidget(self.measureRestButton, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.restStatus = self.status_label(page)
        page.layout().addStretch()
        return page

    def build_ride_page(self):
        page, _ = self.page_with(
            "Step 3: Accuracy check (optional)\n\n"
            "Only needed if you have a reference to compare against, such as a trainer display "
            "or a second power meter. Press Start, then ride at a steady effort for "
            f"{RIDE_MEASURE_SECONDS} seconds. Enter the average watts your reference showed for "
            "that same period and press Apply. Press Skip to leave accuracy unchanged.")
        row = QHBoxLayout()
        row.addWidget(QLabel("Reference average watts:"))
        self.referenceEdit = QLineEdit()
        self.referenceEdit.setMaximumWidth(160)
        row.addWidget(self.referenceEdit)
        row.addStretch()
        page.layout().addLayout(row)
        buttons = QHBoxLayout()
        self.measureRideButton = QPushButton("Start")
        self.measureRideButton.clicked.connect(lambda: self.start_measure("ride"))
        self.applyScaleButton = QPushButton("Apply")
        self.applyScaleButton.clicked.connect(self.apply_scale)
        self.applyScaleButton.setEnabled(False)
        self.skipRideButton = QPushButton("Skip")
        self.skipRideButton.clicked.connect(self.skip_ride)
        for b in (self.measureRideButton, self.applyScaleButton, self.skipRideButton):
            buttons.addWidget(b)
        page.layout().addLayout(buttons)
        self.rideStatus = self.status_label(page)
        self.rideMeasuredWatts = None
        page.layout().addStretch()
        return page

    def build_review_page(self):
        page, self.reviewLabel = self.page_with("")
        buttons = QHBoxLayout()
        self.clearButton = QPushButton("Remove Saved Calibration")
        self.clearButton.clicked.connect(self.clear_saved)
        buttons.addWidget(self.clearButton)
        page.layout().addLayout(buttons)
        self.reviewStatus = self.status_label(page)
        page.layout().addStretch()
        return page

    #--- navigation ---
    def go_to_page(self, index):
        index = max(0, min(index, self.pages.count() - 1))
        self.stop_measure()
        self.pages.setCurrentIndex(index)
        self.backButton.setEnabled(index > 0)
        last = index == self.pages.count() - 1
        self.nextButton.setText("Save" if last else ("Start" if index == 0 else "Next"))
        self.nextButton.setEnabled(self.deviceNumber is not None)
        if last:
            self.refresh_review()

    def next_page(self):
        if self.pages.currentIndex() == self.pages.count() - 1:
            self.save()
        else:
            self.go_to_page(self.pages.currentIndex() + 1)

    #--- step 1: ANT+ zero offset ---
    def send_zero_offset(self):
        self.sendZeroButton.setEnabled(False)
        self.zeroStatus.setText("Sending the zero offset request...")
        self.bridge.request_power_calibration(self.deviceNumber)

    def on_calibration_update(self, deviceNumber, result, offset, attempts):
        if deviceNumber != self.deviceNumber:
            return
        if result == "sending":
            self.zeroStatus.setText(f"Waiting for the power meter to answer... (request {attempts})")
            return
        self.sendZeroButton.setEnabled(True)
        if result == "success":
            self.antZeroOffset = offset
            self.zeroStatus.setText(f"Zero offset succeeded. The meter reported offset {offset}.\nPress Next.")
        elif result == "failed":
            self.zeroStatus.setText(f"The power meter reported that calibration failed (offset {offset}).\n"
                                    "Check the cranks are still and unweighted, then try again.")
        else:
            self.zeroStatus.setText("No answer from the power meter.\n"
                                    "Spin the cranks backwards to wake it and try again, or press Next to skip this step.")

    #--- live readings and measurements ---
    def poll_power(self):
        device = self.bridge.selected_device(ROLE_POWER)
        latest = device.profile.latest if device is not None and device.profile is not None else None
        if latest is None:
            self.liveLabel.setText("Power meter: waiting for readings...")
        else:
            raw = latest.instantaneous_power_watts
            calibrated = PowerCalibration.apply(raw, {"offset_watts": self.offsetWatts, "scale": self.scale})
            self.liveLabel.setText(f"Power meter now: {raw} W    with new calibration: {calibrated} W")
            pages = device.profile.readings_received #new power readings only, not every page
            if self.measuring and pages != self.lastPages:
                self.samples.append(raw)
            self.lastPages = pages

        if self.measuring:
            remaining = self.measureEnd - time.monotonic()
            status = self.restStatus if self.measuring == "rest" else self.rideStatus
            if remaining > 0:
                status.setText(f"Measuring... {int(remaining) + 1} s left ({len(self.samples)} readings)")
            else:
                self.finish_measure()

    def start_measure(self, kind):
        self.samples = []
        self.measuring = kind
        self.measureEnd = time.monotonic() + (REST_MEASURE_SECONDS if kind == "rest" else RIDE_MEASURE_SECONDS)
        self.measureRestButton.setEnabled(False)
        self.measureRideButton.setEnabled(False)
        self.nextButton.setEnabled(False)

    def stop_measure(self):
        self.measuring = None
        self.measureRestButton.setEnabled(True)
        self.measureRideButton.setEnabled(True)
        self.nextButton.setEnabled(self.deviceNumber is not None)

    def finish_measure(self):
        kind = self.measuring
        samples = self.samples
        self.stop_measure()
        if not samples:
            status = self.restStatus if kind == "rest" else self.rideStatus
            status.setText("No readings arrived from the power meter. Wake it up and try again.")
            return
        average = sum(samples) / len(samples)
        if kind == "rest":
            self.offsetWatts = round(average, 1)
            if self.offsetWatts == 0:
                self.restStatus.setText("Reads 0 W at rest, so no offset is needed. Press Next.")
            else:
                self.restStatus.setText(f"Reads {self.offsetWatts:g} W at rest; this will be subtracted from every reading. Press Next.")
        else:
            self.rideMeasuredWatts = sum(max(0.0, s - self.offsetWatts) for s in samples) / len(samples)
            self.applyScaleButton.setEnabled(True)
            self.rideStatus.setText(f"Power meter averaged {self.rideMeasuredWatts:.0f} W.\n"
                                    "Enter your reference's average for the same period and press Apply.")

    #--- step 3: accuracy ---
    def apply_scale(self):
        try:
            reference = float(self.referenceEdit.text())
        except ValueError:
            self.rideStatus.setText("Enter the reference average as a number of watts.")
            return
        if not self.rideMeasuredWatts or self.rideMeasuredWatts < 30:
            self.rideStatus.setText("The ride average was too low to compare. Ride at a steady effort of at least 50 W and try again.")
            return
        scale = reference / self.rideMeasuredWatts
        if not PowerCalibration.MIN_SCALE <= scale <= PowerCalibration.MAX_SCALE:
            self.rideStatus.setText(f"That would scale readings by x{scale:.2f}, which is too far off to be a calibration error.\n"
                                    "Check the reference number, or ride again at a steadier effort.")
            return
        self.scale = round(scale, 4)
        self.rideStatus.setText(f"Readings will be scaled by x{self.scale:.3f}. Press Next.")

    def skip_ride(self):
        self.scale = 1.0
        self.go_to_page(self.pages.currentIndex() + 1)

    #--- step 4: review ---
    def refresh_review(self):
        zero = "not run" if self.antZeroOffset is None else f"meter reported offset {self.antZeroOffset}"
        self.reviewLabel.setText(
            "Step 4: Review\n\n"
            f"   Zero offset: {zero}\n"
            f"   At-rest offset: {self.offsetWatts:g} W subtracted from each reading\n"
            f"   Accuracy scale: x{self.scale:.3f}\n\n"
            f"Previously saved: {PowerCalibration.describe(self.saved)}\n\n"
            "Press Save to use these settings for this power meter in every workout.")
        self.clearButton.setEnabled(self.saved is not None)

    def save(self):
        try:
            PowerCalibration.save(self.deviceType, self.deviceNumber, self.offsetWatts, self.scale, self.antZeroOffset)
        except OSError as e:
            self.reviewStatus.setText(f"Couldn't save the calibration: {e}")
            return
        ClassAntLogger.log_event("CALIBRATE", f"Saved power calibration for {self.deviceType} {self.deviceNumber}: "
                                              f"offset={self.offsetWatts} scale={self.scale} ant_zero_offset={self.antZeroOffset}")
        self.close_window()

    def clear_saved(self):
        try:
            PowerCalibration.clear(self.deviceType, self.deviceNumber)
        except OSError as e:
            self.reviewStatus.setText(f"Couldn't remove the calibration: {e}")
            return
        self.saved = None
        self.offsetWatts, self.scale = 0.0, 1.0
        self.refresh_review()
        self.reviewStatus.setText("Saved calibration removed; readings are used as the meter reports them.")

    def close_window(self):
        self.timer.stop()
        self.bridge.cancel_power_calibration()
        try:
            self.bridge.calibrationUpdate.disconnect(self.on_calibration_update)
        except (TypeError, RuntimeError):
            pass
        self.close()
