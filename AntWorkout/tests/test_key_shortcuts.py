"""KeyShortcuts: keys pressing buttons, which keys a focused editor keeps, Tab cycling and the key labels.
Real widgets on the offscreen platform, real key events through the app wide event filter"""

import pytest
from PyQt6.QtCore import QEvent, Qt
from PyQt6.QtGui import QKeyEvent
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import (QApplication, QLabel, QLineEdit, QListWidget, QMainWindow, QMessageBox, QPushButton,
                             QSlider, QVBoxLayout, QWidget)

import KeyShortcuts as K


@pytest.fixture
def app(qapp):
    K.install(qapp)
    K._bindings.clear()
    yield qapp
    K.unpin_labels()
    K._manager.hide_labels()
    K._bindings.clear()
    for window in QApplication.topLevelWidgets():
        window.close()
        window.deleteLater()
    QApplication.processEvents()


class Window(QMainWindow):
    made = []

    def __init__(self, *names):
        super().__init__()
        Window.made.append(self)
        central = QWidget()
        self.setCentralWidget(central)
        self.box = QVBoxLayout(central)
        self.buttons = {}
        self.clicks = []
        for name in names:
            button = QPushButton(name)
            button.clicked.connect(lambda _=False, name=name: self.clicks.append(name))
            self.box.addWidget(button)
            self.buttons[name] = button

    def add(self, widget):
        self.box.addWidget(widget)
        return widget


def shown(window):
    window.resize(400, 300)
    window.show()
    window.activateWindow()
    QTest.qWaitForWindowExposed(window)
    QTest.qWaitForWindowActive(window)
    QApplication.processEvents()
    return window


def press(widget, key, modifiers=Qt.KeyboardModifier.NoModifier, autorepeat=False):
    """A key press as the app gets it: at the focus widget, else the window"""
    target = QApplication.focusWidget() or widget.window()
    QApplication.sendEvent(target, QKeyEvent(QEvent.Type.KeyPress, key, modifiers, "", autorepeat))


def test_key_presses_its_button(app):
    window = Window("Start", "Stop")
    K.add(window.buttons["Start"], Qt.Key.Key_S, label="S")
    K.add(window.buttons["Stop"], (Qt.Key.Key_X, Qt.Key.Key_Q), label="X")
    shown(window)
    press(window, Qt.Key.Key_S)
    press(window, Qt.Key.Key_Q)
    press(window, Qt.Key.Key_Z)     #not bound
    assert window.clicks == ["Start", "Stop"]


def test_hidden_or_disabled_buttons_do_nothing(app):
    window = Window("A", "B")
    K.add(window.buttons["A"], Qt.Key.Key_A)
    K.add(window.buttons["B"], Qt.Key.Key_B)
    shown(window)
    window.buttons["A"].hide()
    window.buttons["B"].setEnabled(False)
    press(window, Qt.Key.Key_A)
    press(window, Qt.Key.Key_B)
    assert window.clicks == []


def test_action_and_condition(app):
    window = Window("Go")
    ran = []
    state = {"on": False}
    K.add(window.buttons["Go"], Qt.Key.Key_Space, when=lambda: not state["on"])
    K.add(window.buttons["Go"], Qt.Key.Key_Space, when=lambda: state["on"], action=lambda: ran.append("paused"))
    shown(window)
    press(window, Qt.Key.Key_Space)
    state["on"] = True
    press(window, Qt.Key.Key_Space)
    assert window.clicks == ["Go"] and ran == ["paused"]


def test_ctrl_and_alt_keep_their_meaning(app):
    window = Window("Copy")
    K.add(window.buttons["Copy"], Qt.Key.Key_C)
    shown(window)
    press(window, Qt.Key.Key_C, Qt.KeyboardModifier.ControlModifier)
    press(window, Qt.Key.Key_C, Qt.KeyboardModifier.AltModifier)
    assert window.clicks == []
    #Shift is fine
    press(window, Qt.Key.Key_C, Qt.KeyboardModifier.ShiftModifier)
    assert window.clicks == ["Copy"]


