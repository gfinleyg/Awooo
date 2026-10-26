"""
The SimulANT+ hardware test on the C++ app (AntWorkout\\SimulantTests\\README.md has the set-up).

The ride is the C++ app's own (simulant_ride.exe: its real ANT bridge, readings pipeline and workout screen); the
rest is the Python harness's, unchanged: the SimulANT+ scripts it installs, the timeline they follow, and the checks
and report of what the workout screen showed against what the scripts logged they sent (and, for the trainer, the
commands it received).

  py -3.10 hwtests\\simulant_test.py trainer     FE-C trainer + heart rate
  py -3.10 hwtests\\simulant_test.py sensors     power meter + speed/cadence + heart rate

Options: --offscreen (no window), --wait MINUTES (how long to wait for SimulANT+'s devices, default 10),
--exe PATH (simulant_ride.exe; default the one in dist\\AntWorkout)
"""

import argparse
import csv
import datetime
import importlib.util
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
QT_APP = os.path.dirname(HERE)
PYTHON_HARNESS = os.path.join(os.path.dirname(QT_APP), "AntWorkout", "SimulantTests")
#(the Python app's ant.manager, for the channel periods the reception summary uses)
sys.path.insert(0, os.path.join(os.path.dirname(QT_APP), "AntWorkout", "AntNative", "python"))
_spec = importlib.util.spec_from_file_location("python_harness", os.path.join(PYTHON_HARNESS, "run_simulant_test.py"))
harness = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(harness)
tl = harness.tl

#app_samples.csv's text columns; the rest are numbers (empty: none)
TEXT_COLUMNS = {"phase", "cadenceSource", "speedSource", "power_dev", "sc_dev", "hr_dev", "antMode"}


def read_rows(path):
    rows = []
    with open(path, newline="") as f:
        for raw in csv.DictReader(f):
            row = {}
            for key, value in raw.items():
                if key in TEXT_COLUMNS:
                    row[key] = value or None
                else:
                    row[key] = None if value == "" else float(value)
            rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=sorted(tl.SCENARIOS))
    parser.add_argument("--offscreen", action="store_true")
    parser.add_argument("--wait", type=float, default=10.0)
    parser.add_argument("--exe", default=os.path.join(QT_APP, "dist", "AntWorkout", "simulant_ride.exe"))
    args = parser.parse_args()

    copied = harness.install_scripts()
    runDir = os.path.join(tl.RUN_DIR, datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S_") + args.scenario + "_qt")
    os.makedirs(runDir)
    harness.write_control({"run": runDir, "scenario": args.scenario, "epoch": None})
    harness.say(f"Run folder: {runDir}")
    harness.say(f"Installed {len(copied)} scripts into {harness.SIMULANT_SCRIPTS}")
    phases = tl.phases(args.scenario)
    actions = tl.TRAINER_ACTIONS if args.scenario == "trainer" else {}
    setup = {"scenario": args.scenario, "needs": harness.NEEDS[args.scenario], "wheelMm": tl.WHEEL_MM,
             "controlFile": tl.CONTROL_FILE,
             "phases": [[name, start, end, actions.get(name)] for name, start, end, _ in phases]}
    with open(os.path.join(runDir, "ride_setup.json"), "w") as f:
        json.dump(setup, f, indent=1)
    for name, folder, script in harness.SIMULATORS[args.scenario]:
        harness.say(f"    SimulANT+: {name:48s} script {script}")

    command = [args.exe, runDir, "--wait", str(args.wait)] + (["--offscreen"] if args.offscreen else [])
    code = subprocess.call(command, cwd=runDir)
    if code != 0:
        harness.say(f"simulant_ride.exe exited with {code}")
        return code

    rows = read_rows(os.path.join(runDir, "app_samples.csv"))
    with open(os.path.join(runDir, "ride.json")) as f:
        ride = json.load(f)
    logPath = os.path.join(runDir, "antWorkoutLog.txt")
    logText = open(logPath, encoding="utf-8", errors="replace").read() if os.path.exists(logPath) else ""
    results = harness.check(args.scenario, rows, harness.read_simulant_logs(runDir), phases, logText)
    lines = [f"AntWorkoutQt SimulANT+ test: {args.scenario}   {datetime.datetime.now():%Y-%m-%d %H:%M}",
             f"AntWorkout's ANT stick: {ride['stick']}",
             f"Devices: {ride['selection']}",
             f"CPU while riding (this process, all threads): {ride['cpu']:.1f}% of one core", ""]
    passed = sum(1 for ok, _ in results if ok)
    for ok, text in results:
        lines.append(("PASS  " if ok else "FAIL  ") + text)
    lines.append("")
    lines.append(f"{passed}/{len(results)} checks passed")
    recovery = [line for line in logText.splitlines() if "[RECOVERY]" in line]
    lines.append("Reception (while each device was on, from 6s after it came on):")
    lines.extend("    " + line for line in harness.reception(args.scenario, rows, phases, harness.NEEDS[args.scenario]))
    lines.append(f"Recovery events logged: {len(recovery)}")
    lines.extend("    " + line for line in recovery[:40])
    report = "\n".join(lines)
    with open(os.path.join(runDir, "report.txt"), "w", encoding="utf-8") as f:
        f.write(report + "\n")
    print("\n" + report, flush=True)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
