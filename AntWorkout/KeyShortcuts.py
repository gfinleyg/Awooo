"""
KeyShortcuts.py

A key for every button, and holding Ctrl shows them: a small key label on each button of the active window.

  add(widget, keys, label=..., ...)   - the key(s) press the button (or run action) while it's showing
  tab_cycle(fields, anchor)           - Tab/Shift+Tab move between just these text boxes
  install(app)                        - once, from AntWorkout.py
  pin_labels(window)/unpin_labels()   - every label on window, kept up (LayoutViewer's Scenario tab)

How keys are shared out:
  - Ctrl (or Alt) held: the key isn't one of these (Ctrl+C, Ctrl+V... keep their meaning). Ctrl on its own, held a
    moment, shows the labels
  - letters always go to the buttons, a text box focused or not (every text box in the app takes numbers only)
  - the keys a focused editor uses itself (digits, Space, Enter, Del, the arrows...) stay with it, unless the key
    is bound to that editor (field=, e.g. Enter in the FTP box presses Set new FTP)
  - a key whose button is hidden or disabled does nothing (its label shows greyed out)

Buttons never take keyboard focus (so Space/Enter can't press whichever one was last clicked), and closing one of
the popup windows gives the main window its keys back.
"""

from types import SimpleNamespace

from PyQt6 import sip
from PyQt6.QtCore import QEvent, QObject, QPoint, Qt, QTimer
from PyQt6.QtWidgets import (QAbstractItemView, QAbstractSlider, QAbstractSpinBox, QApplication, QFileDialog, QLabel,
                             QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit, QPushButton, QTextEdit, QToolButton,
                             QWidget)

#Ctrl held this long on its own shows the labels (a Ctrl+C on the way doesn't flash them)
REVEAL_DELAY_MS = 300

ENTER = (Qt.Key.Key_Return, Qt.Key.Key_Enter)

#the keys a focused editor keeps for itself
_DIGITS = {Qt.Key(Qt.Key.Key_0 + i) for i in range(10)}
_MOVE_KEYS = {Qt.Key.Key_Left, Qt.Key.Key_Right, Qt.Key.Key_Up, Qt.Key.Key_Down, Qt.Key.Key_Home, Qt.Key.Key_End,
              Qt.Key.Key_PageUp, Qt.Key.Key_PageDown}
_TEXT_KEYS = _DIGITS | _MOVE_KEYS | set(ENTER) | {Qt.Key.Key_Space, Qt.Key.Key_Delete, Qt.Key.Key_Backspace}
_LIST_KEYS = _MOVE_KEYS | {Qt.Key.Key_Space}

#the look of the labels: the app's light blue boxes (themes recolor them, see AppTheme.install)
LABEL_STYLE = """#keyLabel {background-color: %s; color: %s; border: 2px solid %s; border-radius: 8px;
                            padding: 0px 6px; font: 14pt 'Segoe UI Black';}"""
LABEL_COLORS = ("#bdd6de", "black", "black")
LABEL_DISABLED_COLORS = ("#dadbde", "#808080", "#808080")


def _editor_keys(widget):
    if isinstance(widget, (QLineEdit, QAbstractSpinBox, QPlainTextEdit, QTextEdit)):
        return _TEXT_KEYS
    if isinstance(widget, QAbstractItemView):
        return _LIST_KEYS
    if isinstance(widget, QAbstractSlider):
        return _MOVE_KEYS
    return ()


class _Binding:
    def __init__(self, widget, keys, label, action, when, field, place, repeat):
        self.widget = widget
        self.keys = keys
        self.label = label
        self.action = action
        self.when = when
        self.field = field
        self.place = place
        self.repeat = repeat

    def deleted(self):
        return sip.isdeleted(self.widget) or (self.field is not None and sip.isdeleted(self.field))

    def shown(self):
        #on screen, and its condition (if any) holds
        return self.widget.isVisible() and (self.when is None or self.when())

    def usable(self):
        return self.shown() and self.widget.isEnabled()

    def trigger(self):
        if self.action is not None:
            self.action()
        else:
            self.widget.click()


