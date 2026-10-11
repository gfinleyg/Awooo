"""
AntWorkout hardware test against SimulANT+ (see README.md in this folder).

Runs the app's real ANT bridge, device poller and workout screen (Free Mode) on one ANT stick while SimulANT+
simulates the sensors on the other, following the timeline in simulant_scripts/modules/awo_timeline.py. Every
100ms it records what the workout screen shows; at the end it checks that against what the SimulANT+ scripts
logged they transmitted (and, for the trainer, the commands it received), and writes a report.

  python SimulantTests\\run_simulant_test.py sensors     power meter + speed/cadence + heart rate
  python SimulantTests\\run_simulant_test.py trainer     FE-C trainer + heart rate

Options: --offscreen (no window), --wait MINUTES (how long to wait for SimulANT+'s devices, default 10)
"""

import argparse
import csv
import datetime
import os
import shutil
import statistics
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SCRIPTS_SRC = os.path.join(HERE, "simulant_scripts")
SIMULANT_SCRIPTS = os.path.join(os.path.expanduser("~"), "Documents", "SimulANT+", "Scripts")

sys.path.insert(0, os.path.join(SCRIPTS_SRC, "modules"))
import awo_timeline as tl  # noqa: E402

#which device types each scenario needs, by AntWorkout role
NEEDS = {"sensors": {"power": 11, "speed_cadence": 121, "hrm": 120},
         "trainer": {"power": 17, "hrm": 120}}
SIMULATORS = {"sensors": [("Bike Power Sensor (Power Only)", "BikePowerOnlySensor", "awo_power.py"),
                          ("Bike Speed & Cadence Sensor", "BikeSpeedCadenceSensor", "awo_speedcadence.py"),
                          ("Heart Rate Sensor", "HeartRateSensor", "awo_heartrate.py")],
              "trainer": [("Bike Trainer (controllable fitness equipment)", "TrainerControlDevice", "awo_trainer.py"),
                          ("Heart Rate Sensor", "HeartRateSensor", "awo_heartrate.py")]}
KPH_PER_MPH = 1.609344


def say(text):
    print(time.strftime("%H:%M:%S ") + text, flush=True)


