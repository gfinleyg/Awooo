import os
import sys
import threading

import pytest

import ClassAntLogger as L


def test_log_and_read():
    L.log_event("ANT", "hello")
    L.log_event("SCAN", "Ünïcödé song – title")
    text = L.read_log()
    lines = text.splitlines()
    assert lines[0].endswith("[ANT] hello") and lines[1].endswith("[SCAN] Ünïcödé song – title")
    assert L.read_log(max_lines=1) == lines[1] + "\n"


def test_read_without_a_log():
    assert L.read_log() == ""


def test_clear_leaves_a_marker():
    L.log_event("ANT", "x")
    L.clear_log()
    assert L.read_log().strip().endswith("[LOG] Log cleared")


def test_old_codepage_text_still_reads():
    with open(L.LOG_FILE, "wb") as f:
        f.write(b"caf\xe9\n")
    assert L.read_log().startswith("caf")


def test_rotation_trims_the_oldest_lines(monkeypatch):
    monkeypatch.setattr(L, "MAX_LOG_BYTES", 2000)
    for i in range(100):
        L.log_event("T", f"line {i:03d}")
    lines = L.read_log(max_lines=1000).splitlines()
    assert os.path.getsize(L.LOG_FILE) <= 2000 + 50*40
    assert lines[-1].endswith("line 099") and not lines[0].endswith("line 000")


def test_logging_never_raises(monkeypatch):
    def broken(*args, **kwargs):
        raise OSError("read only")
    monkeypatch.setattr("builtins.open", broken)
    L.log_event("ANT", "lost")
    L.clear_log()
    assert L.read_log() == ""


def test_thread_safe():
    threads = [threading.Thread(target=lambda n=n: [L.log_event("T", f"{n}-{i}") for i in range(50)])
               for n in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    lines = L.read_log(max_lines=1000).splitlines()
    assert len(lines) == 400 and all("[T]" in line for line in lines)


def test_error_logging_hooks(monkeypatch):
    for name in ("excepthook", "unraisablehook"):
        monkeypatch.setattr(sys, name, lambda *a: None)
    monkeypatch.setattr(threading, "excepthook", lambda *a: None)
    from PyQt6 import QtCore
    installed = []
    monkeypatch.setattr(QtCore, "qInstallMessageHandler", installed.append)
    L.install_error_logging()
    try:
        raise ValueError("boom")
    except ValueError:
        sys.excepthook(*sys.exc_info())
    thread = threading.Thread(target=lambda: (_ for _ in ()).throw(RuntimeError("in thread")), name="worker")
    thread.start()
    thread.join()
    handler = installed[0]
    context = type("ctx", (), {"file": "x.cpp", "line": 3})()
    handler(QtCore.QtMsgType.QtWarningMsg, context, "careful")
    handler(QtCore.QtMsgType.QtWarningMsg, context, "No QtMultimedia backends found")
    handler(QtCore.QtMsgType.QtDebugMsg, context, "chatter")
    text = L.read_log()
    assert "Unhandled exception:" in text and "ValueError: boom" in text
    assert "in thread worker" in text and "RuntimeError: in thread" in text
    assert "Qt warning (x.cpp:3): careful" in text
    assert "QtMultimedia" not in text and "chatter" not in text
