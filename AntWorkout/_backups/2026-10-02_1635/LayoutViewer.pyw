"""Console-less launcher for LayoutViewer.py (double-click to run without a command prompt).

print() output and any crash traceback go to LayoutViewer_console.log since there is no console.
Optional args are passed through: LayoutViewer.pyw --directwrite
"""
import os
import runpy
import sys

here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)
sys.path.insert(0, here)

if sys.stdout is None or sys.stderr is None:
    log = open(os.path.join(here, "LayoutViewer_console.log"), "w", buffering=1, encoding="utf-8")
    sys.stdout = sys.stderr = log

runpy.run_path(os.path.join(here, "LayoutViewer.py"), run_name="__main__")
