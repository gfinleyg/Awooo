"""
ClassAntLogger.py

Single, unified event log for everything ANT/device-related: connection
lifecycle, device discovery, auto/manual device selection, connect/
disconnect, and every RecoveryManager/ANTManager event (channel retries,
device resets, stale/lost/stalled devices, and now every ANT response
event seen - not just the ones that trigger a recovery action).

This replaces the scattered print()-to-console statements that used to
come from ant/manager.py, ant/recovery.py and ClassAntDevices.py - nothing
in the ANT stack should be writing to stdout any more (AntWorkout.py is
launched without a visible console on the user's machine, so those prints
were invisible anyway; they just meant there was no record of what
happened once the window closed).

Rotation: appends are cheap (one open/write/close under a lock), but once
the file passes MAX_LOG_BYTES, the whole file is read back in and
rewritten with its oldest TRIM_LINES lines dropped. That read-modify-write
is O(file size), so it isn't free - but it only runs once every ~50 lines
of growth once the cap is reached, and the file is capped at ~1MB, so each
rewrite is a bounded, sub-second disk operation even on modest hardware.

A standard alternative worth knowing about: Python's own
logging.handlers.RotatingFileHandler(maxBytes=..., backupCount=...) does
the equivalent job without ever rewriting the live file - when the current
file hits the size cap, it's renamed to a numbered backup (log.txt.1, .2,
...) and a fresh empty file is opened. That's the more standard approach
for anything long-running or high-volume, since it's O(1) per rotation
instead of O(file size). The trade-off is exactly what this module avoids:
"Show Logs" would need to read and stitch together the current file plus
however many backups to show a continuous history, and "Clear Logs" would
need to delete all of them, not just one file. Given this app's logging
volume (event-driven, not per-broadcast-packet) and the simplicity wanted
for the Show/Clear Logs menu, the single-file trim-in-place approach here
is a reasonable fit - but if logging volume grows a lot (e.g. someone
wants every raw broadcast logged, not just events), switching to
RotatingFileHandler would be the first thing to reach for.
"""

import os
import threading
from datetime import datetime

LOG_FILE = "antWorkoutLog.txt"
#UTF-8 whatever the system's codepage, so any text (song titles, file paths) can be logged; reading never fails on
#what an older version wrote in the codepage either, it's just shown as replacement characters
LOG_ENCODING = "utf-8"

MAX_LOG_BYTES = 1_000_000  # 1MB
TRIM_LINES = 50

_lock = threading.Lock()


def log_event(category, message):
    """Append one line to the unified log: "<timestamp> [<category>]
    <message>". Safe to call from any thread (the ANT message-pump
    thread, the recovery watchdog thread, or the Qt UI thread) - a single
    lock serializes all writers. Never raises: a logging failure (e.g.
    the working directory briefly not writable) shouldn't be able to take
    down the ANT connection or the UI.
    """

    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    line = f"{timestamp} [{category}] {message}\n"

    with _lock:
        try:
            with open(LOG_FILE, "a", encoding=LOG_ENCODING, errors="replace") as f:
                f.write(line)
        except OSError:
            return

        _rotate_if_needed()


def _rotate_if_needed():
    #Caller already holds _lock.
    try:
        if os.path.getsize(LOG_FILE) <= MAX_LOG_BYTES:
            return

        with open(LOG_FILE, "r", encoding=LOG_ENCODING, errors="replace") as f:
            lines = f.readlines()

        with open(LOG_FILE, "w", encoding=LOG_ENCODING) as f:
            f.writelines(lines[TRIM_LINES:])

    except OSError:
        pass


def clear_log():
    """Empty the log file (leaves a single marker line rather than a
    silently-empty file, so "Show Logs" right after "Clear Logs" reads as
    intentional rather than broken).
    """
    with _lock:
        try:
            with open(LOG_FILE, "w", encoding=LOG_ENCODING) as f:
                timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                f.write(f"{timestamp} [LOG] Log cleared\n")
        except OSError:
            pass


def read_log(max_lines=500):
    """Return up to the last `max_lines` lines of the log as a single
    string, oldest of that window first. Capped rather than returning the
    whole (up to ~1MB) file, since ShowLogsWindow re-reads this on a
    timer and a smaller read keeps that responsive regardless of how
    close the file is to its rotation size.
    """
    try:
        with open(LOG_FILE, "r", encoding=LOG_ENCODING, errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return ""

    return "".join(lines[-max_lines:])

def install_error_logging():
    """Route Python errors that would otherwise only reach stderr into the
    log, since AntWorkout.py runs without a visible console. Covers
    uncaught exceptions on the main thread (including ones raised inside
    Qt slots, which PyQt hands to sys.excepthook), in worker threads,
    and "exception ignored" errors from __del__/callbacks, plus Qt's own
    warning/critical/fatal messages. Each hook still passes the error on
    to the original handler so a console, when there is one, shows it too.
    """
    import sys
    import traceback

    def log_exception(exc_type, exc_value, exc_tb, where):
        details = "".join(traceback.format_exception(exc_type, exc_value, exc_tb)).rstrip()
        log_event("ERROR", f"Unhandled exception{where}:\n{details}")

    previous_excepthook = sys.excepthook

    def excepthook(exc_type, exc_value, exc_tb):
        log_exception(exc_type, exc_value, exc_tb, "")
        previous_excepthook(exc_type, exc_value, exc_tb)

    sys.excepthook = excepthook

    previous_thread_excepthook = threading.excepthook

    def thread_excepthook(args):
        log_exception(args.exc_type, args.exc_value, args.exc_traceback,
                      f" in thread {args.thread.name if args.thread else '?'}")
        previous_thread_excepthook(args)

    threading.excepthook = thread_excepthook

    previous_unraisablehook = sys.unraisablehook

    def unraisablehook(unraisable):
        where = f" ({unraisable.err_msg or 'Exception ignored'}: {unraisable.object!r})"
        log_exception(unraisable.exc_type, unraisable.exc_value, unraisable.exc_traceback, where)
        previous_unraisablehook(unraisable)

    sys.unraisablehook = unraisablehook

    try:
        from PyQt6.QtCore import QtMsgType, qInstallMessageHandler
    except ImportError:
        return

    qt_levels = {QtMsgType.QtWarningMsg: "Qt warning",
                 QtMsgType.QtCriticalMsg: "Qt critical",
                 QtMsgType.QtFatalMsg: "Qt fatal"}
    #harmless Qt messages, dropped entirely. No media backend only affects QMediaPlayer and
    #friends; QSoundEffect (SoundEffects.py) still works and music goes through miniaudio
    qt_ignored = ("No QtMultimedia backends found",)

    def qt_message_handler(msg_type, context, message):
        if message.startswith(qt_ignored):
            return
        level = qt_levels.get(msg_type)
        if level is not None:
            location = f" ({context.file}:{context.line})" if context.file else ""
            log_event("ERROR", f"{level}{location}: {message}")
        if sys.__stderr__ is not None:
            sys.__stderr__.write(message + "\n")

    qInstallMessageHandler(qt_message_handler)
