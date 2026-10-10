"""The app wide stylesheet, set by AntWorkout.py and LayoutViewer.py.

Laid out for 1920x1200; the app scales it (and everything else) to fit other screen sizes, see AppLayout.py.
Text is black on purpose rather than left to Qt's default: a theme recolors black, never the default.
"""

APP_STYLESHEET = """
                    CustomTitleBar{background-color: transparent}
                    QLineEdit{  color: black;
                                border-style:solid;
                                border-color:black;
                                border-width: 2px;
                                background-color: qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #bdd6de stop:1 #9fc5d1)}
                    QAbstractSpinBox{color: black;}
                    QLabel{color: black; font: 900 14pt 'Orbitron'; qproperty-indent:3; border-radius: 15px; background-color: transparent;}
                    QPushButton {
                                color: black;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 #1CB5E0, stop: 1 #dadbde);
                                border-style: outset;
                                border-width: 2px;
                                border-radius: 15px;
                                font: bold 34px 'Segoe UI Black';
                                padding: 4px;
                                }
                    QPushButton:pressed {
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, stop: 1 #1f15e1);
                                }
                    QPushButton:disabled {
                                color: grey;
                                background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #4a4952, stop: 1 #716f7d);
                                }
                    """
