import os

from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtWidgets import QButtonGroup, QPushButton, QVBoxLayout, QWidget

import AppLayout

from ClassAnalogGauge import *
from ClassLongVerticalLabelSeries import *
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *

import ClassAntLogger
from WorkoutHelpers import ReadSelectedDeviceLines, WriteSelectedDeviceLines


        


###Select Devices QWidget Window
class SelectDevicesWindow(QWidget):
    def __init__(self, parent, devList, devListType, foundDevices=()):
        super(SelectDevicesWindow, self).__init__()

        self.devList = []
        for devItem in devList:
            self.devList.append(devItem.strip("\n"))

        #Frameless window
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        #Titlebar
        self.title_bar = CustomTitleBar(self, False)
        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        
        selectionLayout = QVBoxLayout()
        selectionLayout.addWidget(self.title_bar)

        self.setStyleSheet("""
                            SelectDevicesWindow {background-color:#9fc5d1;}
                          """)

        self.setWindowTitle("   Device Selection   ")
        ### Buttons to match target device count, depress but brighten when appropriate, unavailable will be disabled
        self.buttonList = []
        prevSelectedButtonList = []
        self.buttonGroup = QButtonGroup()
        self.buttonGroupSpd = QButtonGroup()
        self.buttonGroupCad = QButtonGroup()
        self.buttonGroup.buttonClicked.connect(self.button_clicked)
        self.buttonGroupSpd.buttonClicked.connect(self.button_clicked)
        self.buttonGroupCad.buttonClicked.connect(self.button_clicked)
        #Get the list of previously selected items
        selectedDevListNoNewLn = ReadSelectedDeviceLines()
        selectedButtonIndex = 0
        if selectedDevListNoNewLn:
            for devLine in selectedDevListNoNewLn:
                #check if non-scanned selected items need to be added to the device list
                if devListType == "power":
                    if devLine.split()[0] in ["11","17"]:
                        self.devList.append(devLine)
                elif devListType == "hrm":
                    if devLine.split()[0] == "120":
                        self.devList.append(devLine)
                else:
                    if devLine.split()[0] in ["121","122","123"]:
                        self.devList.append(devLine)
        #Get the list of scanned devices
        deviceLinesNoNewLn = list(foundDevices)
        #Combine the scanned devices and previously selected devices
        for selectedDev in selectedDevListNoNewLn:
            if selectedDev not in deviceLinesNoNewLn:
                deviceLinesNoNewLn.append(selectedDev)
        for deviceLine in deviceLinesNoNewLn:
            if deviceLine in self.devList:
                button = QPushButton()
                button.isPressed = False
                typeId = deviceLine.split()
                match typeId[0]:
                    case "120": # ANT+ HRM
                        button.setText("Heart Rate Monitor #: " + typeId[1])
                        if deviceLine in selectedDevListNoNewLn:
                            prevSelectedButtonList.append(selectedButtonIndex)
                        self.buttonList.append(button)
                        self.buttonGroup.addButton(self.buttonList[-1], int(typeId[0]))
                    case "11": # ANT+ Power Meter
                        button.setText("Power Meter #: " + typeId[1])
                        if deviceLine in selectedDevListNoNewLn:
                            prevSelectedButtonList.append(selectedButtonIndex)
                        self.buttonList.append(button)
                        self.buttonGroup.addButton(self.buttonList[-1], int(typeId[0]))
                    case "17": # ANT+ Fitness Equipment
                        button.setText("Stationary Bike #: " + typeId[1])
                        if deviceLine in selectedDevListNoNewLn:
                            prevSelectedButtonList.append(selectedButtonIndex)
                        self.buttonList.append(button)
                        self.buttonGroup.addButton(self.buttonList[-1], int(typeId[0]))
                    case "121": # ANT+ Speed/Cadence
                        button.setText("Speed/Cadence Meter #: " + typeId[1])
                        if deviceLine in selectedDevListNoNewLn:
                            prevSelectedButtonList.append(selectedButtonIndex)
                        self.buttonList.append(button)
                        self.buttonGroup.addButton(self.buttonList[-1], int(typeId[0]))
                    case "122": # ANT+ Cadence
                        button.setText("Cadence Meter #: " + typeId[1])
                        if deviceLine in selectedDevListNoNewLn:
                            prevSelectedButtonList.append(selectedButtonIndex)
                        self.buttonList.append(button)
                        self.buttonGroupCad.addButton(self.buttonList[-1], int(typeId[0]))
                    case "123": # ANT+ Speed"
                        button.setText("Speed Meter #: " + typeId[1])
                        if deviceLine in selectedDevListNoNewLn:
                            prevSelectedButtonList.append(selectedButtonIndex)
                        self.buttonList.append(button)
                        self.buttonGroupSpd.addButton(self.buttonList[-1], int(typeId[0]))
                selectedButtonIndex += 1

        #Connect found device buttons and add them to the layout
        for b in self.buttonList:
            selectionLayout.addWidget(b)   
        self.selectDevicesButton = QPushButton("Keep Current Selections")
        self.selectDevicesButton.clicked.connect(self.exit_selection)
        selectionLayout.addWidget(self.selectDevicesButton)
        self.setLayout(selectionLayout)

        #scaled for this screen, its text fitted to it (see AppLayout.py)
        AppLayout.fit_dialog(self)

        # Don't change the selted devices files on initialization of this window
        self.windowInitDoneBool = False

        #Press the previously selected button(s)
        for buttonIndex in prevSelectedButtonList:
            self.buttonList[buttonIndex].click()

        self.windowInitDoneBool = True

        #Blink every candidate button whenever this slot is genuinely
        #ambiguous - more than one device was found for it, so the
        #person actually has to pick one (a single-candidate slot never
        #gets this window in the first place any more, since
        #WorkoutSetupWindow.auto_select_and_connect() auto-selects it).
        #Mirrors the same blink-on-ambiguity behavior on the setup page
        #this window was opened from.
        self._blinkTgl = False
        self.blinkTimer = QTimer()
        self.blinkTimer.timeout.connect(self._blink_candidates)
        self.blinkTimer.setInterval(500)
        if len(self.buttonList) > 1:
            self.blinkTimer.start()

    def _blink_candidates(self):
        self._blinkTgl = not self._blinkTgl
        for button in self.buttonList:
            if button.isEnabled() and not button.isPressed and self._blinkTgl:
                button.setStyleSheet("background-color: #FFEB3B;")
            else:
                button.setStyleSheet("")

    def button_clicked(self, toggledButton):
        #Toggle the button state
        # skip this step when initializing the buttons
        # if self.windowInitDoneBool:
        toggledButton.isPressed = not toggledButton.isPressed
        #booleans to track if a speed or cadence button was/is pressed
        spdButton = False
        cadButton = False
        cadPressed = False
        spdPressed = False
        #check if target is a speed or cadence button and get the device type id string
        devTypeId = ""
        if toggledButton in self.buttonGroupSpd.buttons():
            spdButton = True
            devTypeId = str(self.buttonGroupSpd.id(toggledButton)) + " " + toggledButton.text().split()[-1]
        elif toggledButton in self.buttonGroupCad.buttons():
            cadButton = True
            devTypeId = str(self.buttonGroupCad.id(toggledButton)) + " " + toggledButton.text().split()[-1]
        else:
            devTypeId = str(self.buttonGroup.id(toggledButton)) + " " + toggledButton.text().split()[-1]
        
        # set enabled and down states for each button group
        for button in self.buttonGroupSpd.buttons():
            #set the down state
            button.setDown(button.isPressed)
            # check if any speed buttons are pressed
            if button.isPressed == True:
                spdPressed = True
            if spdButton: #toggled button was a speed button
                if toggledButton.isPressed: #toggled on
                    if toggledButton != button: #disable other speed buttons
                        button.setEnabled(False)
                else: #toggled off
                    button.setEnabled(True)
            elif not cadButton: #toggled button was speed/cadence button
                if toggledButton.isPressed: #toggled on, disable speed buttons
                    button.setEnabled(False)
                else: #toggled off, enable speed buttons
                    button.setEnabled(True)

        for button in self.buttonGroupCad.buttons():
            #set the down state
            button.setDown(button.isPressed)
            # check if any cadence buttons are pressed
            if button.isPressed == True:
                cadPressed = True
            if cadButton: #toggled button was a cadence button
                if toggledButton.isPressed: #toggled on
                    if toggledButton != button: #disable other speed buttons
                        button.setEnabled(False)
                    
                else: #toggled off
                    button.setEnabled(True)
            elif not spdButton: #toggled button was speed/cadence button
                if toggledButton.isPressed: #toggled on, disable speed buttons
                    button.setEnabled(False)
                else: #toggled off, enable speed buttons
                    button.setEnabled(True)

        for button in self.buttonGroup.buttons():
            #set the down state
            button.setDown(button.isPressed)
            if spdButton or cadButton: #toggled button was a speed or cadence button
                if spdPressed or cadPressed: #speed or cadence buttons are pressed, disable
                    button.setEnabled(False)
                else: #no speed or cadence buttons are pressed, enable
                    button.setEnabled(True)
            else: # speed/cadence, power or hrm button pressed
                if toggledButton.isPressed: #toggled on
                    if button == toggledButton:
                        button.setEnabled(True)
                    else:
                        button.setEnabled(False)
                else: #toggled off, enable all
                    button.setEnabled(True)

        #skip this step when initializing the buttons
        if self.windowInitDoneBool:
            if toggledButton.isPressed:
                self.add_selected_device(devTypeId)
            else:
                self.remove_selected_device(devTypeId)


    def add_selected_device(self, devTypeId):
        ClassAntLogger.log_event("SETUP", f"User manually selected device: {devTypeId}")
        WriteSelectedDeviceLines(ReadSelectedDeviceLines() + [devTypeId])

    def remove_selected_device(self, devTypeId):
        ClassAntLogger.log_event("SETUP", f"User manually deselected device: {devTypeId}")
        WriteSelectedDeviceLines([line for line in ReadSelectedDeviceLines() if line != devTypeId.strip()])

    def exit_selection(self):
        self.stop_blinking()
        self.hide()

    def closeEvent(self, event):
        #the title bar's X and Esc
        self.stop_blinking()
        super().closeEvent(event)

    def stop_blinking(self):
        if hasattr(self, "blinkTimer"):
            self.blinkTimer.stop()
        for button in self.buttonList:
            button.setStyleSheet("")