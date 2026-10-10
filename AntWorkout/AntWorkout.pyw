"""Console-less launcher for AntWorkout.py (double-click to run without a command prompt).

print() output and any crash traceback go to AntWorkout_console.log since there is no console.
"""
import os
import runpy
import sys

here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)
sys.path.insert(0, here)

if sys.stdout is None or sys.stderr is None:
    log = open(os.path.join(here, "AntWorkout_console.log"), "w", buffering=1, encoding="utf-8")
    sys.stdout = sys.stderr = log

#(not in the music analysis worker processes, which import this file as __mp_main__, see AntWorkout.py)
if __name__ == "__main__":
    runpy.run_path(os.path.join(here, "AntWorkout.py"), run_name="__main__")
