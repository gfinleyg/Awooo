from PyQt6.QtWidgets import QWidget, QGridLayout
from PyQt6.QtGui import QPolygon, QPolygonF, QColor, QPen, QFont, QPainter, QPixmap, QFontMetrics, QConicalGradient, QRadialGradient, QFontDatabase
from PyQt6.QtCore import QMetaObject, QCoreApplication, Qt, QTimer, QPoint, QPointF, QRect, QRectF, QSize, QByteArray, pyqtSignal
from PyQt6.QtSvg import QSvgRenderer
from PyQt6.QtSvgWidgets import QSvgWidget

#!/usr/bin/env python

###
# Author: Stefan Holstein
# inspired by: https://github.com/Werkov/PyQt4/blob/master/examples/widgets/analogclock.py
# Thanks to https://stackoverflow.com/

# Updated by 
########################################################################
## SPINN DESIGN CODE
# And GSF
########################################################################


import os
import math
import time

import Performance



################################################################################################
# AnalogGaugeWidget CLASS
################################################################################################
#time constant for the needle easing toward a new reading
NEEDLE_EASE_SECONDS = 0.11


class AnalogGaugeWidget(QWidget):
    """Fetches rows from a Bigtable.
    Args: 
        none
    
    """
    valueChanged = pyqtSignal(int)

    def __init__(self):
        super(AnalogGaugeWidget, self).__init__()

        ################################################################################################
        # DEFAULT TIMER VALUE
        ################################################################################################
        self.use_timer_event = False

        ################################################################################################
        # DEFAULT NEEDLE COLOR
        ################################################################################################
        self.setNeedleColor(0, 0, 0, 255)

        ################################################################################################
        # DEFAULT NEEDLE WHEN RELEASED
        ################################################################################################
        self.NeedleColorReleased = self.NeedleColor

        ################################################################################################
        # DEFAULT NEEDLE COLOR ON DRAG
        ################################################################################################
        # self.NeedleColorDrag = QColor(255, 0, 00, 255)
        self.setNeedleColorOnDrag(255, 0, 00, 255)

        ################################################################################################
        # DEFAULT SCALE TEXT COLOR
        ################################################################################################
        self.setScaleValueColor(0, 0, 0, 255)

        ################################################################################################
        # DEFAULT VALUE COLOR
        ################################################################################################
        self.setDisplayValueColor(0, 0, 0, 255)

        ################################################################################################
        # DEFAULT CENTER POINTER COLOR
        ################################################################################################
        # self.CenterPointColor = QColor(50, 50, 50, 255)
        self.set_CenterPointColor(0, 0, 0, 255)

        ################################################################################################
        # DEFAULT NEEDLE COUNT
        ################################################################################################
        self.value_needle_count = 1

        # self.value_needle = QObject

        ################################################################################################
        # DEFAULT MINIMUM AND MAXIMUM VALUE
        ################################################################################################
        self.minValue = 0
        self.maxValue = 1000
        ################################################################################################
        # DEFAULT START VALUE
        ################################################################################################
        self.value = self.minValue
        #the reading the number in the middle shows: unlike value (and the needle), not stopped at maxValue
        self.reading = self.value
        #where the needle is drawn; eases toward self.value (the real reading) so the needle glides between readings
        self.displayValue = self.value
        #the gauge is drawn into a cached pixmap and only redrawn after update()/update_frame(), so repaints
        #caused by an animated widget behind it (e.g. the rotating chainring) just copy the pixmap. Its face
        #(the outer ring under the band, the tick marks and scale numbers over it) is cached too, and only
        #redrawn after update() (paintCacheDirty); update_frame() redraws just the moving parts over it
        self.paintTarget = self
        self.paintCache = None
        self.paintCacheDirty = True
        self.frameDirty = True
        self.faceUnder = None
        self.faceOver = None
        #create_polygon_pie()'s polygons by their arguments: the band's ends at the needle, so it takes
        #one of a few hundred shapes, and the rest never change
        self.pieCache = {}
        #the easing's frame rate is the ⚙ menu's Performance setting (each frame only redraws the moving parts,
        #see paintEvent); 0 fps: no easing, the needle jumps to each reading
        self.needleTimer = QTimer(self)
        self.needleTimer.setTimerType(Qt.TimerType.PreciseTimer)
        self.needleTimer.timeout.connect(self.animate_needle)
        self.needleLastStep = None
        self.apply_performance()
        Performance.watch(self, "apply_performance")

        ################################################################################################
        # DEFAULT OFFSET
        ################################################################################################
        self.value_offset = 0

        self.valueNeedleSnapzone = 0.05
        self.last_value = 0

        # self.value2 = 0
        # self.value2Color = QColor(0, 0, 0, 255)

        ################################################################################################
        # DEFAULT RADIUS
        ################################################################################################
        self.gauge_color_outer_radius_factor = 1
        self.gauge_color_inner_radius_factor = 0.9

        self.center_horizontal_value = 0
        self.center_vertical_value = 0

        ################################################################################################
        # DEFAULT SCALE VALUE
        ################################################################################################
        self.scale_angle_start_value = 135
        self.scale_angle_size = 270

        self.angle_offset = 0

        self.setScalaCount(10)
        self.scala_subdiv_count = 5

        self.pen = QPen(QColor(0, 0, 0))

        ################################################################################################
        # CUSTOM FONT: Orbitron's static weights are registered once at startup in AntWorkout.py
        ################################################################################################

        ################################################################################################
        # DEFAULT POLYGON COLOR
        ################################################################################################
        self.scale_polygon_colors = []

        ################################################################################################
        # BIG SCALE COLOR
        ################################################################################################
        self.bigScaleMarker = QColor("#000000")

        ################################################################################################
        # FINE SCALE COLOR
        ################################################################################################
        self.fineScaleColor = QColor("#000000")

        ################################################################################################
        # DEFAULT SCALE TEXT STATUS
        ################################################################################################
        self.setEnableScaleText(True)
        self.scale_fontname = "Orbitron"
        self.initial_scale_fontsize = 14
        self.scale_fontsize = self.initial_scale_fontsize
        
        ################################################################################################
        # DEFAULT VALUE TEXT STATUS
        ################################################################################################
        self.enable_value_text = True
        self.value_fontname = "Orbitron"
        self.initial_value_fontsize = 40
        self.value_fontsize = self.initial_value_fontsize
        self.text_radius_factor = 0.5
        self.centerTextValues = False #Centers the value on the widget
        self.value_decimals = 0 #decimal places shown in the value text
        self.fit_value_in_center = False #shrink the value text so the widest possible value fits in the needle center circle

        ################################################################################################
        # DEFAULT VALUE EXTRA ARC
        ################################################################################################
        self.new_arc_colors = [[0, QColor(255,255,255,155)]]

        ################################################################################################
        # ENABLE BAR GRAPH BY DEFAULT
        ################################################################################################
        self.setEnableBarGraph(True)
        ################################################################################################
        # FILL POLYGON COLOR BY DEFAULT
        ################################################################################################
        self.setEnableScalePolygon(True)
        ################################################################################################
        # ENABLE CENTER POINTER BY DEFAULT
        ################################################################################################
        self.enable_CenterPoint = True
        self.big_needle_center_size = 8 #default value, smaller value = bigger center circle
        #nested circles in place of the center point (see set_center_rings): None, or ((outer, inner) colors, level)
        self.center_rings = None
        ################################################################################################
        # ARC SETTINGS
        ################################################################################################
        self.setEnableDrawNewArc(False)
        self.arcCenterValue = 600
        self.newArcColors = [[0, QColor(255,255,255,155)]] #White semitransparent
        self.increasePercentage = 1 #multiplier for the increase to the arc width, set to 0 to disable
        ################################################################################################
        # ENABLE FINE SCALE BY DEFAULT
        ################################################################################################
        self.enable_fine_scaled_marker = True
        ################################################################################################
        # ENABLE BIG SCALE BY DEFAULT
        ################################################################################################
        self.enable_big_scaled_marker = True

        ################################################################################################
        # NEEDLE SCALE FACTOR/LENGTH
        ################################################################################################
        self.needle_scale_factor = 0.8
        ################################################################################################
        # ENABLE NEEDLE POLYGON BY DEFAULT
        ################################################################################################
        self.enable_Needle_Polygon = True

        ################################################################################################
        # ALTERNATE UNITS/VALUES VIEW: UNITS BOTTOM, VALUES CENTER
        ################################################################################################
        self.altUnitsDisplay = False

        ################################################################################################
        # SET GAUGE UNITS
        ################################################################################################
        self.units = "℃"

        # QTimer ensures a new display every X ms
        # possibly improve performance here with self.update() and self.use_timer_event = False
        # todo: self.update as default without checking whether self.use_timer_event is set or not
        # Timer starts the paintEvent event every 10ms
        if self.use_timer_event:
            timer = QTimer(self)
            timer.timeout.connect(self.update)
            timer.start(10)
        else:
            self.update()

        ################################################################################################
        # SET DEFAULT THEME
        ################################################################################################
        self.setGaugeTheme(0)

        ################################################################################################
        # RESIZE GAUGE
        ################################################################################################
        self.rescale_method()

        self.valueChanged.connect(self.update_frame)

    #the ring behind the band (AntDevicesPoller sets the power gauge's with every reading): part of the
    #cached face, so it's only redrawn when it actually changes
    @property
    def outer_circle_bg(self):
        return self._outer_circle_bg

    @outer_circle_bg.setter
    def outer_circle_bg(self, stops):
        if stops != getattr(self, "_outer_circle_bg", None):
            self._outer_circle_bg = stops
            self.update()

    def setValue(self, value):
        self.reading = max(value, self.minValue)
        if value <= self.minValue:
            self.value = self.minValue
        elif value >= self.maxValue:
            self.value = self.maxValue
        else:
            self.value = value
        if Performance.gauge_fps() == 0:
            #no easing: the needle is drawn right at the reading
            self.displayValue = self.value
        elif self.displayValue != self.value and not self.needleTimer.isActive():
            self.needleLastStep = time.monotonic()
            self.needleTimer.start()
        self.valueChanged.emit(int(value))

    #the ⚙ menu's Performance setting, at creation and whenever it changes
    def apply_performance(self):
        fps = Performance.gauge_fps()
        if fps == 0:
            #easing turned off part way through: the needle goes straight to the reading
            if self.needleTimer.isActive():
                self.needleTimer.stop()
                self.displayValue = self.value
                self.update_frame()
        else:
            self.needleTimer.setInterval(round(1000/fps))

    #ease the drawn needle toward the latest reading; the timer stops itself once the needle arrives
    #(the step is by the time since the last one, so the needle moves at the same speed at any frame rate)
    def animate_needle(self):
        now = time.monotonic()
        dt = now - self.needleLastStep
        self.needleLastStep = now
        diff = self.value - self.displayValue
        #covers ~90% of the gap in a quarter second, about the time between ANT+ readings
        step = diff*(1 - math.exp(-dt/NEEDLE_EASE_SECONDS))
        if abs(diff - step) < (self.maxValue - self.minValue)*0.0005:
            self.displayValue = self.value
            self.needleTimer.stop()
        else:
            self.displayValue += step
        self.update_frame()

    ################################################################################################
    # SET SCALE FONT FAMILY
    ################################################################################################
    def setScaleFontFamily(self, font):
        self.scale_fontname = str(font)

    ################################################################################################
    # SET VALUE FONT FAMILY
    ################################################################################################
    def setValueFontFamily(self, font):
        self.value_fontname = str(font)

    ################################################################################################
    # SET BIG SCALE COLOR
    ################################################################################################
    def setBigScaleColor(self, color):
        self.bigScaleMarker = QColor(color)      

    ################################################################################################
    # SET FINE SCALE COLOR
    ################################################################################################
    def setFineScaleColor(self, color):
        self.fineScaleColor = QColor(color)    

    ################################################################################################
    # GAUGE THEMES
    ################################################################################################
    def setGaugeTheme(self, Theme = 1):
        if Theme == 0 or Theme == None:
            self.set_scale_polygon_colors([[.00, QColor("#FF0000")],
                                    [.1, QColor("#FFFF00")],
                                    [.15, QColor("#00FF00")],
                                    [1, QColor("#FFFFFFFF")]])

            self.needle_center_bg = [
                                    [0, QColor(35, 40, 3, 255)], 
                                    [0.16, QColor(30, 36, 45, 255)], 
                                    [0.225, QColor(36, 42, 54, 255)], 
                                    [0.423963, QColor(19, 23, 29, 255)], 
                                    [0.580645, QColor(45, 53, 68, 255)], 
                                    [0.792627, QColor(59, 70, 88, 255)], 
                                    [0.935, QColor(30, 35, 45, 255)], 
                                    [1, QColor(35, 40, 3, 255)]
                                    ]

            self.outer_circle_bg =  [
                                    [0.0645161, QColor(30, 35, 45, 255)], 
                                    [0.37788, QColor(57, 67, 86, 255)], 
                                    [1, QColor(30, 36, 45, 255)]
                                    ]

        if Theme == 1:
            self.set_scale_polygon_colors([[.75, QColor("#FF0000")],
                                     [.5, QColor("#FFFF00")],
                                     [.25, QColor("#00FF00")]])

            self.needle_center_bg = [
                                    [0, QColor(35, 40, 3, 255)], 
                                    [0.16, QColor(30, 36, 45, 255)], 
                                    [0.225, QColor(36, 42, 54, 255)], 
                                    [0.423963, QColor(19, 23, 29, 255)], 
                                    [0.580645, QColor(45, 53, 68, 255)], 
                                    [0.792627, QColor(59, 70, 88, 255)], 
                                    [0.935, QColor(30, 35, 45, 255)], 
                                    [1, QColor(35, 40, 3, 255)]
                                    ]

            self.outer_circle_bg =  [
                                    [0.0645161, QColor(30, 35, 45, 255)], 
                                    [0.37788, QColor(57, 67, 86, 255)], 
                                    [1, QColor(30, 36, 45, 255)]
                                    ]

        if Theme == 2:
            self.set_scale_polygon_colors([[.25, QColor("#FF0000")],
                                     [.5, QColor("#FFFF00")],
                                     [.75, QColor("#00FF00")]])

            self.needle_center_bg = [
                                    [0, QColor(35, 40, 3, 255)], 
                                    [0.16, QColor(30, 36, 45, 255)], 
                                    [0.225, QColor(36, 42, 54, 255)], 
                                    [0.423963, QColor(19, 23, 29, 255)], 
                                    [0.580645, QColor(45, 53, 68, 255)], 
                                    [0.792627, QColor(59, 70, 88, 255)], 
                                    [0.935, QColor(30, 35, 45, 255)], 
                                    [1, QColor(35, 40, 3, 255)]
                                    ]

            self.outer_circle_bg =  [
                                    [0.0645161, QColor(30, 35, 45, 255)], 
                                    [0.37788, QColor(57, 67, 86, 255)], 
                                    [1, QColor(30, 36, 45, 255)]
                                    ]

        elif Theme == 3:
            self.set_scale_polygon_colors([[.00, QColor("#FFFFFF")]])

            self.needle_center_bg = [
                                    [0, QColor("#FFFFFF")], 
                                    ]

            self.outer_circle_bg =  [
                                    [0, QColor("#FFFFFF")], 
                                    ]

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 4:
            self.set_scale_polygon_colors([[1, QColor("#000000")]])

            self.needle_center_bg = [
                                    [0, QColor("#000000")], 
                                    ]

            self.outer_circle_bg =  [
                                    [0, QColor("#000000")], 
                                    ]

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 5:
            self.set_scale_polygon_colors([[1, QColor("#029CDE")]])  

            self.needle_center_bg = [
                                    [0, QColor("#029CDE")], 
                                    ]

            self.outer_circle_bg =  [
                                    [0, QColor("#029CDE")], 
                                    ]

        elif Theme == 6:
            self.set_scale_polygon_colors([[.75, QColor("#01ADEF")],
                                     [.5, QColor("#0086BF")],
                                     [.25, QColor("#005275")]])

            self.needle_center_bg = [
                                    [0, QColor(0, 46, 61, 255)], 
                                    [0.322581, QColor(1, 173, 239, 255)], 
                                    [0.571429, QColor(0, 73, 99, 255)],
                                    [1, QColor(0, 46, 61, 255)]
                                    ]

            self.outer_circle_bg =  [
                                    [0.0645161, QColor(0, 85, 116, 255)], 
                                    [0.37788, QColor(1, 173, 239, 255)], 
                                    [1, QColor(0, 69, 94, 255)]
                                    ]

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 7:
            self.set_scale_polygon_colors([[.25, QColor("#01ADEF")],
                                     [.5, QColor("#0086BF")],
                                     [.75, QColor("#005275")]])

            self.needle_center_bg = [
                                    [0, QColor(0, 46, 61, 255)], 
                                    [0.322581, QColor(1, 173, 239, 255)], 
                                    [0.571429, QColor(0, 73, 99, 255)],
                                    [1, QColor(0, 46, 61, 255)]
                                    ]

            self.outer_circle_bg =  [
                                    [0.0645161, QColor(0, 85, 116, 255)], 
                                    [0.37788, QColor(1, 173, 239, 255)], 
                                    [1, QColor(0, 69, 94, 255)]
                                    ]

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 8:
            self.setCustomGaugeTheme(
                color1 = "#ffaa00",
                color2= "#7d5300",
                color3 = "#3e2900"
            )

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 9:
            self.setCustomGaugeTheme(
                color1 = "#3e2900",
                color2= "#7d5300",
                color3 = "#ffaa00"
            )

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 10:
            self.setCustomGaugeTheme(
                color1 = "#ff007f",
                color2= "#aa0055",
                color3 = "#830042"
            )


            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 11:
            self.setCustomGaugeTheme(
                color1 = "#830042",
                color2= "#aa0055",
                color3 = "#ff007f"
            )
            
            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 12:
            self.setCustomGaugeTheme(
                color1 = "#ffe75d",
                color2= "#896c1a",
                color3 = "#232803"
            )

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 13:
            self.setCustomGaugeTheme(
                color1 = "#ffe75d",
                color2= "#896c1a",
                color3 = "#232803"
            )

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 14:
            self.setCustomGaugeTheme(
                color1 = "#232803",
                color2= "#821600",
                color3 = "#ffe75d"
            )

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 15:
            self.setCustomGaugeTheme(
                color1 = "#00FF11",
                color2= "#00990A",
                color3 = "#002603"
            )

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 16:
            self.setCustomGaugeTheme(
                color1 = "#002603",
                color2= "#00990A",
                color3 = "#00FF11"
            )

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 17:
            self.setCustomGaugeTheme(
                color1 = "#00FFCC",
                color2= "#00876C",
                color3 = "#00211B"
            )

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 18:
            self.setCustomGaugeTheme(
                color1 = "#00211B",
                color2= "#00876C",
                color3 = "#00FFCC"
            )

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 19:
            self.setCustomGaugeTheme(
                color1 = "#001EFF",
                color2= "#001299",
                color3 = "#000426"
            )

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 20:
            self.setCustomGaugeTheme(
                color1 = "#000426",
                color2= "#001299",
                color3 = "#001EFF"
            )

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 21:
            self.setCustomGaugeTheme(
                color1 = "#F200FF",
                color2= "#85008C",
                color3 = "#240026"
            )

            self.bigScaleMarker = QColor("#000000")
            self.fineScaleColor = QColor("#000000")

        elif Theme == 22:
            self.setCustomGaugeTheme(
                color1 = "#240026",
                color2= "#85008C",
                color3 = "#F200FF"
            )

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 23:
            self.setCustomGaugeTheme(
                color1 = "#FF0022",
                color2= "#080001",
                color3 = "#009991"
            )

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

        elif Theme == 24:
            self.setCustomGaugeTheme(
                color1 = "#009991",
                color2= "#080001",
                color3 = "#FF0022"
            )

            self.bigScaleMarker = QColor("#FFFFFF")
            self.fineScaleColor = QColor("#FFFFFF")

    ################################################################################################
    # SET CUSTOM GAUGE THEME
    ################################################################################################
    def setCustomGaugeTheme(self, **colors):
        if "color1" in colors and len(str(colors['color1'])) > 0:
            if "color2" in colors and len(str(colors['color2'])) > 0:
                if "color3" in colors and len(str(colors['color3'])) > 0:

                    self.set_scale_polygon_colors([[.25, QColor(str(colors['color1']))],
                                            [.5, QColor(str(colors['color2']))],
                                            [.75, QColor(str(colors['color3']))]])

                    self.needle_center_bg = [
                                            [0, QColor(str(colors['color3']))], 
                                            [0.322581, QColor(str(colors['color1']))], 
                                            [0.571429, QColor(str(colors['color2']))],
                                            [1, QColor(str(colors['color3']))]
                                            ]

                    self.outer_circle_bg =  [
                                            [0.0645161, QColor(str(colors['color3']))], 
                                            [0.36, QColor(str(colors['color1']))], 
                                            [1, QColor(str(colors['color2']))]
                                            ]

                else:

                    self.set_scale_polygon_colors([[.5, QColor(str(colors['color1']))],
                                             [1, QColor(str(colors['color2']))]])

                    self.needle_center_bg = [
                                            [0, QColor(str(colors['color2']))], 
                                            [1, QColor(str(colors['color1']))]
                                            ]

                    self.outer_circle_bg =  [
                                            [0, QColor(str(colors['color2']))], 
                                            [1, QColor(str(colors['color2']))]
                                            ]

            else:

                self.set_scale_polygon_colors([[1, QColor(str(colors['color1']))]])

                self.needle_center_bg = [
                                        [1, QColor(str(colors['color1']))]
                                        ]

                self.outer_circle_bg =  [
                                        [1, QColor(str(colors['color1']))]
                                        ]

        else:
            self.setGaugeTheme(0)

    ################################################################################################
    # SET SCALE POLYGON COLOR
    ################################################################################################
    def setScalePolygonColor(self, **colors):
        if "color1" in colors and len(str(colors['color1'])) > 0:
            if "color2" in colors and len(str(colors['color2'])) > 0:
                if "color3" in colors and len(str(colors['color3'])) > 0:

                    self.set_scale_polygon_colors([[.25, QColor(str(colors['color1']))],
                                            [.5, QColor(str(colors['color2']))],
                                            [.75, QColor(str(colors['color3']))]])

                else:

                    self.set_scale_polygon_colors([[.5, QColor(str(colors['color1']))],
                                             [1, QColor(str(colors['color2']))]])

            else:

                self.set_scale_polygon_colors([[1, QColor(str(colors['color1']))]])


    ################################################################################################
    # SET NEEDLE CENTER COLOR
    ################################################################################################
    def setNeedleCenterColor(self, **colors):
        if "color1" in colors and len(str(colors['color1'])) > 0:
            if "color2" in colors and len(str(colors['color2'])) > 0:
                if "color3" in colors and len(str(colors['color3'])) > 0:

                    self.needle_center_bg = [
                                            [0, QColor(str(colors['color3']))], 
                                            [0.322581, QColor(str(colors['color1']))], 
                                            [0.571429, QColor(str(colors['color2']))],
                                            [1, QColor(str(colors['color3']))]
                                            ]

                else:

                    self.needle_center_bg = [
                                            [0, QColor(str(colors['color2']))], 
                                            [1, QColor(str(colors['color1']))]
                                            ]

            else:

                self.needle_center_bg = [
                                        [1, QColor(str(colors['color1']))]
                                        ]

    ################################################################################################
    # SET OUTER CIRCLE COLOR
    ################################################################################################
    def setOuterCircleColor(self, **colors):
        if "color1" in colors and len(str(colors['color1'])) > 0:
            if "color2" in colors and len(str(colors['color2'])) > 0:
                if "color3" in colors and len(str(colors['color3'])) > 0:

                    self.outer_circle_bg =  [
                                            [0.0645161, QColor(str(colors['color3']))], 
                                            [0.37788, QColor(str(colors['color1']))], 
                                            [1, QColor(str(colors['color2']))]
                                            ]

                else:

                    self.outer_circle_bg =  [
                                            [0, QColor(str(colors['color2']))], 
                                            [1, QColor(str(colors['color2']))]
                                            ]

            else:

                self.outer_circle_bg =  [
                                        [1, QColor(str(colors['color1']))]
                                        ]



    ################################################################################################
    # RESCALE
    ################################################################################################
    def rescale_method(self):
        ################################################################################################
        # SET WIDTH AND HEIGHT
        ################################################################################################
        if self.width() <= self.height():
            self.widget_diameter = self.width()
        else:
            self.widget_diameter = self.height()

        ################################################################################################
        # SET NEEDLE SIZE
        ################################################################################################
        self.change_value_needle_style([QPolygon([
            QPoint(4, 30),
            QPoint(-4, 30),
            QPoint(-2, - int(self.widget_diameter / 2 * self.needle_scale_factor)),
            QPoint(0, - int(self.widget_diameter / 2 * self.needle_scale_factor - 6)),
            QPoint(2, - int(self.widget_diameter / 2 * self.needle_scale_factor))
        ])])


        ################################################################################################
        # SET FONT SIZE
        ################################################################################################
        self.scale_fontsize = self.initial_scale_fontsize * self.widget_diameter / 400
        self.value_fontsize = self.initial_value_fontsize * self.widget_diameter / 400


    def change_value_needle_style(self, design):
        # prepared for multiple needle instrument
        self.value_needle = []
        for i in design:
            self.value_needle.append(i)
        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # UPDATE VALUE
    ################################################################################################
    

        # ohne timer: aktiviere self.update()
        # if not self.use_timer_event:
        #     self.update()

    def updateAngleOffset(self, offset):
        self.angle_offset = offset
        if not self.use_timer_event:
            self.update()

    def center_horizontal(self, value):
        self.center_horizontal_value = value

    def center_vertical(self, value):
        self.center_vertical_value = value

    ################################################################################################
    # SET NEEDLE COLOR
    ################################################################################################
    def setNeedleColor(self, R=50, G=50, B=50, Transparency=255):
        # Red: R = 0 - 255
        # Green: G = 0 - 255
        # Blue: B = 0 - 255
        # Transparency = 0 - 255
        self.NeedleColor = QColor(R, G, B, Transparency)
        self.NeedleColorReleased = self.NeedleColor

        if not self.use_timer_event:
            self.update()
    ################################################################################################
    # SET NEEDLE COLOR ON DRAG
    ################################################################################################
    def setNeedleColorOnDrag(self, R=50, G=50, B=50, Transparency=255):
        # Red: R = 0 - 255
        # Green: G = 0 - 255
        # Blue: B = 0 - 255
        # Transparency = 0 - 255
        self.NeedleColorDrag = QColor(R, G, B, Transparency)

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET SCALE VALUE COLOR
    ################################################################################################
    def setScaleValueColor(self, R=50, G=50, B=50, Transparency=255):
        # Red: R = 0 - 255
        # Green: G = 0 - 255
        # Blue: B = 0 - 255
        # Transparency = 0 - 255
        self.ScaleValueColor = QColor(R, G, B, Transparency)

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET DISPLAY VALUE COLOR
    ################################################################################################
    def setDisplayValueColor(self, R=50, G=50, B=50, Transparency=255):
        # Red: R = 0 - 255
        # Green: G = 0 - 255
        # Blue: B = 0 - 255
        # Transparency = 0 - 255
        self.DisplayValueColor = QColor(R, G, B, Transparency)

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET CENTER POINTER COLOR
    ################################################################################################
    def set_CenterPointColor(self, R=50, G=50, B=50, Transparency=255):
        self.CenterPointColor = QColor(R, G, B, Transparency)

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SHOW HIDE NEEDLE POLYGON
    ################################################################################################
    def setEnableNeedlePolygon(self, enable = True):
        self.enable_Needle_Polygon = enable

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # ENABLE EXTRA ARC
    ################################################################################################
    def setEnableDrawNewArc(self, enable = True):
        self.enable_draw_new_arc = enable

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SHOW HIDE SCALE TEXT
    ################################################################################################
    def setEnableScaleText(self, enable = True):
        self.enable_scale_text = enable

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SHOW HIDE BAR GRAPH
    ################################################################################################
    def setEnableBarGraph(self, enable = True):
        self.enableBarGraph = enable

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SHOW HIDE VALUE TEXT
    ################################################################################################
    def setEnableValueText(self, enable = True):
        self.enable_value_text = enable

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SHOW HIDE CENTER POINTER
    ################################################################################################
    def setEnableCenterPoint(self, enable = True):
        self.enable_CenterPoint = enable

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SHOW HIDE FILLED POLYGON
    ################################################################################################
    def setEnableScalePolygon(self, enable = True):
        self.enable_filled_Polygon = enable

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SHOW HIDE BIG SCALE
    ################################################################################################
    def setEnableBigScaleGrid(self, enable = True):
        self.enable_big_scaled_marker = enable

        if not self.use_timer_event:
            self.update()


    ################################################################################################
    # SHOW HIDE FINE SCALE
    ################################################################################################
    def setEnableFineScaleGrid(self, enable = True):
        self.enable_fine_scaled_marker = enable

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SHOW HIDE SCALA MAIN CONT
    ################################################################################################
    def setScalaCount(self, count):
        if count < 1:
            count = 1
        self.scalaCount = count

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET MINIMUM VALUE
    ################################################################################################
    def setMinValue(self, min):
        if self.value < min:
            self.value = min
            self.displayValue = min
        if min >= self.maxValue:
            self.minValue = self.maxValue - 1
        else:
            self.minValue = min

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET MAXIMUM VALUE
    ################################################################################################
    def setMaxValue(self, max):
        if self.value > max:
            self.value = max
            self.displayValue = max
        if max <= self.minValue:
            self.maxValue = self.minValue + 1
        else:
            self.maxValue = max

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET SCALE ANGLE
    ################################################################################################
    def setScaleStartAngle(self, value):
        # Value range in DEG: 0 - 360
        self.scale_angle_start_value = value

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET SCALE SIZE
    ################################################################################################
    def setTotalScaleAngleSize(self, value):
        self.scale_angle_size = value

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET GAUGE COLOR OUTER RADIUS
    ################################################################################################
    def setGaugeColorOuterRadiusFactor(self, value):
        self.gauge_color_outer_radius_factor = float(value) / 1000

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET GAUGE COLOR INNER RADIUS
    ################################################################################################
    def setGaugeColorInnerRadiusFactor(self, value):
        self.gauge_color_inner_radius_factor = float(value) / 1000

        if not self.use_timer_event:
            self.update()

    ################################################################################################
    # SET SCALE POLYGON COLOR
    ################################################################################################
    def set_scale_polygon_colors(self, color_array):
        if 'list' in str(type(color_array)):
            self.scale_polygon_colors = color_array
        elif color_array == None:
            self.scale_polygon_colors = [[.0, Qt.GlobalColor.transparent]]
        else:
            self.scale_polygon_colors = [[.0, Qt.GlobalColor.transparent]]

        if not self.use_timer_event:
            self.update()

    def value_gradient_position(self, value):
        """Where a value on the scale falls in the band's gradient (see draw_filled_polygon): the
        conical gradient starts just past the end of the scale and runs the other way round"""
        fraction = (value - self.minValue)/(self.maxValue - self.minValue)
        return (self.scale_angle_size*(1 - fraction) + 1)/360

    def set_value_colors(self, stops):
        """The colored band from (value, QColor) stops: each color sits exactly where the needle points
        at its value. The end of the scale has the highest value's color (even when that value is past the
        end); stops below the start are blended into the color at the start"""
        stops = sorted(stops, key=lambda stop: stop[0])
        if not stops:
            self.set_scale_polygon_colors(None)
            return
        def color_at(value):
            if value <= stops[0][0]:
                return QColor(stops[0][1])
            for (v0, c0), (v1, c1) in zip(stops, stops[1:]):
                if value <= v1:
                    t = (value - v0)/(v1 - v0) if v1 != v0 else 1
                    return QColor.fromRgbF(*(a + (b - a)*t for a, b in zip(c0.getRgbF(), c1.getRgbF())))
            return QColor(stops[-1][1])
        inRange = [(v, QColor(c)) for v, c in stops if self.minValue <= v <= self.maxValue]
        ends = [(v, color) for v, color in ((self.minValue, color_at(self.minValue)),
                                            (self.maxValue, QColor(stops[-1][1])))
                if not any(abs(v - s[0]) < 1e-9 for s in inRange)]
        self.set_scale_polygon_colors([[self.value_gradient_position(v), c]
                                       for v, c in sorted(inRange + ends, key=lambda stop: stop[0])])
        self.update() #(set_scale_polygon_colors leaves timer-driven gauges to their next tick)

    ################################################################################################
    # GET MAXIMUM VALUE
    ################################################################################################
    def get_value_max(self):
        return self.maxValue

    ###############################################################################################
    # SCALE PAINTER
    ###############################################################################################

    ################################################################################################
    # CREATE PIE
    ################################################################################################
    def create_polygon_pie(self, outer_radius, inner_raduis, start, length, bar_graph = True):
        polygon_pie = QPolygonF()
        # start = self.scale_angle_start_value
        # start = 0
        # length = self.scale_angle_size
        # length = 180
        # inner_raduis = self.width()/4
        n = 360     # angle steps size for full circle
        # changing n value will causes drawing issues
        w = 360 / n   # angle per step
        # create outer circle line from "start"-angle to "start + length"-angle
        x = 0
        y = 0

        # todo enable/disable bar graf here
        if not self.enableBarGraph and bar_graph:
            # float_value = ((length / (self.maxValue - self.minValue)) * (self.value - self.minValue))
            length = int(round((length / (self.maxValue - self.minValue)) * (self.displayValue - self.minValue)))

        key = (outer_radius, inner_raduis, start, length, self.angle_offset)
        cached = self.pieCache.get(key)
        if cached is not None:
            return cached

        # mymax = 0

        for i in range(length+1):                                              # add the points of polygon
            t = w * i + start - self.angle_offset
            x = outer_radius * math.cos(math.radians(t))
            y = outer_radius * math.sin(math.radians(t))
            polygon_pie.append(QPointF(x, y))
        # create inner circle line from "start + length"-angle to "start"-angle
        for i in range(length+1):                                              # add the points of polygon
            t = w * (length - i) + start - self.angle_offset
            x = inner_raduis * math.cos(math.radians(t))
            y = inner_raduis * math.sin(math.radians(t))
            polygon_pie.append(QPointF(x, y))

        # close outer line
        polygon_pie.append(QPointF(x, y))
        #(a resize makes a new set of shapes; the old ones are dropped once there are plenty)
        if len(self.pieCache) >= 1000:
            self.pieCache.clear()
        self.pieCache[key] = polygon_pie
        return polygon_pie

    def draw_filled_polygon(self, outline_pen_with=0):
        if not self.scale_polygon_colors == None:
            painter_filled_polygon = QPainter(self.paintTarget)
            painter_filled_polygon.setRenderHint(QPainter.RenderHint.Antialiasing)
            # Koordinatenursprung in die Mitte der Flaeche legen
            painter_filled_polygon.translate(self.width() / 2, self.height() / 2)

            painter_filled_polygon.setPen(Qt.PenStyle.NoPen)

            self.pen.setWidth(outline_pen_with)
            if outline_pen_with > 0:
                painter_filled_polygon.setPen(self.pen)

            colored_scale_polygon = self.create_polygon_pie(
                ((self.widget_diameter / 2) - (self.pen.width() / 2)) * self.gauge_color_outer_radius_factor,
                (((self.widget_diameter / 2) - (self.pen.width() / 2)) * self.gauge_color_inner_radius_factor),
                self.scale_angle_start_value, self.scale_angle_size)

            gauge_rect = QRect(QPoint(0, 0), QSize(int(self.widget_diameter / 2 - 1), int(self.widget_diameter - 1)))
            grad = QConicalGradient(QPointF(0, 0), - self.scale_angle_size - self.scale_angle_start_value +
                                    self.angle_offset - 1)

            # todo definition scale color as array here
            for eachcolor in self.scale_polygon_colors:
                grad.setColorAt(eachcolor[0], eachcolor[1])
            # grad.setColorAt(.00, QColor("#FF0000"))
            # grad.setColorAt(.1, QColor("#FFFF00"))
            # grad.setColorAt(.15, QColor("#00FF00"))
            # grad.setColorAt(1, Qt.transparent)
            # self.brush = QBrush(QColor(255, 0, 255, 255))
            # grad.setStyle(Qt.Dense6Pattern)
            # painter_filled_polygon.setBrush(self.brush)
            painter_filled_polygon.setBrush(grad)
           

            painter_filled_polygon.drawPolygon(colored_scale_polygon)
            # return painter_filled_polygon

    def draw_icon_image(self):
        pass

    ###############################################################################################
    # BIG SCALE MARKERS
    ###############################################################################################
    def draw_big_scaled_marker(self):
        my_painter = QPainter(self.paintTarget)
        my_painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        # Koordinatenursprung in die Mitte der Flaeche legen
        my_painter.translate(self.width() / 2, self.height() / 2)

        # my_painter.setPen(Qt.PenStyle.NoPen)
        self.pen = QPen(self.bigScaleMarker)
        self.pen.setWidth(2)
        # # if outline_pen_with > 0:
        my_painter.setPen(self.pen)

        my_painter.rotate(self.scale_angle_start_value - self.angle_offset)
        steps_size = (float(self.scale_angle_size) / float(self.scalaCount))
        scale_line_outer_start = self.widget_diameter/2
        scale_line_length = (self.widget_diameter / 2) - (self.widget_diameter / 20)
        for i in range(self.scalaCount+1):
            my_painter.drawLine(int(scale_line_length), 0, int(scale_line_outer_start), 0)
            my_painter.rotate(steps_size)

    def create_scale_marker_values_text(self):
        painter = QPainter(self.paintTarget)
        # painter.setRenderHint(QPainter.HighQualityAntialiasing)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        # Koordinatenursprung in die Mitte der Flaeche legen
        painter.translate(self.width() / 2, self.height() / 2)
        # painter.save()
        font = QFont(self.scale_fontname, int(self.scale_fontsize), QFont.Weight.Bold)
        fm = QFontMetrics(font)

        pen_shadow = QPen()

        pen_shadow.setBrush(self.ScaleValueColor)
        painter.setPen(pen_shadow)

        text_radius_factor = 0.8
        text_radius = self.widget_diameter/2 * text_radius_factor

        scale_per_div = int((self.maxValue - self.minValue) / self.scalaCount)

        angle_distance = (float(self.scale_angle_size) / float(self.scalaCount))
        for i in range(self.scalaCount + 1):
            # text = str(int((self.maxValue - self.minValue) / self.scalaCount * i))
            text = str(int(self.minValue + scale_per_div * i))
            w = fm.horizontalAdvance(text) + 1
            h = fm.height()
            painter.setFont(QFont(self.scale_fontname, int(self.scale_fontsize), QFont.Weight.Bold))
            angle = angle_distance * i + float(self.scale_angle_start_value - self.angle_offset)
            x = text_radius * math.cos(math.radians(angle))
            y = text_radius * math.sin(math.radians(angle))

            text = [int(x) - int(w/2), int(y) - int(h/2), int(w), int(h), Qt.AlignmentFlag.AlignCenter, text]
            painter.drawText(text[0], text[1], text[2], text[3], text[4], text[5])
        # painter.restore()

    ################################################################################################
    # FINE SCALE MARKERS
    ################################################################################################
    def create_fine_scaled_marker(self):
        #  Description_dict = 0
        my_painter = QPainter(self.paintTarget)

        my_painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        # Koordinatenursprung in die Mitte der Flaeche legen
        my_painter.translate(self.width() / 2, self.height() / 2)

        my_painter.setPen(self.fineScaleColor)
        my_painter.rotate(self.scale_angle_start_value - self.angle_offset)
        steps_size = (float(self.scale_angle_size) / float(self.scalaCount * self.scala_subdiv_count))
        scale_line_outer_start = self.widget_diameter/2
        scale_line_length = (self.widget_diameter / 2) - (self.widget_diameter / 40)
        for i in range((self.scalaCount * self.scala_subdiv_count)+1):
            my_painter.drawLine(int(scale_line_length), 0, int(scale_line_outer_start), 0)
            my_painter.rotate(steps_size)

    ################################################################################################
    # VALUE TEXT
    ################################################################################################
    def create_values_text(self):
        painter = QPainter(self.paintTarget)
        try:
            painter.setRenderHint(QPainter.RenderHint.TextAntialiasing)
        except AttributeError:
            try:
                painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            except AttributeError:
                # Neither hint is available; you can handle this case as needed
                pass

        # painter.setRenderHint(QPainter.AA_HighDpiScaling)
        # painter.setRenderHint(QPainter.SmoothPixmapTransform)

        painter.translate(self.width() / 2, self.height() / 2)
        # painter.save()
        # xShadow = 3.0
        # yShadow = 3.0
        value_fontsize = self.value_fontsize
        if self.fit_value_in_center:
            value_fontsize = self.fitted_value_fontsize()
        font = QFont(self.value_fontname, int(value_fontsize), QFont.Weight.Bold)
        fm = QFontMetrics(font)

        pen_shadow = QPen()

        pen_shadow.setBrush(self.DisplayValueColor)
        painter.setPen(pen_shadow)

        text_radius = self.widget_diameter / 2 * self.text_radius_factor
        if self.altUnitsDisplay:
            text_radius = 0

        # angle_distance = (float(self.scale_angle_size) / float(self.scalaCount))
        # for i in range(self.scalaCount + 1):
        text = f"{self.reading:.{self.value_decimals}f}" if self.value_decimals else str(int(self.reading))
        w = fm.horizontalAdvance(text) + 1
        h = fm.height()
        painter.setFont(font)

        # Mitte zwischen Skalenstart und Skalenende:
        # Skalenende = Skalenanfang - 360 + Skalenlaenge
        # Skalenmitte = (Skalenende - Skalenanfang) / 2 + Skalenanfang
        angle_end = float(self.scale_angle_start_value + self.scale_angle_size - 360)
        angle = (angle_end - self.scale_angle_start_value) / 2 + self.scale_angle_start_value

        x = text_radius * math.cos(math.radians(angle))
        y = text_radius * math.sin(math.radians(angle))

        text = [int(x) - int(w/2), int(y) - int(h/2), int(w), int(h), Qt.AlignmentFlag.AlignCenter, text]
        painter.drawText(text[0], text[1], text[2], text[3], text[4], text[5])


    def fitted_value_fontsize(self):
        #largest font size (up to value_fontsize) where the widest possible value's
        #glyphs stay inside the needle center circle drawn by draw_big_needle_center_point
        radius = (self.widget_diameter / self.big_needle_center_size - 1) * 0.92
        widest = f"{self.maxValue:.{self.value_decimals}f}"
        font = QFont(self.value_fontname, int(self.value_fontsize), QFont.Weight.Bold)
        fm = QFontMetrics(font)
        widestDigit = max("0123456789", key=fm.horizontalAdvance)
        widest = "".join(widestDigit if c.isdigit() else c for c in widest)
        rect = fm.tightBoundingRect(widest)
        corner = math.hypot(rect.width() / 2, rect.height() / 2)
        if corner <= radius:
            return self.value_fontsize
        return max(1, self.value_fontsize * radius / corner)

    ################################################################################################
    # UNITS TEXT
    ################################################################################################
    def create_units_text(self):
        painter = QPainter(self.paintTarget)
        try:
            painter.setRenderHint(QPainter.RenderHint.TextAntialiasing)
        except AttributeError:
            try:
                painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            except AttributeError:
                # Neither hint is available; you can handle this case as needed
                pass

        #Moves the units to the bottom of the screen
        unitPosition = 180
        if self.altUnitsDisplay:
            unitPosition = 360

        painter.translate(self.width() / 2, self.height() / 2)
        font = QFont(self.value_fontname, int(self.value_fontsize / 2.5), QFont.Weight.Bold)
        fm = QFontMetrics(font)

        pen_shadow = QPen()

        pen_shadow.setBrush(self.DisplayValueColor)
        painter.setPen(pen_shadow)

        text_radius = self.widget_diameter / 1.9 * self.text_radius_factor

        text = str(self.units)
        w = fm.horizontalAdvance(text) + 1
        h = fm.height()
        painter.setFont(QFont(self.value_fontname, int(self.value_fontsize / 2.5), QFont.Weight.Bold))

      
        angle_end = float(self.scale_angle_start_value + self.scale_angle_size + unitPosition)
        angle = (angle_end - self.scale_angle_start_value) / 2 + self.scale_angle_start_value

        x = text_radius * math.cos(math.radians(angle))
        y = text_radius * math.sin(math.radians(angle))
        text = [int(x) - int(w/2), int(y) - int(h/2), int(w), int(h), Qt.AlignmentFlag.AlignCenter, text]
        painter.drawText(text[0], text[1], text[2], text[3], text[4], text[5])


    ################################################################################################
    # CENTER POINTER
    ################################################################################################
    CENTER_RING_COUNT = 4

    def set_center_rings(self, colors, level=0.0):
        """Nested circles in place of the center point, shaded from the outer color to the inner, or the
        center point back (colors None). level (0 to 1) brightens them and swells them a little, e.g. on a beat"""
        rings = None if colors is None else (tuple(QColor(c) for c in colors), level)
        old = self.center_rings
        if old is not None and rings is not None and old[0] == rings[0] and abs(old[1] - level) < 0.02:
            return
        if old != rings:
            self.center_rings = rings
            self.update_frame()

    def draw_center_rings(self, painter, radius):
        (outer, inner), level = self.center_rings
        white = QColor("#ffffff")
        def mix(a, b, t):
            return QColor.fromRgbF(*(x + (y - x)*t for x, y in zip(a.getRgbF(), b.getRgbF())))
        count = self.CENTER_RING_COUNT
        for i in range(count):
            t = i/(count - 1) #outermost 0 to innermost 1
            r = radius*(1 - 0.6*t)*(1 + 0.04*level)
            #paler toward the middle (the reading is drawn over it), less pale on the beat
            color = mix(mix(outer, inner, t), white, (0.15 + 0.55*t)*(1 - 0.6*level))
            grad = QRadialGradient(QPointF(0, 0), r)
            grad.setColorAt(0, color.lighter(112))
            grad.setColorAt(1, color.darker(118))
            painter.setBrush(grad)
            painter.setPen(QPen(color.darker(140), max(1.0, radius*0.02)))
            painter.drawEllipse(QPointF(0, 0), r, r)

    def draw_big_needle_center_point(self, diameter=30):
        painter = QPainter(self.paintTarget)
        # painter.setRenderHint(QtGui.QPainter.HighQualityAntialiasing)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        # Koordinatenursprung in die Mitte der Flaeche legen
        painter.translate(self.width() / 2, self.height() / 2)
        painter.setPen(Qt.PenStyle.NoPen)
        # painter.setPen(Qt.PenStyle.NoPen)

        # painter.setBrush(self.CenterPointColor)
        # diameter = diameter # self.widget_diameter/6
        # painter.drawEllipse(int(-diameter / 2), int(-diameter / 2), int(diameter), int(diameter))


        if self.center_rings is not None:
            self.draw_center_rings(painter, (self.widget_diameter / self.big_needle_center_size) - (self.pen.width() / 2))
            return

        # create_polygon_pie(self, outer_radius, inner_raduis, start, length)
        colored_scale_polygon = self.create_polygon_pie(
                ((self.widget_diameter / self.big_needle_center_size) - (self.pen.width() / 2)),
                0,
                self.scale_angle_start_value, 360, False)

        # 150.0 0.0 131 360

        grad = QConicalGradient(QPointF(0, 0), 0)

        # todo definition scale color as array here
        for eachcolor in self.needle_center_bg:
            grad.setColorAt(eachcolor[0], eachcolor[1])
        # grad.setColorAt(.00, QColor("#FF0000"))
        # grad.setColorAt(.1, QColor("#FFFF00"))
        # grad.setColorAt(.15, QColor("#00FF00"))
        # grad.setColorAt(1, Qt.transparent)
        painter.setBrush(grad)
        # self.brush = QBrush(QColor(255, 0, 255, 255))
        # painter_filled_polygon.setBrush(self.brush)

        painter.drawPolygon(colored_scale_polygon)
        # return painter_filled_polygon

    ################################################################################################
    # CREATE OUTER COVER
    ################################################################################################
    def draw_outer_circle(self, diameter=30):
        painter = QPainter(self.paintTarget)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        painter.translate(self.width() / 2, self.height() / 2)
        painter.setPen(Qt.PenStyle.NoPen)
        colored_scale_polygon = self.create_polygon_pie(
                ((self.widget_diameter / 2) - (self.pen.width())),
                (self.widget_diameter / 6),
                self.scale_angle_start_value / 10, 360, False)

        radialGradient = QRadialGradient(QPointF(0, 0), self.width())

        for eachcolor in self.outer_circle_bg:
            radialGradient.setColorAt(eachcolor[0], eachcolor[1])


        painter.setBrush(radialGradient)

        painter.drawPolygon(colored_scale_polygon)


    
    ################################################################################################
    # Extra Arc
    # Select a point in range to make a centered arc
    # Width increases with the ratio of point value to max value
    ################################################################################################
    def draw_new_arc(self):
        painter = QPainter(self.paintTarget)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        arcWidth = 8 + int(40*self.increasePercentage*(self.arcCenterValue/self.maxValue))
        #round up to an even split
        if arcWidth % 2 == 1: arcWidth = arcWidth + 1
        #move the given center value back halfway through the arcWidth
        self.new_arc_start_value = 135 + int(270*(self.arcCenterValue/self.maxValue)) - int(arcWidth/2)
        if self.arcCenterValue == 0:
            arcWidth = 0

        painter.translate(self.width() / 2, self.height() / 2)
        painter.setPen(Qt.PenStyle.NoPen)
        colored_scale_polygon = self.create_polygon_pie(
                ((self.widget_diameter / 1.95) - (self.pen.width())),
                (self.widget_diameter / 6),
                self.new_arc_start_value, arcWidth, False)

        radialGradient = QRadialGradient(QPointF(0, 0), self.width())

        for eachcolor in self.newArcColors:
            radialGradient.setColorAt(eachcolor[0], eachcolor[1])


        painter.setBrush(radialGradient)

        painter.drawPolygon(colored_scale_polygon)


    ################################################################################################
    # NEEDLE POINTER
    ################################################################################################
    def draw_needle(self):
        painter = QPainter(self.paintTarget)
        # painter.setRenderHint(QtGui.QPainter.HighQualityAntialiasing)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        # Koordinatenursprung in die Mitte der Flaeche legen
        painter.translate(self.width() / 2, self.height() / 2)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self.NeedleColor)
        painter.rotate(((self.displayValue - self.value_offset - self.minValue) * self.scale_angle_size /
                        (self.maxValue - self.minValue)) + 90 + self.scale_angle_start_value)

        painter.drawConvexPolygon(self.value_needle[0])

    ###############################################################################################
    # EVENTS
    ###############################################################################################

    ################################################################################################
    # ON WINDOW RESIZE
    ################################################################################################
    def resizeEvent(self, event):
        self.rescale_method()
        self.paintCacheDirty = True

    #any change that calls update() redraws the whole gauge, its cached face included
    #(args ignored: Qt's own calls pass some)
    def update(self, *args):
        self.paintCacheDirty = True
        self.update_frame()

    #a change to only the moving parts (needle, band, reading, center, target arc): drawn over the cached face
    #(args ignored: it's also connected to valueChanged(int))
    def update_frame(self, *args):
        self.frameDirty = True
        super().update()

    ################################################################################################
    # ON PAINT EVENT
    ################################################################################################
    def paintEvent(self, event):
        dpr = self.devicePixelRatioF()
        size = (round(self.width()*dpr), round(self.height()*dpr))
        if self.paintCache is None or self.paintCache.devicePixelRatio() != dpr or \
                (self.paintCache.width(), self.paintCache.height()) != size:
            self.paintCache = self.new_layer(size, dpr)
            self.paintCacheDirty = True
        if self.paintCacheDirty:
            self.faceUnder = self.draw_layer(self.new_layer(size, dpr), self.draw_face_under)
            self.faceOver = self.draw_layer(self.new_layer(size, dpr), self.draw_face_over)
            self.paintCacheDirty = False
            self.frameDirty = True
        if self.frameDirty:
            self.paintCache.fill(Qt.GlobalColor.transparent)
            self.draw_layer(self.paintCache, self.draw_frame)
            self.frameDirty = False
        QPainter(self).drawPixmap(0, 0, self.paintCache)

    @staticmethod
    def new_layer(size, dpr):
        layer = QPixmap(*size)
        layer.setDevicePixelRatio(dpr)
        layer.fill(Qt.GlobalColor.transparent)
        return layer

    def draw_layer(self, layer, draw):
        """draw() onto the layer pixmap (the draw_ methods paint on self.paintTarget)"""
        self.paintTarget = layer
        try:
            draw()
        finally:
            self.paintTarget = self
        return layer

    # Main Drawing: the cached face under and over the band, then every frame the band and everything that
    # moves over them, in the order the whole gauge was always drawn in
    # vgl http://doc.qt.io/qt-4.8/qt-demos-affine-xform-cpp.html
    def draw_face_under(self):
        self.draw_outer_circle()
        self.draw_icon_image()

    def draw_face_over(self):
        # draw scale marker lines
        if self.enable_fine_scaled_marker:
            self.create_fine_scaled_marker()
        if self.enable_big_scaled_marker:
            self.draw_big_scaled_marker()

        # draw scale marker value text
        if self.enable_scale_text:
            self.create_scale_marker_values_text()

    def draw_frame(self):
        QPainter(self.paintTarget).drawPixmap(0, 0, self.faceUnder)
        # colored pie area
        if self.enable_filled_Polygon:
            self.draw_filled_polygon()
        #drawing the big scale markers (cached in the face, so not drawn here) leaves self.pen 2 wide after the band
        #set it to 0, and the center point, the arc and the next face's outer circle are sized by that width
        if self.enable_big_scaled_marker:
            self.pen.setWidth(2)
        QPainter(self.paintTarget).drawPixmap(0, 0, self.faceOver)

        # draw needle 1
        if self.enable_Needle_Polygon:
            self.draw_needle()

        # Draw Center Point
        if self.enable_CenterPoint:
            self.draw_big_needle_center_point(diameter=(self.widget_diameter / 6))

        # Display Value
        if self.enable_value_text:
            self.create_values_text()
            self.create_units_text()

        if self.enable_draw_new_arc:
            self.draw_new_arc()