def install_scripts():
    """The SimulANT+ scripts into its Scripts folder, so they show in each simulator's script list"""
    if os.path.normcase(os.path.abspath(tl.RUN_DIR)) != os.path.normcase(os.path.join(HERE, "run")):
        raise SystemExit(f"awo_timeline.RUN_DIR ({tl.RUN_DIR}) isn't this folder's run folder - fix it first")
    if not os.path.isdir(SIMULANT_SCRIPTS):
        raise SystemExit(f"SimulANT+ scripts folder not found: {SIMULANT_SCRIPTS}")
    copied = []
    for root, _, files in os.walk(SCRIPTS_SRC):
        for name in files:
            if not name.endswith(".py"):
                continue
            rel = os.path.relpath(os.path.join(root, name), SCRIPTS_SRC)
            dest = os.path.join(SIMULANT_SCRIPTS, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(os.path.join(root, name), dest)
            copied.append(rel)
    return copied


def write_control(control):
    temp = tl.CONTROL_FILE + ".tmp"
    with open(temp, "w") as f:
        f.write(tl.control_text(control))
    os.replace(temp, tl.CONTROL_FILE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=sorted(tl.SCENARIOS))
    parser.add_argument("--offscreen", action="store_true")
    parser.add_argument("--wait", type=float, default=10.0)
    parser.add_argument("--stick", type=int, default=None,
                        help="ANT stick index for AntWorkout (default: the first free one); SimulANT+ gets the other")
    args = parser.parse_args()
    if args.offscreen:
        os.environ["QT_QPA_PLATFORM"] = "offscreen"

    copied = install_scripts()
    runDir = os.path.join(tl.RUN_DIR, datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S_") + args.scenario)
    os.makedirs(runDir)
    control = {"run": runDir, "scenario": args.scenario, "epoch": None}
    write_control(control)
    #the app's own files (its log, settings) go in the run folder, not the project
    os.chdir(runDir)
    say(f"Run folder: {runDir}")
    say(f"Installed {len(copied)} scripts into {SIMULANT_SCRIPTS}")

    #ant_native before PyQt6 (see AntWorkout.py)
    sys.path.insert(0, os.path.join(PROJECT, "AntNative", "python"))
    sys.path.insert(0, PROJECT)
    import ant.manager  # noqa: F401
    import ClassAntLogger
    ClassAntLogger.install_error_logging()
    errors = []
    previousHook = sys.excepthook
    def hook(t, v, tb):
        errors.append("".join(traceback.format_exception(t, v, tb)))
        previousHook(t, v, tb)
    sys.excepthook = hook

    from PyQt6.QtCore import QTimer
    from PyQt6.QtGui import QFont
    from PyQt6.QtWidgets import QApplication, QMainWindow
    app = QApplication(sys.argv)
    import AppStyle
    import AppTheme
    from AppState import AppState
    import FtpColors
    from ClassAntDevices import AntWorkoutBridge
    from ClassHUDWindow import HUDWindow

    AppTheme.install()
    AppState.defaultFont = QFont(app.font())
    app.setStyleSheet(AppStyle.APP_STYLESHEET)
    AppTheme.clear()

    bridge = AntWorkoutBridge()
    from ant.transport.native import ant_native
    sticks = {d["index"]: d.get("product_description") for d in ant_native.list_devices()}
    if args.stick is not None:
        if args.stick not in sticks:
            raise SystemExit(f"No ANT stick {args.stick}; there are {sticks}")
        openStick = bridge.ant.open
        bridge.ant.open = lambda: openStick(device_number=args.stick)
    bridge.errorOccurred.connect(lambda e: (say("ANT error: " + e), app.exit(2)))
    bridge.start()

    class Window(QMainWindow):
        #the parts of MainWindow the workout screen uses
        def __init__(self):
            super().__init__()
            self.startExe = bridge
            self.resize(1400, 900)

    state = {"stage": "init", "hud": None, "window": None, "rows": [], "phase": None, "cpu0": None,
             "selection": None, "waitUntil": time.time() + args.wait*60, "announced": False}
    needs = NEEDS[args.scenario]
    phases = tl.phases(args.scenario)
    total = tl.total_seconds(args.scenario)

    def announce():
        stick = getattr(bridge.ant, "device_number", None)
        state["stick"] = f"{stick} ({sticks.get(stick, '?')})"
        say(f"AntWorkout is scanning on ANT stick {state['stick']}. Now in SimulANT+, on the "
            "OTHER stick's device, add these simulators and run each one's script (Script panel, pick it from the "
            "list, Run):")
        for name, folder, script in SIMULATORS[args.scenario]:
            say(f"    {name:48s} script {script}")
        say("The scripts send steady 'pairing' values until AntWorkout connects; then the timed test starts by itself "
            f"(about {total/60:.1f} minutes).")

    def tick():
        try:
            step()
        except Exception:
            errors.append(traceback.format_exc())
            say("Harness error:\n" + traceback.format_exc())
            app.exit(3)

    def step():
        now = time.time()
        if state["stage"] == "init":
            if bridge.is_initialized():
                announce()
                state["stage"] = "scan"
            elif now > state["waitUntil"]:
                raise RuntimeError("ANT stick never initialized")
            return

        if state["stage"] == "scan":
            found = {}
            for entry in bridge.found_devices():
                deviceType, number = (int(x) for x in entry.split())
                found.setdefault(deviceType, []).append(number)
            missing = [role for role, deviceType in needs.items() if deviceType not in found]
            if missing or bridge.scan_settle_remaining() > 0:
                if now > state["waitUntil"]:
                    raise RuntimeError(f"Timed out waiting for SimulANT+ devices: {missing}")
                return
            selection = {}
            for role, deviceType in needs.items():
                numbers = found[deviceType]
                if len(numbers) > 1:
                    say(f"WARNING: several type {deviceType} devices heard {numbers}, using {numbers[-1]}")
                selection[role] = (deviceType, numbers[-1])
            state["selection"] = selection
            say(f"Found everything, connecting: {selection}")
            bridge.request_connect(selection)
            state["stage"] = "connect"
            return

        if state["stage"] == "connect":
            if not bridge.is_ready():
                return
            roles = bridge.selected_roles()
            AppState.antHRM = "hrm" in roles
            AppState.antSpeedCadence = "speed_cadence" in roles
            AppState.antCadence = AppState.antSpeed = False
            AppState.workoutMode = False
            AppState.cadenceMode = False
            AppState.ftpTestMode = None
            AppState.ftp, AppState.maxHr = "250", "185"
            AppState.wheelCircumference = str(tl.WHEEL_MM/1000)
            AppState.maxWatts = 1200
            AppState.ftpRanges[:] = FtpColors.power_ranges()
            FtpColors.update_graph_brushes()
            AppState.startTimestamp = datetime.datetime.now()
            window = Window()
            hud = HUDWindow(window)
            window.setCentralWidget(hud)
            window.show()
            hud.show()
            hud.antDevices.start()
            state["window"], state["hud"] = window, hud
            control["epoch"] = time.time() + 2.0
            write_control(control)
            state["cpu0"] = (time.process_time(), time.time())
            say(f"Connected - timeline starts now ({total:.0f}s)")
            state["stage"] = "run"
            return

        if state["stage"] == "run":
            hud = state["hud"]
            t = now - control["epoch"]
            phase = next((p for p in phases if p[1] <= t < p[2]), None)
            name = phase[0] if phase else ("done" if t >= total else "pre")
            if name != state["phase"]:
                state["phase"] = name
                say(f"t={t:6.1f}s  phase: {name}")
                if args.scenario == "trainer" and name in tl.TRAINER_ACTIONS:
                    action = tl.TRAINER_ACTIONS[name]
                    if action == "release":
                        bridge.release_trainer_power()
                    elif action == "stop":
                        hud.antDevices.stop()
                    else:
                        hud.difficultySlider.setValue(action)
                        #(the same value again doesn't move the slider, so command it directly too)
                        bridge.set_trainer_target_power(action)
            record(t, name)
            if t >= total + 3:
                finish()

    def record(t, phaseName):
        hud = state["hud"]
        def gauge(attr):
            display = getattr(hud, attr, None)
            return None if display is None else display.ui.widget.value
        def device(role):
            d = bridge.selected_device(role)
            return "-" if d is None else f"{d.state}{'/stalled' if d.data_stalled else ''}"
        def packets(role):
            #messages heard from it so far (a stick reset can make a new device object: summed over every match)
            identity = state["selection"][role]
            return sum(d.packet_count for d in bridge.ant.get_devices()
                       if (d.device_type, d.device_number) == identity)
        poller = hud.antDevices
        target = bridge._trainer_target
        state["rows"].append({
            "t": round(t, 2), "phase": phaseName,
            "power": gauge("wattGaugeDisplay"), "powerReading": hud.wattGaugeDisplay.ui.widget.reading,
            "cadence": gauge("cadenceGaugeDisplay"), "hr": gauge("hrGaugeDisplay"),
            "hrMin": hud.hrGaugeDisplay.ui.widget.minValue if hasattr(hud, "hrGaugeDisplay") else None,
            "hrReading": hud.hrGaugeDisplay.ui.widget.reading if hasattr(hud, "hrGaugeDisplay") else None,
            "speedKph": round(hud.speedMph*KPH_PER_MPH, 3), "distKm": round(hud.distanceMiles*KPH_PER_MPH, 4),
            "cadenceSource": getattr(poller, "_cadenceSource", None), "speedSource": getattr(poller, "_speedSource", None),
            "power_dev": device("power"), "sc_dev": device("speed_cadence"), "hr_dev": device("hrm"),
            "trainerTarget": None if target is None else target.get("watts"),
            "antMode": bridge.ant.mode, "antChannels": len(bridge.ant.channel_devices()),
            **{f"pkts_{role}": packets(role) for role in needs},
        })

    def finish():
        state["stage"] = "done"
        cpu0, wall0 = state["cpu0"]
        cpu = (time.process_time() - cpu0)/(time.time() - wall0)*100
        hud = state["hud"]
        hud.stop_timers()
        hud.antDevices.stop()
        say("Timeline finished - checking")
        rows = state["rows"]
        with open(os.path.join(runDir, "app_samples.csv"), "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        logText = open("antWorkoutLog.txt", encoding="utf-8", errors="replace").read() \
            if os.path.exists("antWorkoutLog.txt") else ""
        results = check(args.scenario, rows, read_simulant_logs(runDir), phases, logText)
        lines = [f"AntWorkout SimulANT+ test: {args.scenario}   {datetime.datetime.now():%Y-%m-%d %H:%M}",
                 f"AntWorkout's ANT stick: {state.get('stick')}",
                 f"Devices: {state['selection']}",
                 f"CPU while riding (this process, all threads): {cpu:.1f}% of one core", ""]
        passed = sum(1 for ok, _ in results if ok)
        for ok, text in results:
            lines.append(("PASS  " if ok else "FAIL  ") + text)
        lines.append("")
        lines.append(f"{passed}/{len(results)} checks passed")
        if errors:
            lines.append(f"{len(errors)} Python error(s):")
            lines.extend(errors[:5])
        recovery =[l for l in logText.splitlines() if "[RECOVERY]" in l]
        lines.append("Reception (while each device was on, from 6s after it came on):")
        lines.extend("    " + line for line in reception(args.scenario, rows, phases, needs))
        lines.append(f"Recovery events logged: {len(recovery)}")
        lines.extend("    " + l for l in recovery[:40])
        report = "\n".join(lines)
        with open(os.path.join(runDir, "report.txt"), "w", encoding="utf-8") as f:
            f.write(report + "\n")
        print("\n" + report, flush=True)
        say("You can stop the scripts in SimulANT+ now.")
        bridge.stop()
        bridge.wait(5000)
        app.exit(0 if passed == len(results) and not errors else 1)

    timer = QTimer()
    timer.timeout.connect(tick)
    timer.start(100)
    code = app.exec()
    if state["stage"] != "done":
        bridge.stop()
        bridge.wait(5000)
    #(out without Python's teardown, as AntWorkout.py does)
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)


###----------------------------------------------------------------------------------------------
### Reception: how much of what each device sent got through (information, for comparing sticks)
###----------------------------------------------------------------------------------------------
ROLE_DEVICE = {"sensors": {"power": "pm", "speed_cadence": "sc", "hrm": "hr"},
               "trainer": {"power": "fe", "hrm": "hr"}}


def reception(scenario, rows, phases, needs):
    from ant.manager import CHANNEL_PERIODS, DEFAULT_CHANNEL_PERIOD
    lines = []
    for role, deviceType in needs.items():
        device = ROLE_DEVICE[scenario][role]
        rate = 32768/CHANNEL_PERIODS.get(deviceType, DEFAULT_CHANNEL_PERIOD)
        key = f"pkts_{role}"
        received = expected = 0.0
        gaps = []
        for name, start, end, values in phases:
            if values.get(device) is None:
                continue
            window = [r for r in rows if start + 6 <= r["t"] < end and r[key] is not None]
            if len(window) < 2:
                continue
            received += window[-1][key] - window[0][key]
            expected += (window[-1]["t"] - window[0]["t"])*rate
            lastChange = window[0]["t"]
            for previous, row in zip(window, window[1:]):
                if row[key] != previous[key]:
                    if row["t"] - lastChange > 0.75:
                        gaps.append(row["t"] - lastChange)
                    lastChange = row["t"]
            if window[-1]["t"] - lastChange > 0.75:
                gaps.append(window[-1]["t"] - lastChange)
        if not expected:
            continue
        lines.append(f"{role:14s} {100*received/expected:5.1f}% of {expected:.0f} expected messages; "
                     f"{len(gaps)} gaps over 0.75s, longest {max(gaps, default=0):.1f}s, "
                     f"over 2s: {sum(1 for g in gaps if g > 2)}")
    return lines


###----------------------------------------------------------------------------------------------
### Checks
###----------------------------------------------------------------------------------------------
def read_simulant_logs(runDir):
    """{device: [(time.time(), event, detail)]} from the SimulANT+ scripts"""
    logs = {}
    for name in os.listdir(runDir):
        if name.startswith("simulant_") and name.endswith(".csv"):
            device = name[len("simulant_"):-len(".csv")]
            with open(os.path.join(runDir, name)) as f:
                logs[device] = [(float(t), event, detail) for t, event, detail in
                                (line.rstrip("\n").split(",", 2) for line in f if line.count(",") >= 2)]
    return logs


def check(scenario, rows, logs, phases, logText=""):
    results = []
    epoch = tl.read_control()["epoch"]
    byName = {name: (start, end, values) for name, start, end, values in phases}

    def ok(cond, text):
        results.append((bool(cond), text))

    def window(name, fromStart=8.0, toEnd=0.0):
        start, end, _ = byName[name]
        return [r for r in rows if start + fromStart <= r["t"] < end + toEnd]

    def med(name, key, **kw):
        values = [r[key] for r in window(name, **kw) if r[key] is not None]
        return statistics.median(values) if values else None

    def near(name, key, expected, tolerance, label, **kw):
        value = med(name, key, **kw)
        good = value is not None and abs(value - expected) <= tolerance
        ok(good, f"{name}: {label} {value if value is None else round(value, 2)} (expected {expected} ±{tolerance})")

    def never_back_up(name, key, label):
        #once it reads 0 in this phase it stays at 0 (the stale reading never comes back)
        series = [(r["t"], r[key]) for r in window(name, fromStart=0) if r[key] is not None]
        firstZero = next((t for t, v in series if v <= 0.05), None)
        if firstZero is None:
            ok(False, f"{name}: {label} never reached 0")
            return
        after = [v for t, v in series if t >= firstZero]
        cameBack = max(after) > 0.05
        ok(not cameBack, f"{name}: {label} reached 0 {firstZero - byName[name][0]:.1f}s in and "
                         + (f"went back up to {max(after):.1f} after" if cameBack else "stayed there"))

    #the SimulANT+ scripts really followed the timeline
    for device in {"sensors": ("pm", "sc", "hr"), "trainer": ("fe", "hr")}[scenario]:
        entries = logs.get(device, [])
        seen = {detail.split("|")[0] for t, event, detail in entries if event == "tx"}
        expected = {name for name, _, _, values in phases if values.get(device) is not None}
        errors = [detail for t, event, detail in entries if event == "error"]
        missed = sorted(expected - seen)
        ok(entries and not errors and not missed,
           f"SimulANT+ {device} script ran the timeline" + (f" - errors: {errors[:2]}" if errors else "")
           + (f" - phases not sent: {missed}" if entries and missed else "")
           + ("" if entries else " - no log (script not run?)"))

    #the devices stay on their own dedicated channels (what lets the trainer be commanded); a stick reset can
    #take them away for a moment, the scan channel for good is a failure
    expectedChannels = len(NEEDS[scenario])
    off = [r["t"] for r in rows if r["t"] >= 0 and (r["antMode"] != "channels" or r["antChannels"] != expectedChannels)]
    ok(len(off) <= 30, f"dedicated channels open the whole ride ({len(off)/10:.1f}s without them"
                       + (f", from t={off[0]:.1f}s" if off else "") + ")")

    #a device switched off while the others carry on is no reason to reset the stick (which cuts them all off)
    resets = [l for l in logText.splitlines() if "escalating to device-level reset" in l]
    ok(not resets, f"no stick resets for a device that's just switched off ({len(resets)})")
    roleColumn = {"power": "power_dev", "speed_cadence": "sc_dev", "hrm": "hr_dev"}
    roleDevice = {"power": "pm" if scenario == "sensors" else "fe", "speed_cadence": "sc", "hrm": "hr"}
    for name, start, end, values in phases:
        for role, column in roleColumn.items():
            if role not in NEEDS[scenario] or values.get(roleDevice[role]) is None:
                continue
            #(a device that was off in the phase before gets a few seconds to be found again)
            lost = [r["t"] for r in window(name, fromStart=6) if "LOST" in str(r[column])]
            if any(values.get(roleDevice[r]) is None for r in NEEDS[scenario]) or lost:
                ok(not lost, f"{name}: {role} kept coming through"
                             + (f" - lost from t={lost[0]:.1f}s" if lost else ""))

    if scenario == "sensors":
        for name, power, cadence, speed, hr in (("ride", 200, 85, 30.0, 130), ("harder", 300, 95, 36.0, 150),
                                                ("resume", 200, 85, 30.0, 130)):
            near(name, "power", power, power*0.03, "power")
            near(name, "cadence", cadence, 1.5, "cadence")
            near(name, "speedKph", speed, 0.8, "speed km/h")
            near(name, "hr", hr, 1, "heart rate")
        #stopping: everything to 0 and staying there (the fix for stale cadence / made-up speed)
        near("stopped", "power", 0, 0, "power")
        never_back_up("stopped", "cadence", "cadence")
        never_back_up("stopped", "speedKph", "speed")
        stoppedStart, stoppedEnd, _ = byName["stopped"]
        stalled = [r["t"] for r in rows if stoppedStart <= r["t"] < stoppedEnd and "stalled" in str(r["sc_dev"])]
        results.append((True, "stopped: speed/cadence sensor flagged as stalled from "
                        + (f"t={stalled[0]:.1f}s ({stalled[0]-stoppedStart:.1f}s in)" if stalled else "never")
                        + " (information only)"))
        #speed/cadence sensor gone while riding: cadence from the power meter, speed estimated, no distance added
        near("sc dropout", "cadence", 88, 1.5, "cadence (power meter fallback)", fromStart=10)
        near("sc dropout", "speedKph", 31.2, 31.2*0.12, "speed km/h (estimated from power)", fromStart=10)
        dropRows = window("sc dropout", fromStart=3)
        ok(dropRows and dropRows[-1]["distKm"] - dropRows[0]["distKm"] <= 0.002,
           f"sc dropout: distance not added from the estimate ({dropRows[0]['distKm']} -> {dropRows[-1]['distKm']} km)"
           if dropRows else "sc dropout: no samples")
        sources = {r["cadenceSource"] for r in window("sc dropout", fromStart=10)}
        ok(sources == {"fallback"}, f"sc dropout: cadence source {sorted(map(str, sources))}")
        near("sc back", "cadence", 88, 1.5, "cadence (sensor again)")
        near("sc back", "speedKph", 31.0, 0.8, "speed km/h (sensor again)")
        backRows = window("sc back")
        ok(backRows and backRows[-1]["distKm"] > backRows[0]["distKm"],
           "sc back: distance counting again" if backRows else "sc back: no samples")
        #heart rate monitor gone: the gauge drops to its bottom, then comes back
        hrRows = window("hr dropout")
        ok(hrRows and all(r["hr"] <= r["hrMin"] for r in hrRows),
           f"hr dropout: heart rate gauge at its minimum (max {max(r['hr'] for r in hrRows) if hrRows else '-'})")
        #(the number in the middle reads 0, not the needle's minimum)
        ok(hrRows and all(r["hrReading"] == 0 for r in hrRows),
           f"hr dropout: heart rate reads 0 (max {max(r['hrReading'] for r in hrRows) if hrRows else '-'})")
        near("hr back", "hr", 140, 1, "heart rate")
        #power meter gone: power drops to 0, cadence/speed carry on from their own sensor
        near("pm dropout", "power", 0, 0, "power")
        near("pm dropout", "cadence", 85, 1.5, "cadence (sensor)")
        near("pm dropout", "speedKph", 30.0, 0.8, "speed km/h (sensor)")
        near("pm back", "power", 200, 6, "power")

    if scenario == "trainer":
        fe = logs.get("fe", [])
        targets = [(t - epoch, float(d)) for t, e, d in fe if e == "rx_target"]
        releases = [(t - epoch, float(d)) for t, e, d in fe if e == "rx_resistance"]
        onTimes = [t - epoch for t, e, d in fe if e == "on"]

        def received(watts, start, within, label):
            hit = next((t for t, w in targets if start - 1 <= t <= start + within and abs(w - watts) <= 0.5), None)
            ok(hit is not None, f"{label}: trainer received target {watts}W"
                                + (f" {hit - start:+.1f}s after" if hit is not None else f" - not within {within}s"))
        for name, watts in (("target 150", 150), ("target 250", 250), ("target 100", 100), ("target 200", 200)):
            start, end, _ = byName[name]
            received(watts, start, 3, name)
            near(name, "power", watts, max(3, watts*0.03), "power shown (trainer riding at the target)")
        start, end, _ = byName["target 150"]
        resends = [t for t, w in targets if start + 3 <= t < end and abs(w - 150) <= 0.5]
        ok(len(resends) >= 8, f"target 150: target re-sent while held ({len(resends)} times in {end-start-3:.0f}s)")
        start, end, _ = byName["released"]
        rel = next((t for t, r in releases if start - 1 <= t <= start + 3 and r == 0), None)
        ok(rel is not None, "released: trainer received basic resistance 0%"
                            + (f" {rel - start:+.1f}s after" if rel is not None else " - not within 3s"))
        late = [w for t, w in targets if start + 1 < t < end]
        ok(not late, f"released: no target power sent while released ({len(late)} sent)")
        near("released", "power", 120, 4, "power shown (rider's own)")
        near("fe dropout", "power", 0, 0, "power")
        start, end, _ = byName["fe back"]
        back = next((t for t in onTimes if t >= start - 1), None)
        if back is None:
            ok(False, "fe back: trainer never switched back on")
        else:
            again = next((t for t, w in targets if t >= back and abs(w - 200) <= 0.5), None)
            ok(again is not None and again - back <= 8,
               "fe back: target 200W sent again after the trainer came back"
               + (f" ({again - back:.1f}s after)" if again is not None else " - never"))
        near("fe back", "power", 200, 6, "power shown", fromStart=12)
        start, end, _ = byName["stopped"]
        received(0, start, 4, "stopped")
        near("target 150", "hr", 130, 1, "heart rate")
    return results


if __name__ == "__main__":
    main()
