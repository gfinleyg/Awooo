import os

from PyQt6.QtCore import QSize, Qt
from PyQt6.QtGui import QGuiApplication, QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QFileDialog,
    QMenu,
    QMessageBox,
    QStyle,
    QToolButton,
    QWidget,
)

import ClassAntLogger

#letting go of a title bar drag with the cursor this close to the top of a screen maximizes the window there
SNAP_DISTANCE = 6

class CustomTitleBar(QWidget):
    def __init__(self, parent, mainWindow=True):
        super().__init__(parent)
        self.setAutoFillBackground(True)
        # self.setBackgroundRole(QPalette.ColorRole.Highlight)
        self.initial_pos = None
        self.snaps = mainWindow #dialogs just move
        title_bar_layout = QHBoxLayout(self)
        title_bar_layout.setContentsMargins(1, 1, 1, 1)
        title_bar_layout.setSpacing(2)

        self.title = QLabel(f"{self.__class__.__name__}", self)
        if mainWindow:
            self.title.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
            self.title.setStyleSheet("""font-weight: bold;
                                        font: 900 20pt 'Orbitron';
                                        background-color:#bdd6de;
                                        border: 2px solid #bdd6de;
                                        border-radius: 16px;
                                        """
            )
        else:
            self.title.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.title.setStyleSheet("""font: 900 20pt 'Orbitron';
                                        background-color:#9fc5d1;
                                        border: 2px solid white;
                                        border-radius: 16px;
                                        """
            )
        if title := parent.windowTitle():
            self.title.setText(title)
        title_bar_layout.addWidget(self.title)

        # Settings (gear) button - only on the shared title bar MainWindow
        # builds once and reuses across the setup, workout and synopsis
        # pages (mainWindow=True; the small popup windows like
        # ConnectDevicesWindow/SelectDevicesWindow pass False and don't
        # get one). Its dropdown holds the unified log's two actions.
        self.settings_button = None
        if mainWindow:
            self.settings_button = QToolButton(self)
            self.settings_button.setText("⚙")  # gear glyph, no icon asset needed
            self.settings_button.setToolTip("Settings")
            self.settings_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)

            settings_menu = QMenu(self.settings_button)
            #Match the title label's own look (this same font/background
            #combination is the app's convention everywhere - CustomTitleBar's
            #own title text above, and the popup windows built from it) rather
            #than leaving the dropdown in the OS's plain default menu style.
            settings_menu.setStyleSheet(
                """
                QMenu {
                    background-color: #bdd6de;
                    border: 2px solid #bdd6de;
                    font: 900 14pt 'Orbitron';
                }
                QMenu::item {
                    padding: 6px 24px;
                }
                QMenu::item:selected {
                    background-color: #9fc5d1;
                }
                """
            )
            show_logs_action = settings_menu.addAction("Show Logs")
            show_logs_action.triggered.connect(self.show_logs)
            clear_logs_action = settings_menu.addAction("Clear Logs")
            clear_logs_action.triggered.connect(self.clear_logs)
            settings_menu.addSeparator()
            load_theme_action = settings_menu.addAction("Load Theme...")
            load_theme_action.triggered.connect(self.load_theme)
            clear_theme_action = settings_menu.addAction("Clear Theme")
            clear_theme_action.triggered.connect(self.clear_theme)
            settings_menu.addSeparator()
            measurements_action = settings_menu.addAction("Measurements...")
            measurements_action.triggered.connect(self.measurements)
            performance_action = settings_menu.addAction("Performance...")
            performance_action.triggered.connect(self.performance)
            self.settings_button.setMenu(settings_menu)

        # Min button
        self.min_button = QToolButton(self)
        min_icon = self.style().standardIcon(
            QStyle.StandardPixmap.SP_TitleBarMinButton
        )
        self.min_button.setIcon(min_icon)
        self.min_button.clicked.connect(self.window().showMinimized)

        # Max button
        self.max_button = QToolButton(self)
        max_icon = self.style().standardIcon(
            QStyle.StandardPixmap.SP_TitleBarMaxButton
        )
        self.max_button.setIcon(max_icon)
        self.max_button.clicked.connect(self.window().showMaximized)

        # Close button
        self.close_button = QToolButton(self)
        close_icon = self.style().standardIcon(
            QStyle.StandardPixmap.SP_TitleBarCloseButton
        )
        self.close_button.setIcon(close_icon)
        self.close_button.clicked.connect(self.window().close)
        #Esc doubles as the X on the popup windows (the main window's X asks to exit the app, so it's left off there)
        if not mainWindow:
            QShortcut(QKeySequence(Qt.Key.Key_Escape), self.window(), activated=self.close_button.click)

        buttons = [
            self.min_button,
            self.max_button,
            self.close_button,
        ]
        if self.settings_button is not None:
            buttons.insert(0, self.settings_button)
        for button in buttons:
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.setFixedSize(QSize(28, 28))
            if mainWindow:
                button.setStyleSheet(
                    """QToolButton { border: 1px solid #bdd6de;
                                    border-radius: 12px;
                                    }
                    """
                )
            else:
                button.setStyleSheet(
                    """QToolButton { border: 1px solid white;
                                    border-radius: 12px;
                                    }
                    """
                )
            title_bar_layout.addWidget(button)

    def show_logs(self):
        #Lazy import: ShowLogsWindow builds its own CustomTitleBar, so
        #importing it at module level here would be circular.
        from ClassShowLogsWindow import ShowLogsWindow
        self._logs_window = ShowLogsWindow()
        self._logs_window.move(20, 20)
        self._logs_window.show()

    def clear_logs(self):
        ClassAntLogger.clear_log()

    def load_theme(self):
        #a theme, colors or gradients .json saved from LayoutViewer.py; applies right away and is remembered for
        #next time (a whole theme replaces everything, a colors/gradients file just that part, see AppTheme.py)
        import AppTheme
        import SavedSettings
        folder = AppTheme.THEMES_DIR if os.path.isdir(AppTheme.THEMES_DIR) else os.getcwd()
        path, _ = QFileDialog.getOpenFileName(self, "Load Theme, Colors or Gradients", folder,
                                              "Theme files (*.json)")
        if not path:
            return
        try:
            kind = AppTheme.load_file(path)
        except (OSError, ValueError) as e:
            ClassAntLogger.log_event("ERROR", f"Failed to load theme {path}: {e}")
            QMessageBox.warning(self, "Load Theme", f"Couldn't load {os.path.basename(path)}:\n{e}")
            return
        if kind == "theme":
            SavedSettings.update(themeFile=path, themePartFiles=AppTheme.part_files())
        else:
            SavedSettings.update(themePartFiles=AppTheme.part_files())
        what = "theme" if kind == "theme" else AppTheme.PARTS[kind]
        ClassAntLogger.log_event("SETUP", f"Loaded {what} {path}")

    def clear_theme(self):
        #back to the default theme (defaultTheme.json), or the app's own fonts and colors without one
        import AppTheme
        import SavedSettings
        AppTheme.clear()
        SavedSettings.update(themeFile=None, themePartFiles={})

    def measurements(self):
        #lbs/kg, W/kg or W/lb, miles/km (see Units.py); each choice applies as it's clicked
        from ClassMeasurementsWindow import MeasurementsWindow
        MeasurementsWindow(self.window()).exec()

    def performance(self):
        #how smoothly the gauge needles move (see Performance.py); applies as the slider moves
        from ClassPerformanceWindow import PerformanceWindow
        PerformanceWindow(self.window()).exec()

    def set_title(self, text):
        #Allows the owning window to update the displayed title text
        #without rebuilding the title bar (e.g. appending the active
        #workout filename).
        self.title.setText(text)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.initial_pos = event.position().toPoint()
        super().mousePressEvent(event)
        event.accept()

    def mouseMoveEvent(self, event):
        if self.initial_pos is not None:
            delta = event.position().toPoint() - self.initial_pos
            self.window().move(
                self.window().x() + delta.x(),
                self.window().y() + delta.y(),
            )
        super().mouseMoveEvent(event)
        event.accept()

    def mouseReleaseEvent(self, event):
        dragged = self.initial_pos is not None
        self.initial_pos = None
        if dragged and self.snaps and event.button() == Qt.MouseButton.LeftButton:
            self.snap_to_top(event.globalPosition().toPoint())
        super().mouseReleaseEvent(event)
        event.accept()

    def snap_to_top(self, cursor):
        """Dropped at the top edge of a screen: maximized on that screen (the frameless window misses out
        on Windows' own snapping)"""
        window = self.window()
        if window.isMaximized() or not getattr(window, "maximizedMode", True):
            #(the layout viewer's display window keeps a simulated screen's exact size)
            return
        screen = QGuiApplication.screenAt(cursor)
        if screen is None or cursor.y() - screen.geometry().top() > SNAP_DISTANCE:
            return
        #onto that screen first: Windows maximizes a window on the screen that holds most of it
        available = screen.availableGeometry()
        window.move(min(max(window.x(), available.left()), available.right() - 100), available.top())
        window.showMaximized()

    def mouseDoubleClickEvent(self, event):
        self.window().showMaximized()
        event.accept()