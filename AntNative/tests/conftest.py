"""AntNative's own tests: the decode (through the Python module and through antnative.dll's C API) and the C API's
behavior without a stick. Run from this folder after building (build_windows.ps1):  py -m pytest
They need no ANT stick; the hardware scripts are in ..\\python\\*_test.py."""
import os
import sys

ANTNATIVE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(ANTNATIVE_DIR, "build", "Release")

sys.path.insert(0, BUILD_DIR)
sys.path.insert(0, os.path.join(ANTNATIVE_DIR, "python"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