################################################################################################
# END ==>
################################################################################################



class BeatingSvgWidget(QWidget):
    #background image that flips between a resting and a beating svg once per beat, at the rate bpmSource() returns
    #only repaints twice per beat (not every frame), so it stays cheap even though the gauge drawn over it repaints too
    BEAT_FRACTION = 0.35 #portion of each beat spent showing the larger heart

    def __init__(self, restSvgFile, beatSvgFile, restColor, beatColor, bpmSource):
        super().__init__()
        self.restRenderer = self.colored_renderer(restSvgFile, restColor)
        self.beatRenderer = self.colored_renderer(beatSvgFile, beatColor)
        self.bpmSource = bpmSource
        self.beating = False
        self.beatTimer = QTimer(self)
        self.beatTimer.setSingleShot(True)
        self.beatTimer.setTimerType(Qt.TimerType.PreciseTimer)
        self.beatTimer.timeout.connect(self.next_phase)
        self.beatTimer.start(0)

    def colored_renderer(self, svgFile, color):
        with open(svgFile, 'rb') as f:
            data = f.read().replace(b'fill="#000000"', b'fill="' + QColor(color).name().encode() + b'"')
        renderer = QSvgRenderer(QByteArray(data), self)
        renderer.setAspectRatioMode(Qt.AspectRatioMode.KeepAspectRatio)
        return renderer

    #the rate is re-read every beat so it follows the heart rate as it changes
    def next_phase(self):
        bpm = self.bpmSource()
        if bpm <= 0: #no reading, rest and check again shortly
            self.set_beating(False)
            self.beatTimer.start(500)
            return
        periodMs = 60000/bpm
        if self.beating:
            self.set_beating(False)
            self.beatTimer.start(round(periodMs*(1 - self.BEAT_FRACTION)))
        else:
            self.set_beating(True)
            self.beatTimer.start(round(periodMs*self.BEAT_FRACTION))

    def set_beating(self, beating):
        if beating != self.beating:
            self.beating = beating
            self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        (self.beatRenderer if self.beating else self.restRenderer).render(painter, QRectF(self.rect()))


