from PyQt6.QtCore import QTime
from PyQt6.QtGui import QColor
import pyqtgraph as pg
import math

import Units

def hourMinuteFromSecond(seconds):
    if seconds > 3590:
        return QTime(0,0).addSecs(math.floor(seconds)).toString("h'h 'm'm'")
        
    else:
        return QTime(0,0).addSecs(math.floor(seconds)).toString("m'm")

def hourMinuteSecondFromSecond(seconds):
    if seconds > 3590:
        return QTime(0,0).addSecs(math.floor(seconds)).toString("h'h 'm'm 'ss's'")
    else:
        return QTime(0,0).addSecs(math.floor(seconds)).toString("m'm 'ss's'")

class TimeAxisItem(pg.AxisItem):
    """Internal timestamp for x-axis"""
    def __init__(self, *args, **kwargs):
        super(TimeAxisItem, self).__init__(*args, **kwargs)

    def tickStrings(self, values, scale, spacing):
        """Overload default version for timestamp"""
        if spacing < 60:
            #ticks closer than a minute apart (zoomed-in synopsis) need the seconds too
            return [hourMinuteSecondFromSecond(math.floor(value)) for value in values]
        return [hourMinuteFromSecond(math.floor(value)) for value in values]

class WorkoutGraph(pg.PlotWidget):
    def __init__(self):
        super(WorkoutGraph, self).__init__()

        # Expected Power, power, hr, cadence vs time plot

        styles = {"color": "#000000", "font-size": "18px"}
        #axis titles' point size before theme scaling, if not the title's (see AppTheme.theme_graph)
        self.axisTitlePoints = None

        ## Set up the power axis
        self.plotScreen = self.plotItem
        self.setBackground("#bdd6de")
        self.setXRange(0,20)
        self.plotScreen.setTitle("Workout Stats", color="#000000", size="20pt")
        self.plotScreen.setLabel("left", "Power (Watts)", **styles)
        self.plotScreen.getAxis('left').setTickSpacing(100, 20)
        self.plotScreen.getAxis('left').setTickPen(QColor("#1CB5E0"))
        self.axisStyles = styles
        self.set_speed_label()
        self.plotScreen.getAxis('right').setTickSpacing(50, 50)
        self.plotScreen.getAxis('right').setTickPen(QColor("#dadbde"))
        self.plotScreen.setAxisItems(axisItems={'bottom': TimeAxisItem(orientation='bottom')})
        self.plotScreen.getAxis('bottom').setTickSpacing(60, 60)
        self.plotScreen.setLabel("bottom", "Time (minutes)", **styles)

        self.plotScreen.addLegend()
        self.plotScreen.showGrid(x=True, y=True)

        ## create new ViewBoxes for workout power, power produced, HR, and cadence
        self.powerVBox = pg.ViewBox()
        self.plotScreen.scene().addItem(self.powerVBox)
        self.plotScreen.getAxis('left').linkToView(self.powerVBox)
        self.powerVBox.setXLink(self.plotScreen)
        self.powerVBox.setYRange(0,400)
        
        self.hrCadenceVBox = pg.ViewBox()
        self.plotScreen.scene().addItem(self.hrCadenceVBox)
        self.plotScreen.getAxis('right').linkToView(self.hrCadenceVBox)
        self.hrCadenceVBox.setXLink(self.plotScreen)
        self.hrCadenceVBox.setYRange(0,220)


    #the right axis' label, with speed in the distance unit (see Units.py)
    def set_speed_label(self):
        self.plotScreen.setLabel("right", f"Heart Rate(BPM) / Cadence(RPM) / Speed({Units.speed_unit()})",
                                 **self.axisStyles)

    ## Handle view resizing 
    def updateViews(self):
        ## view has resized; update auxiliary views to match
        self.powerVBox.setGeometry(self.plotScreen.vb.sceneBoundingRect())
        self.hrCadenceVBox.setGeometry(self.plotScreen.vb.sceneBoundingRect())
        
        ## need to re-update linked axes since this was called
        ## incorrectly while views had different shapes.
        ## (probably this should be handled in ViewBox.resizeEvent)
        self.powerVBox.linkedViewChanged(self.plotScreen.vb, self.powerVBox.XAxis)
        self.hrCadenceVBox.linkedViewChanged(self.plotScreen.vb, self.hrCadenceVBox.XAxis)
