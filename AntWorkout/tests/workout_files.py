"""
Two ways of reading .awo lines, for the tests:

  verify(lines)  - awo-verifier.py's verdict: the errors it prints (empty if it finds the file fine)
  WorkoutParser  - the app's own reader (WorkoutSetupWindow.process_workout_lines and the checks it calls), on a
                   stand-in for the setup screen: rejections are recorded instead of shown
"""

import contextlib
import importlib.util
import io
import os
import tempfile

from conftest import APP_DIR

from AppState import AppState


def load_verifier():
    """awo-verifier.py as a module. It checks every .awo file in the working folder when loaded, so that's done from
    an empty one"""
    previous = os.getcwd()
    with tempfile.TemporaryDirectory() as empty:
        os.chdir(empty)
        try:
            spec = importlib.util.spec_from_file_location("awo_verifier", os.path.join(APP_DIR, "awo-verifier.py"))
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        finally:
            os.chdir(previous)
    return module


_verifier = None


def verify_file(path):
    global _verifier
    _verifier = _verifier or load_verifier()
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        _verifier.processAwo(path)
    return out.getvalue().strip()


def verify(lines):
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "test.awo")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        return verify_file(path)


class _Label:
    def __init__(self):
        self.ui = type("ui", (), {})()
        self.ui.widget = type("widget", (), {})()


class WorkoutParser:
    """The setup screen's workout reading, without the screen"""

    def __init__(self, ftp=200, maxBlockSeconds=1800):
        from ClassWorkoutSetupWindow import WorkoutSetupWindow
        AppState.ftp = str(ftp)
        self.maxBlockSeconds = maxBlockSeconds
        self.previewing = True
        self.workoutDisplayLabel = _Label()
        self.rejections = []
        for name in ("process_workout_file", "process_workout_lines", "clear_workout", "process_ramp", "ramp_block", "single_block_interval",
                     "split_message_fields", "parse_block_messages", "add_block_messages", "duration_checker",
                     "power_value", "watts_at", "position_checker", "valid_free_ride", "valid_steady_state",
                     "valid_ramp", "valid_interval"):
            setattr(self, name, getattr(WorkoutSetupWindow, name).__get__(self))
        self.watts_at = WorkoutSetupWindow.watts_at

    def reject_workout(self, filename, reason, lnNum=None, line=None):
        self.rejections.append((reason, lnNum, line))

    def parse(self, lines, filename="test.awo"):
        self.rejections = []
        ok = self.process_workout_lines(list(lines), filename)
        return ok