class RotatingSvgWidget(QWidget):
    #background image that spins clockwise at the rate rpmSource() returns
    #the svg is rendered to a pixmap once per size, each frame only draws that pixmap rotated
    FRAME_MS = 16 #~60fps while spinning
    IDLE_MS = 250 #how often to check for motion again while stopped

    def __init__(self, svgFile, rpmSource):
        super().__init__()
        self.renderer = QSvgRenderer(svgFile, self)
        self.renderer.setAspectRatioMode(Qt.AspectRatioMode.KeepAspectRatio)
        self.rpmSource = rpmSource
        self.scale = 1.0 #the image's size, as a part of the widget's smaller side
        self.angle = 0.0
        self.pixmap = None
        self.lastStep = time.monotonic()
        self.spinTimer = QTimer(self)
        self.spinTimer.setTimerType(Qt.TimerType.PreciseTimer)
        self.spinTimer.timeout.connect(self.step)
        self.spinTimer.start(self.IDLE_MS)

    #advance by elapsed time rather than per tick so the speed stays right even if frames are late
    def step(self):
        now = time.monotonic()
        dt = now - self.lastStep
        self.lastStep = now
        rpm = self.rpmSource()
        if rpm <= 0: #stopped, nothing to redraw
            if self.spinTimer.interval() != self.IDLE_MS:
                self.spinTimer.setInterval(self.IDLE_MS)
            return
        if self.spinTimer.interval() != self.FRAME_MS:
            self.spinTimer.setInterval(self.FRAME_MS)
            return #dt covered the idle wait, start timing from here
        self.angle = (self.angle + rpm*6*dt) % 360 #rpm*360/60 degrees per second
        self.update()

    def resizeEvent(self, event):
        self.pixmap = None

    def paintEvent(self, event):
        side = int(min(self.width(), self.height())*self.scale)
        if side <= 0:
            return
        dpr = self.devicePixelRatioF()
        if self.pixmap is None or self.pixmap.devicePixelRatio() != dpr or self.pixmap.width() != round(side*dpr):
            self.pixmap = QPixmap(round(side*dpr), round(side*dpr))
            self.pixmap.setDevicePixelRatio(dpr)
            self.pixmap.fill(Qt.GlobalColor.transparent)
            pixmapPainter = QPainter(self.pixmap)
            pixmapPainter.setRenderHint(QPainter.RenderHint.Antialiasing)
            self.renderer.render(pixmapPainter, QRectF(0, 0, side, side))
            pixmapPainter.end()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        painter.setOpacity(0.2) #80% transparent so the gauge's tick marks and units show through
        painter.translate(self.width()/2, self.height()/2)
        painter.rotate(self.angle)
        painter.drawPixmap(QPointF(-side/2, -side/2), self.pixmap)


