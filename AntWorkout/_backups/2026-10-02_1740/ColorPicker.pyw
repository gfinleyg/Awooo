"""Console-less launcher for ColorPicker.py (double-click to run without a command prompt).

Optional args are passed through: ColorPicker.pyw r g b a
"""
import os
import runpy
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)

if sys.stdout is None or sys.stderr is None:
    log = open(os.path.join(here, "ColorPicker_console.log"), "w", buffering=1, encoding="utf-8")
    sys.stdout = sys.stderr = log

runpy.run_path(os.path.join(here, "ColorPicker.py"), run_name="__main__")
