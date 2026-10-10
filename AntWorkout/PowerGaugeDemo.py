###Power gauge demo: the HUD's power gauge (ClassHUDWindow.new_power_gauge) with made-up readings that climb
###in steps from easy spinning to well past FTP and start over. The reading sits in the middle of the gauge, and the
###nested circles there show the last POWER_RING_SECONDS of power: the outermost ring the power now, the
###innermost the power that long ago (see ClassHUDWindow.set_power_rings). No ANT stick needed.
###Run: python PowerGaugeDemo.py [--ftp 250] [--gif out.gif]
###  --ftp  the FTP the power zones (the band and the rings' colors) are worked out from
###  --gif  saves one climb as an animated gif instead of showing the window

###Same import order guard as AntWorkout.py: ant_native.pyd must load before PyQt6
import os as _os
import sys as _sys
_ant_native_python_dir = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "AntNative", "python")
if _ant_native_python_dir not in _sys.path:
    _sys.path.insert(0, _ant_native_python_dir)
import ant.manager as _ant_manager_preload  # noqa: F401  (import order guard, see above)

import argparse
import random
import sys

_os.chdir(_os.path.dirname(_os.path.abspath(__file__)))

from PyQt6.QtCore import QTimer
from PyQt6.QtWidgets import QApplication

from AppState import AppState
import AppTheme
from ClassHUDWindow import new_power_gauge, set_power_rings
import FtpColors

#the climb: steps from start to end (both % of FTP), each held a while, so the rings show the steps behind
#the one being ridden; a power meter's readings come about 4 a second
CLIMB_START_PERCENT = 20
CLIMB_END_PERCENT = 200
CLIMB_STEP_PERCENT = 15
CLIMB_STEP_SECONDS = 2
CLIMB_SECONDS = CLIMB_STEP_SECONDS*((CLIMB_END_PERCENT - CLIMB_START_PERCENT)//CLIMB_STEP_PERCENT + 1)
READING_SECONDS = 0.25
TICK_SECONDS = 0.1 #(as the HUD's process_events)


class Ride:
    """The made-up readings, into the gauge and AppState.savedPowerVals as AntDevicesPoller puts them"""

    def __init__(self, gauge, ftp):
        self.gauge = gauge
        self.ftp = ftp
        self.nextReading = 0.0
        AppState.secondsPassed = 0.0
        AppState.savedPowerVals = ([], [])

    def power(self, seconds):
        step = int(seconds % CLIMB_SECONDS)//CLIMB_STEP_SECONDS
        percent = CLIMB_START_PERCENT + CLIMB_STEP_PERCENT*step
        return max(0, round(self.ftp*percent/100 + random.uniform(-6, 6)))

    def tick(self):
        """TICK_SECONDS on: any readings due, then the rings, as the HUD's process_events does"""
        AppState.secondsPassed += TICK_SECONDS
        while self.nextReading <= AppState.secondsPassed:
            power = self.power(self.nextReading)
            self.gauge.ui.widget.setValue(power)
            AppState.savedPowerVals[0].append(AppState.secondsPassed)
            AppState.savedPowerVals[1].append(power)
            while AppState.savedPowerVals[0][-1] - AppState.savedPowerVals[0][0] > 30:
                AppState.savedPowerVals[0].pop(0)
                AppState.savedPowerVals[1].pop(0)
            self.nextReading += READING_SECONDS
        set_power_rings(self.gauge)


def save_gif(gauge, ride, path):
    from PIL import Image
    #no easing: each frame shows the needle where the HUD's would have caught up to
    widget = gauge.ui.widget
    frames = []
    framesEvery = 2 #ticks (5 frames a second)
    for tick in range(round(CLIMB_SECONDS/TICK_SECONDS)):
        ride.tick()
        if tick % framesEvery:
            continue
        widget.displayValue = widget.value
        widget.update()
        image = gauge.grab().toImage()
        frames.append(Image.frombytes("RGBA", (image.width(), image.height()),
                                      bytes(image.convertToFormat(image.Format.Format_RGBA8888).constBits()
                                            .asarray(image.sizeInBytes()))).convert("RGB"))
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=round(1000*TICK_SECONDS*framesEvery),
                   loop=0)
    print(f"saved {len(frames)} frames to {path}")


def main():
    parser = argparse.ArgumentParser(description="The HUD's power gauge with made-up climbing power")
    parser.add_argument("--ftp", type=int, default=250)
    parser.add_argument("--gif", help="save one climb as an animated gif instead of showing the window")
    args = parser.parse_args()

    app = QApplication(sys.argv)
    AppTheme.register_fonts()
    AppState.ftp = str(args.ftp)

    gauge = new_power_gauge()
    gauge.ui.widget.set_value_colors(FtpColors.gauge_scale_stops(FtpColors.zone_ftp())) #(as gaugeChooser)
    gauge.setWindowTitle(f"Power gauge demo: FTP {args.ftp} W")
    gauge.resize(420, 420)
    ride = Ride(gauge, args.ftp)

    if args.gif:
        save_gif(gauge, ride, args.gif)
        return

    timer = QTimer(gauge)
    timer.timeout.connect(ride.tick)
    timer.start(round(TICK_SECONDS*1000))
    gauge.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