#svg files ship next to this module (in the frozen build, inside the bundle),
#not in the working directory, so resolve bare names against this folder
_SVG_DIR = os.path.dirname(os.path.abspath(__file__))


class Ui_Form(object):
    def setupUi(self, Form, svgFile, beatSvg=None, spin=False):
        if svgFile:
            svgFile = os.path.join(_SVG_DIR, svgFile)
        if beatSvg:
            beatSvg = (os.path.join(_SVG_DIR, beatSvg[0]),) + tuple(beatSvg[1:])
        Form.setObjectName("Form")
        Form.resize(400, 300)
        self.vLayout = QGridLayout(Form)
        self.vLayout.setObjectName("vLayout")
        self.widget = AnalogGaugeWidget()
        self.widget.setObjectName("widget")
        if beatSvg: #(beatSvgFile, restColor, beatColor), beats at the gauge's current value
            beatSvgFile, restColor, beatColor = beatSvg
            gauge = self.widget
            self.svg = BeatingSvgWidget(svgFile, beatSvgFile, restColor, beatColor,
                                        lambda: gauge.value if gauge.value > gauge.minValue else 0)
            self.vLayout.addWidget(self.svg, 0,0)
        elif spin: #spins at the gauge's current value in RPM
            gauge = self.widget
            self.svg = RotatingSvgWidget(svgFile, lambda: gauge.reading if gauge.reading > gauge.minValue else 0)
            self.vLayout.addWidget(self.svg, 0,0)
        else:
            self.svg = QSvgWidget()
            if svgFile:
                self.svg.load(svgFile)
                self.svg.renderer().setAspectRatioMode(Qt.AspectRatioMode(1))
                self.vLayout.addWidget(self.svg, 0,0)
        self.vLayout.addWidget(self.widget, 0,0)

        self.retranslateUi(Form)

        QMetaObject.connectSlotsByName(Form)

    def retranslateUi(self, Form):
        _translate = QCoreApplication.translate
        Form.setWindowTitle(_translate("Form", "Form"))


