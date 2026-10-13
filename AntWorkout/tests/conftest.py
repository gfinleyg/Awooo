"""
Shared setup for the unit tests (run from the AntWorkout folder: py -3.x -m pytest tests).

No ANT stick or SimulANT+ is needed: the ANT layer is driven through tests/fakes.py's FakeNative, and every Qt
widget is built on the offscreen platform. Each test runs in its own empty working folder, so the settings, log and
cache files the app writes next to itself never touch the real ones.
"""

import copy
import os
import sys

APP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_DIR = os.path.dirname(APP_DIR)
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
for path in (os.path.join(APP_DIR, "AntNative", "python"), APP_DIR, TESTS_DIR):
    if path not in sys.path:
        sys.path.insert(0, path)

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

#ant_native.pyd has to load before PyQt6 (see AntWorkout.py's import order guard)
import ant.manager  # noqa: E402,F401

import pytest  # noqa: E402
from PyQt6.QtWidgets import QApplication  # noqa: E402


def pytest_configure(config):
    config.addinivalue_line("markers", "whole_app: builds the app's real screens (test_screens.py, test_app_flow.py)")


def pytest_collection_modifyitems(config, items):
    """The whole_app tests go last: building the real main window leaves app wide Qt state behind (AppTheme's
    hooks, AppLayout's window, popups owned by deleted screens) that the app itself never has to clean up, as it
    has the one main window until it exits. Smaller Qt tests run after them can crash on it"""
    items.sort(key=lambda item: item.get_closest_marker("whole_app") is not None)


_exitStatus = 0


def pytest_sessionfinish(session, exitstatus):
    global _exitStatus
    _exitStatus = int(exitstatus)


def pytest_unconfigure(config):
    """Out without Python's own teardown once the results are in, as AntWorkout.py does: destroying the Qt objects
    in whatever order it gets to them can crash Qt6Core on the way out (an access violation after every test
    passed)"""
    if _app is not None:
        for stream in (sys.stdout, sys.stderr):
            if stream is not None:
                stream.flush()
        os._exit(_exitStatus)


#the one QApplication, kept to the end: freed before the exit above, it takes the app's widgets down with it
_app = None


@pytest.fixture(scope="session")
def qapp():
    global _app
    _app = QApplication.instance() or QApplication([])
    yield _app


def _snapshot_class(cls):
    saved = {}
    for name, value in vars(cls).items():
        if name.startswith("__") or callable(value) or isinstance(value, (staticmethod, classmethod)):
            continue
        try:
            saved[name] = copy.deepcopy(value)
        except Exception:
            saved[name] = value
    return saved


@pytest.fixture(autouse=True)
def qt_slot_errors(monkeypatch):
    """An exception in a Qt slot or event filter fails the test it happened in (PyQt would otherwise end the whole
    run: with Python's own excepthook in place it aborts the process)"""
    import traceback
    errors = []
    monkeypatch.setattr(sys, "excepthook", lambda *info: errors.append("".join(traceback.format_exception(*info))))
    yield errors
    if errors:
        pytest.fail("Exception in a Qt slot:\n" + "\n".join(errors))


@pytest.fixture(autouse=True)
def isolated_app_state(tmp_path, monkeypatch):
    """Every test: an empty working folder, and the app's module-level state put back afterwards"""
    monkeypatch.chdir(tmp_path)
    from AppState import AppState
    import Performance
    import Units
    import WorkoutHelpers
    appState = _snapshot_class(AppState)
    units = _snapshot_class(Units._Units)
    unitsCurrent = dict(vars(Units.current))
    selected = list(WorkoutHelpers._selectedDeviceLines)
    gaugeFps = Performance._gaugeFps
    yield
    for name, value in appState.items():
        current = getattr(AppState, name, None)
        #lists are changed in place elsewhere (AppState.ftpRanges[:] = ...), keep the same object
        if isinstance(current, list) and isinstance(value, list):
            current[:] = value
        else:
            setattr(AppState, name, value)
    for name, value in units.items():
        setattr(Units._Units, name, value)
    vars(Units.current).clear()
    vars(Units.current).update(unitsCurrent)
    Units._watchers.clear()
    WorkoutHelpers._selectedDeviceLines[:] = selected
    Performance._gaugeFps = gaugeFps
    Performance._watchers.clear()
