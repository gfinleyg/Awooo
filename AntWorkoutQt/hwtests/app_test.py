"""
The full app test against SimulANT+ on the C++ app (AntWorkout\\SimulantTests\\run_app_test.py, whose README has the
set-up). The app is the C++ one (simulant_app.exe: the real main window, setup screen, workout screen and synopsis,
on its real ANT bridge), driven through the same steps and checks; the rest is the Python harness's: the SimulANT+
scripts and their steady "pairing" values, the trainer's workout and the songs to ride to.

  py -3.10 hwtests\\app_test.py trainer     FE-C trainer + heart rate: Workout Mode on the trainer, the synopsis,
                                            back to setup, exit
  py -3.10 hwtests\\app_test.py sensors     power meter + speed/cadence + heart rate: Musical Cadence Mode with music
                                            analyzed and played (silently), a shortened 20m FTP test and Apply FTP,
                                            Free Mode, exit

The simulators to have running are the same as run_app_test.py's for that scenario (and only those).
Options: --offscreen (no windows: the build's simulant_app.exe on Qt's offscreen platform), --wait MINUTES (how long
to wait for the devices, default 10), --exe PATH (simulant_app.exe; default the one in dist\\AntWorkout)
"""

import argparse
import datetime
import importlib.util
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
QT_APP = os.path.dirname(HERE)
PYTHON_HARNESS = os.path.join(os.path.dirname(QT_APP), "AntWorkout", "SimulantTests")
QT_BIN = r"C:\Qt\6.7.0\msvc2019_64\bin"


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(PYTHON_HARNESS, file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


#(the Python app's ant.manager, which the ride harness imports for its channel periods)
sys.path.insert(0, os.path.join(os.path.dirname(QT_APP), "AntWorkout", "AntNative", "python"))
rideHarness = load("python_ride_harness", "run_simulant_test.py")
appHarness = load("python_app_harness", "run_app_test.py")
tl = appHarness.tl


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=sorted(appHarness.NEEDS))
    parser.add_argument("--offscreen", action="store_true")
    parser.add_argument("--wait", type=float, default=10.0)
    parser.add_argument("--exe")
    args = parser.parse_args()
    exe = args.exe or (os.path.join(QT_APP, "build", "Release", "bin", "simulant_app.exe") if args.offscreen
                       else os.path.join(QT_APP, "dist", "AntWorkout", "simulant_app.exe"))

    copied = rideHarness.install_scripts()
    runDir = os.path.join(tl.RUN_DIR, datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S_app_") + args.scenario + "_qt")
    os.makedirs(runDir)
    #no epoch: the scripts keep sending their pairing values, and log to this run's folder
    rideHarness.write_control({"run": runDir, "scenario": args.scenario, "epoch": None})
    appHarness.say(f"Run folder: {runDir}")
    appHarness.say(f"Installed {len(copied)} scripts into {rideHarness.SIMULANT_SCRIPTS}")
    #the app's own files in the run folder, starting from known settings; the music silent
    with open(os.path.join(runDir, "savedSettingsANT.json"), "w") as f:
        json.dump({"wheelCircumference": tl.WHEEL_MM/1000, "ftp": appHarness.FTP, "maxHr": 185, "maxWatts": 1200,
                   "powerDriveCutoff": 300, "musicVolume": 0}, f)
    songs = []
    if args.scenario == "sensors":
        for name, bpm in (("App Test - Ninety", 90), ("App Test - One Twenty", 120)):
            path = os.path.join(runDir, name + ".wav").replace("\\", "/")
            appHarness.write_tone(path, 25, bpm)
            songs.append([path, name])
    setup = {"scenario": args.scenario, "needs": sorted(appHarness.NEEDS[args.scenario]),
             "pairingPower": appHarness.PAIRING_POWER, "pairingSpeedKmh": tl.PAIRING["sc"]["speed"],
             "trainerWorkout": appHarness.TRAINER_WORKOUT, "songs": songs}
    with open(os.path.join(runDir, "app_setup.json"), "w") as f:
        json.dump(setup, f, indent=1)

    env = dict(os.environ)
    if args.offscreen:
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["PATH"] = QT_BIN + os.pathsep + env.get("PATH", "")
    code = subprocess.call([exe, runDir, "--wait", str(args.wait)], cwd=runDir, env=env)
    if code not in (0, 1):
        appHarness.say(f"simulant_app.exe exited with {code}")
    return code


if __name__ == "__main__":
    sys.exit(main())