class Gauge(QWidget):
    def __init__(self, parent=None, svgFile=None, beatSvg=None, spin=False):
        super().__init__()
        #Setup the UI main window
        self.ui = Ui_Form()
        self.ui.setupUi(self, svgFile, beatSvg, spin)


# if __name__ == "__main__":
#     import sys
#     app = QApplication(sys.argv)
#     Form = QWidget()
#     ui = Ui_Form()
#     ui.setupUi(Form)
#     Form.show()
#     sys.exit(app.exec())
        
# Define specific gauge view(s) with these commands
#        #change the font
#        #QFontDatabase.addApplicationFont(os.path.join(os.path.dirname(__file__), 'fonts/Orbitron/Orbitron-VariableFont_wght.ttf') )
#        self.ui.widget.setNeedleColor(0, 0, 0, 255)
#        #Sets unit color
#        self.ui.widget.setDisplayValueColor(0, 0, 0, 255)
#        #numbered unit count
#        self.ui.widget.setScalaCount(10)
#        #subdivisions between numbers
#        self.ui.widget.scala_subdiv_count = 10
#        #outer bar, polygon colors enable/disable
#        self.ui.widget.setEnableBarGraph(False)
#        #units
#        self.ui.widget.units = "Watts"
#        # self.ui.widget.setGaugeTheme(2) #2 Power, 3 Cadence
#        # self.ui.widget.setCustomGaugeTheme(color1="#996100", color2="#FFA200", color3="#261800")
#        # self.ui.widget.setGaugeTheme(2) #2 Power
        
