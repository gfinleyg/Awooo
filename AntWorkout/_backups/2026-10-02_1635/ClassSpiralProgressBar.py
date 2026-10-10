from PyQt6.QtWidgets import QWidget, QVBoxLayout
from PyQt6.QtCore import QMetaObject, QCoreApplication, QRect
from PyQt6.QtGui import QRegion, QIcon

from QSpiralProgressBar import *

import os


class Ui_Form(object):
    def setupUi(self, Form):
        Form.setObjectName("Form")
        Form.resize(400, 300)
        self.verticalLayout = QVBoxLayout(Form)
        self.verticalLayout.setObjectName("verticalLayout")
        self.widget = QSpiralProgressBar(parent=Form)
        self.widget.setObjectName("widget")
        self.verticalLayout.addWidget(self.widget)

        self.retranslateUi(Form)
        QMetaObject.connectSlotsByName(Form)

    def retranslateUi(self, Form):
        _translate = QCoreApplication.translate
        Form.setWindowTitle(_translate("Form", "Form"))
from QSpiralProgressBar import QSpiralProgressBar

class SpiralProgressBar(QWidget):
    def __init__(self, parent=None):
        super().__init__()
        #Setup the UI main window
        self.ui = Ui_Form()
        self.ui.setupUi(self)

        #self.ui.widget.spb_setMinimumSize()
        # self.ui.widget.spb_setNoProgressBar(2) #number of progress bars
        # self.ui.widget.spb_setValue((15, 25, 50)) #this means value of outermost to  innermost.        
        # self.ui.widget.spb_setGap(20) #gap between paths
        # self.ui.widget.spb_setInitialPos(('North','South','East')) #sets initial starting position
        # self.ui.widget.spb_reset() #initialize the widget values
        # self.ui.widget.spb_setDirection(('Clockwise','AntiClockwise','Clockwise')) #rotation direction
        # self.ui.widget.spb_lineColor(((255,0,0), (0,255,0), (0,0,255))) #progress line color, outer to inner different; inner colors for different workout types, difficulties?
        # self.ui.widget.spb_lineStyle(('SolidLine', 'DotLine', 'DashLine')) #dash not very useful, solid/dotted for different types, difficulties?
        # self.ui.widget.spb_lineCap(('SquareCap', 'RoundCap', 'SquareCap')), stick with round (default)
        # self.ui.widget.spb_setPathHidden(True) #Just shows the progress bar if true
        # self.ui.widget.spb_pathColor(((0,0,0), (0,0,0), (0,0,0))) #path color

        


        
