import sys
from pathlib import Path

here = Path(__file__).resolve()

_native_path = (
    here.parents[3]
    / "build"
    / "Release"
)

sys.path.insert(0, str(_native_path))

import ant_native


class NativeTransport:

    def __init__(self):
        self.native = ant_native.ANTNative()


    def list_devices(self):
        return ant_native.list_devices()


    def __getattr__(self, name):
        return getattr(self.native, name)