#        #how far out from the center the outer bar, polygon colors spread
#        self.ui.widget.gauge_color_outer_radius_factor = 1
#        self.ui.widget.gauge_color_inner_radius_factor = 0.92

#        #outer bar, current level indicator
#        self.ui.widget.set_scale_polygon_colors([[.25, Qt.red],
#                                                [.5, Qt.yellow],
#                                                [.75, Qt.white]])

#        self.ui.widget.needle_center_bg = [
#                                        [0, QColor(30, 40, 3, 255)], 
#                                     [0.16, QColor(30, 36, 45, 255)], 
#                                    [0.225, QColor(36, 42, 54, 255)], 
#                                 [0.423963, QColor(19, 23, 29, 255)], 
#                                 [0.580645, QColor(45, 53, 68, 255)], 
#                                 [0.792627, QColor(59, 70, 88, 255)], 
#                                    [0.935, QColor(30, 35, 45, 255)], 
#                                        [1, QColor(35, 40, 3, 255)]
#                                        ]

#        self.ui.widget.outer_circle_bg =  [
#                                            [0, QColor(0,0,0,0)] #transparent
#                                #         [0, QColor(200, 70, 23, 255)], 
#                                #      [0.16, QColor(60, 66, 75, 255)], 
#                                #     [0.225, QColor(66, 72, 84, 255)], 
#                                #  [0.753963, QColor(49, 53, 59, 255)], 
#                                #  [0.580675, QColor(75, 83, 68, 255)], 
#                                #  [0.792627, QColor(59, 70, 88, 255)], 
#                                #     [0.935, QColor(60, 55, 75, 255)], 
#                                #         [1, QColor(255, 70, 23, 255)]
#                                        ]