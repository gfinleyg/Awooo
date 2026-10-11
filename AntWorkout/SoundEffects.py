import atexit
import os

from PyQt6.QtCore import QEvent, QObject, QThread, QTimer, QUrl, pyqtSignal, pyqtSlot
from PyQt6.QtMultimedia import QSoundEffect
from PyQt6.QtWidgets import QAbstractButton, QToolButton

import ClassAntLogger


#UI sound effects. Files live in sounds/ next to this file, 16-bit PCM WAV
#(QSoundEffect only plays uncompressed WAV). A missing file is logged once
#and that sound is just skipped, so the app runs fine without them.
SOUNDS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sounds")

#name: (filename, volume 0.0-1.0)
SOUND_FILES = {
    "click":    ("click.wav",            0.9),
    "window":   ("window_change.wav",    0.9),
    "complete": ("workout_complete.wav", 0.9),
}

_player = None
_playerThread = None
_filter = None
_windowSoundCount = 0 #window sounds requested so far, lets a click tell if it opened a new screen


class _SoundPlayer(QObject):
    #Owns the QSoundEffects on a dedicated thread. QSoundEffect feeds its audio
    #from the thread it lives on, and the workout screen keeps the UI thread
    #busy enough (16 ms bar animation plus gauge/graph repaints) that sounds
    #played from there got starved and never came out.
    playRequested = pyqtSignal(str)

    def __init__(self, paths):
        super().__init__()
        self._paths = paths
        self._effects = {}
        self.playRequested.connect(self._play)

    @pyqtSlot()
    def load(self):
        for name, (path, volume) in self._paths.items():
            effect = QSoundEffect(self)
            effect.setSource(QUrl.fromLocalFile(path))
            effect.setVolume(volume)
            self._effects[name] = effect

    @pyqtSlot(str)
    def _play(self, name):
        effect = self._effects.get(name)
        if effect is not None:
            effect.play()


def init(app):
    """Load every sound and start watching buttons app-wide. Call once, right
    after the QApplication is created."""
    global _player, _playerThread, _filter
    paths = {}
    for name, (filename, volume) in SOUND_FILES.items():
        path = os.path.join(SOUNDS_DIR, filename)
        if not os.path.isfile(path):
            ClassAntLogger.log_event("SOUND", f"Missing sound file sounds/{filename}, '{name}' sound disabled")
            continue
        paths[name] = (path, volume)

    _playerThread = QThread()
    _player = _SoundPlayer(paths)
    _player.moveToThread(_playerThread)
    _playerThread.started.connect(_player.load)
    _playerThread.start()
    #stop the thread before the app ends (AntWorkout.py leaves without Python's
    #teardown once its event loop is done), or Python tears the objects down
    app.aboutToQuit.connect(_stop_player_thread)
    atexit.register(_stop_player_thread)

    _filter = _ButtonSoundFilter(app)
    app.installEventFilter(_filter)


def _stop_player_thread():
    if _playerThread is not None and _playerThread.isRunning():
        _playerThread.quit()
        _playerThread.wait(1000)


def play(name):
    #safe from any thread, the sound thread does the actual playing
    global _windowSoundCount
    if name == "window":
        _windowSoundCount += 1
    if _player is not None:
        _player.playRequested.emit(name)


def _play_click(button):
    #Waits until the button's own clicked handlers have all finished, then
    #plays only if none of them switched screens (window sound requested since
    #the press), so the click doesn't pile on top of the window sound. Counted
    #from the press since some of those handlers may run before this one.
    countAtPress = button.property("_windowSoundCountAtPress")
    QTimer.singleShot(0, lambda: _windowSoundCount == countAtPress and play("click"))


class _ButtonSoundFilter(QObject):
    #Watches every button in the app, so new buttons get the click sound with
    #no per-window wiring, via the button's own clicked signal (which also
    #covers keyboard activation, and not a press that's dragged off the
    #button before release)

    def eventFilter(self, obj, event):
        if isinstance(obj, QAbstractButton) and event.type() == QEvent.Type.Polish:
            self._hook_click(obj)
        return False

    def _hook_click(self, button):
        if button.property("_clickSoundHooked"):
            return
        button.setProperty("_clickSoundHooked", True)
        #InstantPopup tool buttons (the settings gear) open their menu on
        #press and never emit clicked
        if isinstance(button, QToolButton) and button.popupMode() == QToolButton.ToolButtonPopupMode.InstantPopup:
            #(a menu, never a screen change)
            button.pressed.connect(lambda: play("click"))
        else:
            button.pressed.connect(lambda: button.setProperty("_windowSoundCountAtPress", _windowSoundCount))
            button.clicked.connect(lambda: _play_click(button))
