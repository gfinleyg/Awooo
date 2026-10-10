from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QLabel, QPushButton, QVBoxLayout, QWidget

from ClassAnalogGauge import *
from ClassLongVerticalLabel import *
from ClassLongVerticalLabelSeries import *
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *

from ClassAntDevices import read_selected_devices
from WorkoutHelpers import ReadSelectedDeviceLines



###Connect to Devices QWidget Window
class ConnectDevicesWindow(QWidget):
    def __init__(self, bridge=None):
        super(ConnectDevicesWindow, self).__init__()

        #AntWorkoutBridge instance - request_connect() is called on this
        #directly from connect_devices() below.
        self.bridge = bridge

        #Frameless window
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)

        #Titlebar
        self.title_bar = CustomTitleBar(self, False)
        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        
        #Window Color
        self.setStyleSheet("""
                            ConnectDevicesWindow {background-color:#9fc5d1;}
                            QLabel{font: 900 28pt 'Orbitron'; qproperty-indent:3; border-radius: 15px; background-color: transparent;}
                          """)
        connectLayout = QVBoxLayout()
        connectLayout.addWidget(self.title_bar)
        self.connectLabel = QLabel()
        self.connectLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        connectLayout.addWidget(self.connectLabel)

        selectedDevList = []
        scannedDevList = []
        connectingList = []
        otherList = []
        notScannedMessageBool = False
        pwrSelected = False
        hrmSelected = False
        spdCadSelected = False
        spdSelected = False
        cadSelected = False
        otherUsableDevices = False
        connectingMessage = ""
        selectedDevList = ReadSelectedDeviceLines()
        scannedDevList.extend(self.bridge.found_devices() if self.bridge is not None else [])
        if len(selectedDevList) == 0 or len(scannedDevList) == 0: #shouldn't be possible
            self.exit_selection()
        #check which selected devices were scanned
        for selDev in selectedDevList:
            #find device type
            typeId = selDev.split()
            devString = ""
            if typeId[0] == "11":
                devString = "Power Meter: " + typeId[1]
                pwrSelected = True
            elif typeId[0] == "17":
                devString = "Stationary Trainer: " + typeId[1]
                pwrSelected = True
            elif typeId[0] == "120":
                devString = "Heart Rate Monitor: " + typeId[1]
                hrmSelected = True
            elif typeId[0] == "121":
                devString = "Speed/Cadence Meter: " + typeId[1]
                spdCadSelected = True
            elif typeId[0] == "122":
                devString = "Cadence Meter: " + typeId[1]
                cadSelected = True
            elif typeId[0] == "123":
                devString = "Speed Meter: " + typeId[1]
                spdSelected = True
            #check if the selected device was found
            if selDev in scannedDevList:
                connectingList.append(devString + " - Connecting")
            else:
                connectingList.append(selDev + " - Not Found")
                notScannedMessageBool = True
        #now check for any extra devices that can be connected but won't
        for scnDev in scannedDevList:
            if scnDev not in selectedDevList:
                #find device type
                typeId = scnDev.split()
                if typeId[0] == "11" and not pwrSelected:
                    otherList.append("Power Meter: " + typeId[1])
                elif typeId[0] == "17" and not pwrSelected:
                    otherList.append("Stationary Trainer: " + typeId[1])
                elif typeId[0] == "120" and not hrmSelected:
                    otherList.append("Heart Rate Monitor: " + typeId[1])
                elif typeId[0] == "121" and not (spdCadSelected or cadSelected or spdSelected):
                    otherList.append("Speed/Cadence Meter: " + typeId[1])
                elif typeId[0] == "122" and not (spdCadSelected or cadSelected):
                    otherList.append("Cadence Meter: " + typeId[1])
                elif typeId[0] == "123" and not (spdCadSelected or spdSelected):
                    otherList.append("Speed Meter: " + typeId[1])
        connectingMessage = "\n"
        if notScannedMessageBool:
            connectingMessage += "Not all selected devices have been detected yet:\n"
        if len(connectingList) > 1:
            connectingMessage += "\n".join(connectingList)
        elif len(connectingList) == 1:
            connectingMessage += connectingList[0]
        if len(otherList) > 1:
            otherUsableDevices = True
            connectingMessage += "\n\nOther Unselected Usable Devices Scanned:\n"
            connectingMessage += "\n".join(otherList)
        if len(otherList) == 1:
            otherUsableDevices = True
            connectingMessage += "\n\nOne Unselected Usable Device Scanned:\n" + otherList[0]
        if otherUsableDevices or notScannedMessageBool:
            connectingMessage += "\n\nAvailable devices unselected, are you sure you want to connect?"
        connectingMessage += "\n"
        self.connectLabel.setText(connectingMessage)

        self.connectForSureButton = QPushButton("Connect to Selected Devices")
        self.connectForSureButton.clicked.connect(self.connect_devices)
        connectLayout.addWidget(self.connectForSureButton)
        self.goBackToReselectButton = QPushButton("Reselect Devices")
        self.goBackToReselectButton.clicked.connect(self.exit_selection)
        connectLayout.addWidget(self.goBackToReselectButton)
        self.setLayout(connectLayout)
        
        
    def connect_devices(self):
        if self.bridge is not None:
            self.bridge.request_connect(read_selected_devices())
        self.hide()

    def exit_selection(self):
        self.hide()