_bindings = []
_manager = None


def add(widget, keys=(), label=None, action=None, when=None, field=None, place="corner", repeat=False):
    """keys (one or a tuple) press widget, a button, or run action instead, while widget is showing on the active
    window and when() (if given) is true. label is what holding Ctrl shows on widget (None: nothing; keys=() with a
    label just shows it, for a key handled elsewhere such as Esc). field: only while that editor has focus.
    place: "corner" (top right), "left" (beside it) or "inside" (inside its top right). repeat: held keys repeat"""
    if not isinstance(keys, (tuple, list)):
        keys = (keys,)
    _bindings.append(_Binding(widget, set(keys), label, action, when, field, place, repeat))


def tab_cycle(fields, anchor):
    """Tab/Shift+Tab move around just these text boxes (the showing ones), wrapping; from anywhere else on the
    window Tab goes to the first. The Tab label shows on anchor"""
    def step(forward):
        shown = [f for f in fields if f.isVisible() and f.isEnabled()]
        if not shown:
            return
        focus = QApplication.focusWidget()
        if focus in shown:
            index = (shown.index(focus) + (1 if forward else -1)) % len(shown)
        else:
            index = 0 if forward else -1
        #(a tab focus selects the box's text, ready to type over)
        shown[index].setFocus(Qt.FocusReason.TabFocusReason if forward else Qt.FocusReason.BacktabFocusReason)
    add(anchor, Qt.Key.Key_Tab, label="Tab", action=lambda: step(True))
    add(anchor, Qt.Key.Key_Backtab, action=lambda: step(False))


def step_list(listWidget, step):
    """Up/Down for a list that doesn't have focus: the next/previous row becomes the current one"""
    count = listWidget.count()
    if count:
        row = listWidget.currentRow()
        listWidget.setCurrentRow(max(0, min(row + step, count - 1)) if row >= 0 else 0)


def install(app):
    global _manager
    if _manager is None:
        _manager = _KeyManager(app)
        app.installEventFilter(_manager)


def _live_bindings(window):
    _bindings[:] = [b for b in _bindings if not b.deleted()]
    return [b for b in _bindings if b.widget.window() is window]