def test_held_keys_repeat_only_when_asked(app):
    window = Window("Once", "Again")
    K.add(window.buttons["Once"], Qt.Key.Key_O)
    K.add(window.buttons["Again"], Qt.Key.Key_A, repeat=True)
    shown(window)
    for _ in range(3):
        press(window, Qt.Key.Key_O, autorepeat=True)
        press(window, Qt.Key.Key_A, autorepeat=True)
    assert window.clicks == ["Again"]*3


def test_focused_text_box_keeps_its_keys(app):
    window = Window("Set FTP", "Enter anywhere", "Letter")
    field = window.add(QLineEdit())
    K.add(window.buttons["Set FTP"], K.ENTER, field=field)
    K.add(window.buttons["Enter anywhere"], K.ENTER)
    K.add(window.buttons["Letter"], (Qt.Key.Key_L, Qt.Key.Key_5, Qt.Key.Key_Delete))
    shown(window)
    field.setFocus()
    QApplication.processEvents()
    assert QApplication.focusWidget() is field
    #digits and Delete stay with the box, letters still go to the buttons
    QTest.keyClick(field, Qt.Key.Key_5)
    QTest.keyClick(field, Qt.Key.Key_Delete)
    QTest.keyClick(field, Qt.Key.Key_L)
    assert window.clicks == ["Letter"] and field.text() == "5"
    #Enter bound to the box presses its button
    QTest.keyClick(field, Qt.Key.Key_Return)
    assert window.clicks == ["Letter", "Set FTP"]
    #away from the box, the box's Enter is someone else's
    field.clearFocus()
    QApplication.processEvents()
    press(window, Qt.Key.Key_Enter)
    assert window.clicks[-1] == "Enter anywhere"


def test_focused_list_keeps_arrows_and_space_but_not_delete(app):
    window = Window("Remove", "Toggle")
    songs = window.add(QListWidget())
    songs.addItems(["one", "two"])
    K.add(window.buttons["Remove"], Qt.Key.Key_Delete)
    K.add(window.buttons["Toggle"], Qt.Key.Key_Space)
    shown(window)
    songs.setFocus()
    QApplication.processEvents()
    QTest.keyClick(songs, Qt.Key.Key_Space)
    QTest.keyClick(songs, Qt.Key.Key_Delete)
    assert window.clicks == ["Remove"]


def test_focused_slider_keeps_its_arrows(app):
    window = Window("Up")
    slider = window.add(QSlider(Qt.Orientation.Horizontal))
    K.add(window.buttons["Up"], Qt.Key.Key_Right)
    shown(window)
    slider.setFocus()
    QApplication.processEvents()
    QTest.keyClick(slider, Qt.Key.Key_Right)
    assert window.clicks == [] and slider.value() == 1


def test_keys_belong_to_their_window(app):
    first, second = Window("First"), Window("Second")
    K.add(first.buttons["First"], Qt.Key.Key_F)
    K.add(second.buttons["Second"], Qt.Key.Key_F)
    shown(first)
    shown(second)
    press(second, Qt.Key.Key_F)
    assert first.clicks == [] and second.clicks == ["Second"]


def test_deleted_widgets_are_forgotten(app):
    window = Window("Gone", "Kept")
    K.add(window.buttons["Gone"], Qt.Key.Key_G)
    K.add(window.buttons["Kept"], Qt.Key.Key_K)
    shown(window)
    window.buttons["Gone"].deleteLater()
    QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete.value)
    QApplication.processEvents()
    press(window, Qt.Key.Key_G)
    assert len(K._bindings) == 1


def test_tab_cycles_just_the_given_boxes(app):
    window = Window("Anchor")
    boxes = [window.add(QLineEdit(str(i))) for i in range(3)]
    other = window.add(QLineEdit("not in the cycle"))
    K.tab_cycle(boxes, window.buttons["Anchor"])
    shown(window)
    other.setFocus()
    QApplication.processEvents()
    order = []
    for _ in range(4):
        press(window, Qt.Key.Key_Tab)
        order.append(QApplication.focusWidget())
    assert order == [boxes[0], boxes[1], boxes[2], boxes[0]]
    press(window, Qt.Key.Key_Backtab)
    assert QApplication.focusWidget() is boxes[2]
    #hidden ones are skipped
    boxes[0].hide()
    press(window, Qt.Key.Key_Tab)
    assert QApplication.focusWidget() is boxes[1]


