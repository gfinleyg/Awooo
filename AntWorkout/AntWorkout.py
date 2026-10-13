###ant_native.pyd MUST finish loading before PyQt6 touches the process.
###Importing PyQt6 first causes a hard access violation (0xC0000005) the
###moment ant_native.pyd is later loaded - some DLL PyQt6 pulls onto the
###process's search path shadows/conflicts with one ant_native.pyd (or
###ANT_LIB.dll) depends on. Forcing ant_native to load here, before any
###PyQt6 import anywhere below (including via ClassAnalogGauge and friends),
###works around it. Do not move this below the PyQt6 imports.
import os as _os
import sys as _sys
###The music analysis (Musical Cadence Mode, see MusicCadence.py) runs in worker processes. A frozen
###build's workers are this exe started again, and freeze_support() turns them into workers right
###here; when run from source they import this file as __mp_main__ instead, which the
###__name__ == "__main__" check below keeps from starting another copy of the app.
import multiprocessing as _multiprocessing
_multiprocessing.freeze_support()
###Frozen (PyInstaller) build: settings, logs and workouts live next to the
###exe, so run from there no matter where it was launched from. Bundled
###fonts/sounds/svgs are found via __file__, which points inside the bundle.
if getattr(_sys, "frozen", False):
    _os.chdir(_os.path.dirname(_sys.executable))
_ant_native_python_dir = _os.path.join(
    _os.path.dirname(_os.path.abspath(__file__)),
    "AntNative",
    "python",
)
if _ant_native_python_dir not in _sys.path:
    _sys.path.insert(0, _ant_native_python_dir)
import ant.manager as _ant_manager_preload  # noqa: F401  (import order guard, see above)



if __name__ == "__main__":
    import ClassAntLogger
    #log Python errors from here on, including any raised while importing the UI below
    ClassAntLogger.install_error_logging()

    from ClassMainWindow import MainWindow

    from PyQt6.QtGui import QFont
    from PyQt6.QtWidgets import QApplication

    import sys

    from AppState import AppState
    import AppStyle
    import AppTheme
    import KeyShortcuts
    import SavedSettings
    import SoundEffects

    ###Start the application
    app = QApplication(sys.argv)

    #every stylesheet/color/font set from here on goes through the current theme (the app's own,
    #AppTheme.BUILT_IN_THEME, until one is loaded), so a theme from the settings menu can be applied on the fly
    AppTheme.install()

    #a key for every button, Ctrl shows them (see KeyShortcuts.py)
    KeyShortcuts.install(app)

    #UI sound effects (button clicks, screen changes, workout complete)
    SoundEffects.init(app)

    #the system default font, before the theme replaces the app-wide one: buttons keep it (styled QPushButtons
    #set their own in AppStyle.APP_STYLESHEET; this covers the rest, e.g. the gear menu button), see AppTheme.apply.
    #The 'Orbitron' and 'Segoe UI Black' the stylesheets name only mark which of the theme's fonts to use
    AppState.defaultFont = QFont(app.font())
    #app wide look (see AppStyle.py)
    app.setStyleSheet(AppStyle.APP_STYLESHEET)
    #theme loaded last time from the settings menu, otherwise the default theme (if there is one)
    _themeFile = SavedSettings.load().get("themeFile")
    try:
        if _themeFile:
            AppTheme.load_file(_themeFile)
        else:
            AppTheme.clear()
    except (OSError, ValueError) as e:
        ClassAntLogger.log_event("ERROR", f"Failed to load theme {_themeFile}: {e}")
        AppTheme.clear()
    #then the colors/gradients files loaded on top of it last time
    for _partFile in (SavedSettings.load().get("themePartFiles") or {}).values():
        try:
            AppTheme.load_file(_partFile)
        except (OSError, ValueError) as e:
            ClassAntLogger.log_event("ERROR", f"Failed to load {_partFile}: {e}")

    w = MainWindow()
    w.show()

    code = app.exec()

    #Out without Python's own teardown: by now the ANT stick is closed (ConfirmExitWindow.exit_program), settings and
    #the log are written as they change and the sound thread is stopped (aboutToQuit), so there's nothing left for it
    #to do but destroy the Qt objects in whatever order it gets to them, which could crash Qt6Core on the way out
    for stream in (sys.stdout, sys.stderr):
        if stream is not None:
            stream.flush()
    _os._exit(code)