class _KeyManager(QObject):
    def __init__(self, parent):
        super().__init__(parent)
        self.labels = []
        self.revealTimer = QTimer(self)
        self.revealTimer.setSingleShot(True)
        self.revealTimer.setInterval(REVEAL_DELAY_MS)
        self.revealTimer.timeout.connect(self.show_labels)

    def eventFilter(self, obj, event):
        kind = event.type()
        if kind == QEvent.Type.KeyPress:
            return self.key_press(obj, event)
        if kind == QEvent.Type.KeyRelease:
            if event.key() == Qt.Key.Key_Control and not event.isAutoRepeat():
                self.hide_labels()
        elif kind in (QEvent.Type.MouseButtonPress, QEvent.Type.WindowDeactivate):
            self.hide_labels()
        elif kind == QEvent.Type.ApplicationStateChange:
            #switched to another app with Ctrl down (its release never comes here)
            self.hide_labels()
        elif kind == QEvent.Type.Polish and isinstance(obj, (QPushButton, QToolButton)):
            #(the standard message boxes keep theirs, they're driven by Tab/Enter)
            if not self.in_standard_dialog(obj):
                obj.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        elif (kind == QEvent.Type.Hide and isinstance(obj, QWidget) and obj.isWindow() and not event.spontaneous()
              and not isinstance(obj, QMainWindow)
              and obj.windowType() not in (Qt.WindowType.Popup, Qt.WindowType.ToolTip)):
            #a popup window closed: Windows doesn't always hand the keys back to the main window
            QTimer.singleShot(0, self.activate_main_window)
        return False

    @staticmethod
    def in_standard_dialog(widget):
        while widget is not None:
            if isinstance(widget, (QMessageBox, QFileDialog)):
                return True
            widget = widget.parentWidget()
        return False

    @staticmethod
    def activate_main_window():
        if QApplication.activeWindow() is not None:
            return
        for window in QApplication.topLevelWidgets():
            if isinstance(window, QMainWindow) and window.isVisible() and not window.isMinimized():
                window.activateWindow()
                return

    def key_press(self, obj, event):
        #key events reach here once per widget they pass through (and for the QWindow): only the first, at the focus
        #widget (or the window, with nothing focused)
        if not isinstance(obj, QWidget):
            return False
        key = event.key()
        if key != Qt.Key.Key_Control:
            self.hide_labels()
        focus = QApplication.focusWidget()
        if obj is not focus and not (focus is None and obj.isWindow()):
            return False
        if key == Qt.Key.Key_Control:
            if not event.isAutoRepeat() and not self.labels:
                self.revealTimer.start()
            return False
        if QApplication.activePopupWidget() is not None:
            #a menu is open (the ⚙ menu has its own letters)
            return False
        if event.modifiers() & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier
                                | Qt.KeyboardModifier.MetaModifier):
            return False
        editorKeys = _editor_keys(focus)
        for binding in _live_bindings(obj.window()):
            if key not in binding.keys:
                continue
            if binding.field is not None:
                if focus is not binding.field:
                    continue
            elif key in editorKeys:
                continue
            if not binding.usable():
                continue
            if not (event.isAutoRepeat() and not binding.repeat):
                binding.trigger()
            return True
        return False

    def show_labels(self):
        #still held, and only Ctrl
        if QApplication.queryKeyboardModifiers() != Qt.KeyboardModifier.ControlModifier:
            return
        window = QApplication.activeWindow()
        if window is None or QApplication.activePopupWidget() is not None:
            return
        self.hide_labels()
        self.labels = [_make_label(binding, window) for binding in _labeled(window)]

    def hide_labels(self):
        self.revealTimer.stop()
        _remove_labels(self.labels)
        self.labels = []


def _labeled(window):
    return [binding for binding in _live_bindings(window) if binding.label and binding.shown()]


def _make_label(binding, window):
    label = QLabel(binding.label, window)
    label.setObjectName("keyLabel")
    label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
    label.setStyleSheet(LABEL_STYLE % (LABEL_COLORS if binding.widget.isEnabled() else LABEL_DISABLED_COLORS))
    label.adjustSize()
    widget = binding.widget
    topLeft = widget.mapTo(window, QPoint(0, 0))
    width, height = label.width(), label.height()
    if binding.place == "left":
        x, y = topLeft.x() - width - 4, topLeft.y() + (widget.height() - height)//2
    elif binding.place == "inside":
        x, y = topLeft.x() + widget.width() - width - 8, topLeft.y() + 8
    else:
        #over the top right corner, a little outside it so the button's text stays readable
        x, y = topLeft.x() + widget.width() - width + 6, topLeft.y() - 6
    label.move(max(0, min(x, window.width() - width)), max(0, min(y, window.height() - height)))
    label.show()
    label.raise_()
    return label


def _remove_labels(labels):
    for label in labels:
        if not sip.isdeleted(label):
            label.hide()
            label.deleteLater()


_pinned = SimpleNamespace(labels=[], state=None)


def pin_labels(window):
    """Every key label on window, as holding Ctrl shows them, left up until unpin_labels() (LayoutViewer's
    Scenario tab). Call again after the window changes: the labels are only rebuilt if what they'd show (or where)
    has changed"""
    bindings = _labeled(window)
    state = [(binding, binding.widget.isEnabled(), binding.widget.mapTo(window, QPoint(0, 0)), binding.widget.size())
             for binding in bindings] + [window.size()]
    if state == _pinned.state and not any(sip.isdeleted(label) for label in _pinned.labels):
        return
    _remove_labels(_pinned.labels)
    _pinned.labels = [_make_label(binding, window) for binding in bindings]
    _pinned.state = state


def unpin_labels():
    _remove_labels(_pinned.labels)
    _pinned.labels, _pinned.state = [], None