def test_step_list(app):
    songs = QListWidget()
    K.step_list(songs, 1)       #empty: nothing
    songs.addItems(["a", "b", "c"])
    K.step_list(songs, -1)
    assert songs.currentRow() == 0
    K.step_list(songs, 1)
    K.step_list(songs, 5)
    assert songs.currentRow() == 2
    K.step_list(songs, -1)
    assert songs.currentRow() == 1


def test_buttons_never_take_focus(app):
    window = Window("Plain")
    shown(window)
    assert window.buttons["Plain"].focusPolicy() == Qt.FocusPolicy.NoFocus
    box = QMessageBox(QMessageBox.Icon.Question, "Sure?", "Really?",
                      QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
    box.show()
    QApplication.processEvents()
    assert box.button(QMessageBox.StandardButton.Yes).focusPolicy() != Qt.FocusPolicy.NoFocus
    assert K._KeyManager.in_standard_dialog(box.button(QMessageBox.StandardButton.Yes))
    assert not K._KeyManager.in_standard_dialog(window.buttons["Plain"])
    box.close()


###Labels
def labels_on(window):
    return {label.text(): label for label in window.findChildren(QLabel, "keyLabel") if label.isVisible()}


def test_pinned_labels(app):
    window = Window("Corner", "Left", "Inside", "Hidden", "Disabled", "Quiet")
    K.add(window.buttons["Corner"], Qt.Key.Key_C, label="C")
    K.add(window.buttons["Left"], Qt.Key.Key_L, label="L", place="left")
    K.add(window.buttons["Inside"], Qt.Key.Key_I, label="I", place="inside")
    K.add(window.buttons["Hidden"], Qt.Key.Key_H, label="H")
    K.add(window.buttons["Disabled"], Qt.Key.Key_D, label="D")
    K.add(window.buttons["Quiet"], Qt.Key.Key_Q)
    K.add(window.buttons["Corner"], label="Esc")   #a label for a key handled elsewhere
    shown(window)
    window.buttons["Hidden"].hide()
    window.buttons["Disabled"].setEnabled(False)
    QApplication.processEvents()
    K.pin_labels(window)
    labels = labels_on(window)
    assert set(labels) == {"C", "L", "I", "D", "Esc"}
    assert K.LABEL_DISABLED_COLORS[0] in labels["D"].styleSheet()
    assert K.LABEL_COLORS[0] in labels["C"].styleSheet()
    #every label inside the window
    for label in labels.values():
        assert 0 <= label.x() <= window.width() - label.width() and 0 <= label.y() <= window.height() - label.height()
    #nothing changed: the same labels are kept
    before = K._pinned.labels
    K.pin_labels(window)
    assert K._pinned.labels is before
    window.buttons["Hidden"].show()
    K.pin_labels(window)
    assert "H" in labels_on(window)
    K.unpin_labels()
    QApplication.processEvents()
    assert labels_on(window) == {}


def test_holding_ctrl_shows_labels(app, monkeypatch):
    window = Window("Go")
    K.add(window.buttons["Go"], Qt.Key.Key_G, label="G")
    shown(window)
    monkeypatch.setattr(K.QApplication, "queryKeyboardModifiers",
                        staticmethod(lambda: Qt.KeyboardModifier.ControlModifier))
    monkeypatch.setattr(K.QApplication, "activeWindow", staticmethod(lambda: window))
    press(window, Qt.Key.Key_Control)
    assert K._manager.revealTimer.isActive()
    K._manager.revealTimer.stop()
    K._manager.show_labels()
    assert set(labels_on(window)) == {"G"}
    #any other key hides them
    press(window, Qt.Key.Key_G, Qt.KeyboardModifier.ControlModifier)
    QApplication.processEvents()
    assert labels_on(window) == {}


def test_ctrl_released_early_shows_nothing(app, monkeypatch):
    window = Window("Go")
    K.add(window.buttons["Go"], Qt.Key.Key_G, label="G")
    shown(window)
    monkeypatch.setattr(K.QApplication, "queryKeyboardModifiers", staticmethod(lambda: Qt.KeyboardModifier.NoModifier))
    K._manager.show_labels()
    assert labels_on(window) == {}
