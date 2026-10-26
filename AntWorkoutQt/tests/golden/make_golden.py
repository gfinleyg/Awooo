"""
Writes the golden files the C++ tests compare against: what the Python app's own modules (..\\..\\..\\AntWorkout) give
for the same inputs. Run it again whenever the Python app's results change on purpose:

    py -3.10 make_golden.py

(the Python version AntWorkout's tests use, with its requirements and requirements-test installed)

Workout entries are written the way the Python app keeps them, as its tuples (lists in JSON); the C++ tests turn
their typed entries into the same shapes to compare.
"""

import glob
import json
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "..", "AntWorkout"))
for path in (os.path.join(APP_DIR, "AntNative", "python"), APP_DIR, os.path.join(APP_DIR, "tests")):
    sys.path.insert(0, path)
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

# ant_native has to load before PyQt6 (see AntWorkout.py)
import ant.manager  # noqa: E402,F401

from AppState import AppState  # noqa: E402
import AppTheme  # noqa: E402
import FtpColors  # noqa: E402
import FtpTest  # noqa: E402
import PowerCalibration  # noqa: E402
import Session  # noqa: E402
import Units  # noqa: E402
import WorkoutGrading  # noqa: E402
import WorkoutHelpers  # noqa: E402
import ZwoConvert  # noqa: E402
from workout_files import WorkoutParser  # noqa: E402


def plain(value):
    """Tuples as lists, all the way down"""
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    if isinstance(value, dict):
        return {str(k): plain(v) for k, v in value.items()}
    return value


def write(name, data):
    #(compact: the workouts' 21 targets a block add up)
    with open(os.path.join(HERE, name), "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")
    print("wrote", name)


###Workout files
def parse_result(parser, ok):
    rejection = parser.rejections[0] if parser.rejections else None
    return {
        "ok": bool(ok),
        "title": AppState.workoutTitle if ok else None,
        "seconds": AppState.workoutDurationSeconds if ok else None,
        "entries": plain(AppState.workoutList) if ok else None,
        "messages": plain(AppState.workoutMessages) if ok else None,
        "rejection": list(rejection) if rejection else None,
    }


# lines read on their own (beside the shipped files): edge cases of each check
LINE_CASES = [
    "T: My Ride ", "T:", "Title", "T:A", "4:60", " 1:60,0.5", "", "0", "0:60", "0:60,5", "0:60, 3", "0:60,+3", "0:60,03",
    "0:60,1_0", "0:60,x", "0:60:1", "0:0", "0:1800", "0:1801", "0:6.5", "0:60 ", "0:99999999999999999999",
    "1:60,0.5", "1:60, 0.5 ", "1:60,.5", "1:60,5.", "1:60,1e-1", "1:60,0.0_5", "1:60,+0.5", "1:60,-0.5", "1:60,nan",
    "1:60,inf", "1:60,3", "1:60,3.0001", "1:60,0.05", "1:60,0.0499", "1:60,250W", "1:60,250w", "1:60,250 W",
    "1:60, 250W", "1:60,0W", "1:60,2000W", "1:60,2001W", "1:60,W", "1:60,abc", "1:60", "1:60,0.5,1,2", "1:60,0.5,6",
    "2:60,0.5", "2:60,0.5,1.5", "2:120,0.5,1.0,2", "2:60,1.0,0.5", "2:20,0.600,0.602", "2:60,0.5,0.50",
    "2:60,250W,250w", "2:60,100W,0.5", "2:1800,0.1,3", "2:7,0.3,2.9", "2:3,1,1.01",
    "3:4;1:30,1.2,3;0:30", "3:3;1:30,1.2,3;2:20,0.5,0.6;0:10,1", "3:2;1:30,1;1:30,1;1:30,1;1:30,1", "3:0;1:30,1;0:30",
    "3:x;1:30,1;0:30", "3:2;1:30,1;", "3:2|Hi;1:30,1;0:30", "3:2;1:30,1;7:30", "3;1:30,1;0:30", "3:3;2:60,0.5,1.0",
    "3:10;1:400,0.6", "3:2;0:901", "3:2;0:900", "3:2;1:30,1;3:2;1:30,1", "3:4;1:30,1.2,3|Go|5", "3:2;0:45",
    "3:3;2:20,0.5,0.50|Up|5|10", "3:2;1:30,1.2|Go|5;0:20|Rest||5", "3:2;1:30,1|Hi|40;0:30", "3:2;2:30,250W,250W;0:30",
    "3:1001;1:1,0.5;0:1",
    "1:60,0.5|", "1:60,0.5|Hi|10|60", "1:60,0.5|Hi|0", "1:60,0.5|Hi|20|50", "1:60,0.5|Hi|5|0|Bye",
    "1:60,0.5|Start|10|0|Later|5|50", "1:20,0.5|Whole block", "1:60,0.5| Spaced | 5 | 2 ", "1:60,0.5|Msg|x",
    "0:30|Free|30|0", "1:60,0.5|a, b: c|10",
]

# whole files, as lists of lines
LINE_FILES = [
    ["T: My Ride ", "0:60,3", "1:300,0.75", ""],
    ["0:30", "1:60,0.5|Start|10|0|Later|5|50", "1:20,0.5|Whole block"],
    ["0:10", "3:2;1:30,1.2|Go|5;0:20|Rest||5"],
    ["T:x", "1:60,0.5", "1:60,9"],
    ["T:A", "T:B"],
    ["2:300,0.5,0.75", "3:4;1:30,1.2,3;1:60,0.6", "1:300,0.5"],
]


def awo_golden():
    files = sorted(glob.glob(os.path.join(APP_DIR, "awoWorkouts", "*.awo")) +
                   glob.glob(os.path.join(APP_DIR, "awoWorkouts", "scratchpad", "*.awo")))
    cases = {"files": [], "lines": [], "lineFiles": [], "ftpTests": []}
    for ftp in (0, 200, 263):
        parser = WorkoutParser(ftp=ftp)
        for path in files:
            parser.rejections = []
            ok = parser.process_workout_file(path.replace("\\", "/"))
            cases["files"].append({"file": os.path.relpath(path, os.path.join(APP_DIR, "awoWorkouts")).replace("\\", "/"),
                                   "ftp": ftp, **parse_result(parser, ok)})
        for line in LINE_CASES:
            ok = parser.parse([line])
            cases["lines"].append({"lines": [line], "ftp": ftp, **parse_result(parser, ok)})
        for lines in LINE_FILES:
            ok = parser.parse(lines)
            cases["lineFiles"].append({"lines": lines, "ftp": ftp, **parse_result(parser, ok)})
    for mode in FtpTest.TESTS:
        for ftpSet in (True, False):
            ftp = 250 if ftpSet else 0
            effort = FtpTest.TESTS[mode].get("effortSeconds", 0)
            parser = WorkoutParser(ftp=ftp, maxBlockSeconds=max(1800, effort))
            lines = FtpTest.workout_lines(mode, ftpSet)
            ok = parser.parse(lines)
            cases["ftpTests"].append({"mode": mode, "ftpSet": ftpSet, "lines": lines,
                                      "maxBlockSeconds": max(1800, effort), "ftp": ftp, **parse_result(parser, ok)})
    write("awo.json", cases)


###Zwift files
ZWO_CASES = [
    """<workout_file><name>Test Ride</name><sportType>bike</sportType><workout>
        <Warmup Duration="300" PowerLow="0.25" PowerHigh="0.75"/><SteadyState Duration="600" Power="0.88"/>
        <FreeRide Duration="120"/><Cooldown Duration="300" PowerLow="0.7" PowerHigh="0.3"/></workout></workout_file>""",
    """<workout_file><name>  Spaced   name </name><workout><steadystate duration="60" power="0.5"/>
        <RAMP DURATION="60" POWERLOW="0.5" POWERHIGH="0.6"/><Ramp Duration="60" PowerLow="0.5" PowerHigh="0.5"/>
        <SteadyState Duration="60" PowerLow="0.5" PowerHigh="0.7"/></workout></workout_file>""",
    """<workout_file><workout><SteadyState Duration="4000" Power="0.6"/><FreeRide Duration="1801"/>
        <Warmup Duration="3600" PowerLow="0.4" PowerHigh="0.8"/><Ramp Duration="5401" PowerLow="0.333" PowerHigh="1.777"/>
        </workout></workout_file>""",
    """<workout_file><workout><IntervalsT Repeat="4" OnDuration="30" OffDuration="60" OnPower="1.2" OffPower="0.5"/>
        <IntervalsT Repeat="3" OnDuration="20" OffDuration="0" PowerOnHigh="1.5" PowerOffLow="0.4"/>
        <IntervalsT Repeat="2" OnDuration="2000" OffDuration="60" OnPower="0.8" OffPower="0.5"/>
        <IntervalsT Repeat="0" OnDuration="20" OffDuration="20" OnPower="1.1" OffPower="0.5"/>
        <IntervalsT Repeat="2.5" OnDuration="20.5" OffDuration="21.5" OnPower="1.1" OffPower="0.5"/>
        </workout></workout_file>""",
    """<workout_file><workout><IntervalsT Repeat="3" OnDuration="30" OffDuration="30" OnPower="1.2" OffPower="0.5">
        <textevent timeoffset="5" message="Go | hard; now"/><textevent timeoffset="40" message="Rest" duration="5"/>
        </IntervalsT><IntervalsT Repeat="2" OnDuration="30" OffDuration="30" OnPower="1.2" OffPower="0.5">
        <textevent timeoffset="70" message="Second rep"/></IntervalsT></workout></workout_file>""",
    """<workout_file><workout><SteadyState Duration="60" Power="0.6"><textevent timeoffset="55" message="End soon"/>
        <textevent timeoffset="90" message="Past the end"/><textevent timeoffset="0" message="  "/>
        <textevent timeoffset="0" duration="0" message="zero duration"/><TextEvent TimeOffset="10" Message="caps"/>
        </SteadyState></workout></workout_file>""",
    """<workout_file><workout><MaxEffort Duration="20"/><SteadyState Duration="60" Power="0.6" Cadence="105"/>
        <SteadyState Duration="60" Power="0.6" Cadence="60"/><SteadyState Duration="60" Power="0.6" Cadence="85"/>
        <FreeRide Duration="30" Cadence="99.5"/><IntervalsT Repeat="2" OnDuration="30" OffDuration="30" OnPower="1.2"
        OffPower="0.5" Cadence="110" CadenceResting="65"/></workout></workout_file>""",
    """<workout_file><name>W</name><ftpOverride>250</ftpOverride><workout><SteadyState Duration="60" Power="0.8"/>
        <Ramp Duration="60" PowerLow="0.5" PowerHigh="1.0"/><SteadyState Duration="60" Power="10"/>
        <SteadyState Duration="60" Power="0.0021"/></workout></workout_file>""",
    """<workout_file><workout><SteadyState Duration="60" Power="5"/><SteadyState Duration="60" Power="0.01"/>
        <SteadyState Duration="60" Power="0.12345"/><SteadyState Duration="60" Power="1.0005"/>
        <SteadyState Duration="60" Power="0.0625"/></workout></workout_file>""",
    """<workout_file><workout><SteadyState Duration="60" Power="0.6"/><SolidState Duration="30"/><Mystery/>
        </workout></workout_file>""",
    """<workout_file><sportType>run</sportType><workout><SteadyState Duration="0" Power="1"/>
        <SteadyState Duration="60" Power="1"/></workout></workout_file>""",
    """<workout_file><workout><SteadyState Duration="60" Power="0.6"></SteadyState></SteadyState>
        <FreeRide Duration="30"/></workout></workout_file>""",
    """<workout_file><ftpOverride> 0 </ftpOverride><workout><FreeRide Duration=" 60.4 "/></workout></workout_file>""",
    # errors
    "not xml at all",
    "<workout_file><workout>",
    "<html><body/></html>",
    "<workout_file><name>x</name></workout_file>",
    "<workout_file><ftpOverride>abc</ftpOverride><workout><FreeRide Duration='60'/></workout></workout_file>",
    "<workout_file><workout><SteadyState Duration='60'/></workout></workout_file>",
    "<workout_file><workout><SteadyState Duration='x' Power='1'/></workout></workout_file>",
    "<workout_file><workout><Ramp Duration='60' PowerLow='0.5'/></workout></workout_file>",
    "<workout_file><workout><IntervalsT Repeat='2' OnDuration='30'/></workout></workout_file>",
    "<workout_file><workout></workout></workout_file>",
]


def zwo_golden():
    cases = []
    with tempfile.TemporaryDirectory() as folder:
        for i, text in enumerate(ZWO_CASES):
            path = os.path.join(folder, f"case{i}.zwo")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            try:
                lines, title, warnings = ZwoConvert.convert(path)
                cases.append({"xml": text, "lines": lines, "title": title, "warnings": warnings, "error": None})
            except ZwoConvert.ZwoError as e:
                cases.append({"xml": text, "lines": None, "title": None, "warnings": None, "error": str(e)})
    write("zwo.json", cases)


###Workout text, grading, FTP tests, session
def text_golden():
    rng = random.Random(1)
    durations = [0, 1, 59, 59.9, 60, 61, 599, 3599, 3600, 3600.5, 3660, 3661, 7322, -5, 86399]
    durations += [rng.randint(0, 20000) for _ in range(200)] + [rng.uniform(0, 20000) for _ in range(50)]
    texts = ["Seated 200 Watts 5m", "Seated 100-200 Watts 1m", "Seated 90 RPM 5m", "Standing 1m", "Watts 5m",
             "Seated abc Watts", "4x", "Seated +5-7 Watts", "Seated 1-2-3 Watts", "Seated - Watts", "RPM", "a Watts RPM"]
    write("text.json", {
        "durations": [[d, WorkoutHelpers.MinimizedDuration(d)] for d in durations],
        "targets": [[t, plain(WorkoutHelpers.TargetsFromText(t))] for t in texts],
    })


def grading_golden():
    rng = random.Random(7)
    scenarios = []
    for i in range(16):
        duration = rng.choice([8, 12, 20, 45, 60, 120, 300])
        free = i % 5 == 4
        ftp = rng.choice([150, 250, 320])
        previous = rng.choice([0, 100, 200, 350])
        target = rng.choice([80, 150, 200, 260, 400, 520]) if not free else 0
        slope = rng.choice([0, 0, 0.5, -0.3])
        ticks = []
        steps = int(round(duration/0.1))
        dropStart = rng.uniform(0, duration)
        dropLength = rng.choice([0, 0, 3, 8])
        resumeAt = rng.choice([None, None, duration*0.4])
        endedAt = rng.choice([None, None, duration*0.7])
        for k in range(steps):
            t = k*0.1
            if endedAt is not None and t >= endedAt:
                break
            tv = target + slope*t
            av = None if dropStart <= t < dropStart + dropLength else max(0.0, tv*rng.uniform(0.6, 1.4))
            ticks.append([t, 0.1, tv, av])
        grader = WorkoutGrading.BlockGrader(duration, previous, ftp, free)
        resumed = False
        for t, dt, tv, av in ticks:
            if resumeAt is not None and not resumed and t >= resumeAt:
                grader.resume(t, tv)
                resumed = True
            grader.add(t, dt, tv, av)
        scenarios.append({"duration": duration, "previous": previous, "ftp": ftp, "free": free, "resumeAt": resumeAt,
                          "endedAt": endedAt, "ticks": ticks, "grade": grader.finish(endedAt)})
    ratios = [[[rng.uniform(0.05, 1.5), rng.uniform(0, 400), rng.uniform(0, 500)] for _ in range(n)] for n in (1, 5, 40)]
    write("grading.json", {
        "scenarios": scenarios,
        "rolling": [{"samples": s, "ratios": WorkoutGrading.rolling_ratios([tuple(x) for x in s])} for s in ratios],
        "letters": [[p, WorkoutGrading.letter(p)] for p in [None, 0, 59.99, 60, 66.999, 97, 100, 100.0001, 150]],
    })


def ftptest_golden():
    rng = random.Random(3)
    messages = []
    for mode in FtpTest.TESTS:
        for start, ftp in ((900, 250), (1020, 0), (60, 333)):
            messages.append({"mode": mode, "start": start, "ftp": ftp, "messages": plain(FtpTest.messages(mode, start, ftp))})
    averages = [[mode, a, FtpTest.ftp_from_average(mode, a)] for mode in FtpTest.TESTS
                for a in (0, 100, 251.6, 263.15789473684214, 300, 333.3333, 412.5)]
    rolling = []
    for window in (10, 60):
        best = FtpTest.BestRollingAverage(window)
        effort = FtpTest.EffortAverage()
        steps = []
        for k in range(400):
            seconds = rng.choice([0.1, 0.1, 0.1, 0.25, 0, -0.1])
            power = None if rng.random() < 0.05 else rng.uniform(0, 600)
            if k == 200:
                best.start_new_window()
                steps.append(["new"])
            best.add(seconds, power)
            effort.add(seconds, power)
            steps.append([seconds, power, best.average(), effort.average()])
        rolling.append({"window": window, "steps": steps})
    write("ftptest.json", {"messages": messages, "averages": averages, "rolling": rolling})


def session_golden():
    rng = random.Random(5)
    perSecond = [[rng.uniform(0, 30), rng.uniform(0, 500), None if rng.random() < 0.3 else rng.uniform(40, 120)]
                 for _ in range(200)]
    rings = []
    for case in range(4):
        def readings(rate, gaps):
            times, values, t = [], [], rng.uniform(0, 1)
            while t < 120:
                if not any(a <= t < b for a, b in gaps):
                    times.append(t)
                    values.append(rng.uniform(0, 400))
                t += rate
            return times, values
        power = readings(0.25, [(30, 36), (80, 81)] if case % 2 else [])
        cadence = readings(0.5, [(50, 70)] if case > 1 else [])
        hasCadence = case != 3
        rings.append({"length": 121.7, "power": power, "cadence": cadence, "hasCadence": hasCadence,
                      "ring": plain(Session.ring_from_readings(121.7, power, cadence, hasCadence))})
    write("session.json", {"perSecond": {"samples": perSecond, "result": plain(Session.per_second(perSecond))},
                           "rings": rings})


###Colors
def colors_golden():
    rng = random.Random(11)
    hexes = ["#000000", "#ffffff", "#808080", "#ff0000", "#00ff00", "#0000ff", "#28275e", "#9fc5d1"]
    hexes += ["#%06x" % rng.randrange(0x1000000) for _ in range(60)]
    themes = sorted(glob.glob(os.path.join(APP_DIR, "themes", "colors", "*.json")))
    gradients = sorted(glob.glob(os.path.join(APP_DIR, "themes", "gradients", "*.json")))
    palettes = []
    zoneValues = []
    for theme in [None] + themes:
        colors = {}
        if theme:
            with open(theme, encoding="utf-8") as f:
                colors = {k.lower(): v.lower() for k, v in json.load(f)["colors"].items()}
        backgrounds = FtpColors.theme_backgrounds(colors)
        for gradient in [None] + gradients:
            zones = FtpColors.file_palette(AppTheme._read(gradient)["ftpColors"]) if gradient else \
                [FtpColors.DEFAULTS["zones"][z] for z in FtpColors.ZONES]
            fitted = FtpColors.palette(zones, colors)
            palettes.append({"theme": os.path.basename(theme) if theme else None,
                             "gradient": os.path.basename(gradient) if gradient else None,
                             "themeColors": colors, "backgrounds": list(backgrounds), "zones": zones,
                             "palette": plain(fitted)})
    # zone colors along the power and cadence zones, on the default palette and two themes'
    for index in (0, 1, len(palettes) - 1):
        entry = palettes[index]
        fitted = tuple(tuple(z) for z in entry["palette"])
        for cadence in (False, True):
            AppState.cadenceMode = AppState.workoutMode = cadence
            ranges = FtpColors.cadence_ranges(FtpColors.CADENCE_SCALE_BOTTOM_RPM) if cadence else [72, 174, 238, 374]
            AppState.ftpRanges[:] = ranges
            values = [rng.uniform(0, ranges[-1]*1.6) for _ in range(40)] + [0, ranges[0], ranges[-1], 1e6]
            zoneValues.append({"palette": index, "cadence": cadence, "ranges": list(ranges),
                               "hex": [[v, FtpColors.zone_hex(v, ranges, fitted)] for v in values],
                               "pale": [FtpColors._blend(fitted[z][0], fitted[z][1], FtpColors.fade_from(ranges))
                                        if FtpColors.fade_from(ranges) else fitted[z][0] for z in range(5)],
                               "stops": [[v, c.name()] for v, c in _stops(ranges, fitted)],
                               "graph": [[v, [FtpColors.graph_color(v, ranges, state).name(QColorArgb)
                                              for state in ("paused", "upcoming", "past")]] for v in values[:10]]
                               if index == 0 else []})
    AppState.cadenceMode = AppState.workoutMode = False
    write("colors.json", {
        "oklch": [[h, list(FtpColors.to_oklch(h))] for h in hexes],
        "fromOklch": [[L, C, h, FtpColors.from_oklch(L, C, h)] for L, C, h in
                      [(rng.uniform(-0.1, 1.1), rng.uniform(0, 0.4), rng.uniform(-4, 4)) for _ in range(80)]],
        "mix": [[a, b, t, FtpColors.mix(a, b, t)] for a, b, t in
                [(rng.choice(hexes), rng.choice(hexes), rng.choice([0, 0.25, 0.5, 0.85, 1, rng.random()])) for _ in range(80)]],
        "difference": [[a, b, FtpColors.difference(a, b)] for a, b in [(rng.choice(hexes), rng.choice(hexes)) for _ in range(40)]],
        "halo": [[h, FtpColors.halo_color(h).name(QColorArgb)] for h in hexes],
        "cadenceRanges": [[low, FtpColors.cadence_ranges(low)] for low in (40, 60, 90, 112, 200)],
        "palettes": palettes,
        "zoneValues": zoneValues,
    })


def _stops(ranges, fitted):
    original = FtpColors.palette
    FtpColors.palette = lambda *a, **k: fitted
    try:
        return FtpColors.zone_stops(ranges)
    finally:
        FtpColors.palette = original


###Units and calibration
def units_golden():
    rng = random.Random(13)
    weights = [0, 20, 70, 70.26, 75, 165/Units.LBS_PER_KG, 300] + [rng.uniform(20, 300) for _ in range(60)]
    result = {"weights": [], "powerToWeight": [], "calibration": []}
    for unit in ("lbs", "kg"):
        Units.current.weight = unit
        result["weights"] += [[unit, kg, Units.format_weight(kg)] for kg in weights]
    Units.current.weight = "lbs"
    for powerWeight in ("W/kg", "W/lb"):
        Units.current.powerWeight = powerWeight
        Units.current.riderWeightKg = 72.5
        result["powerToWeight"] += [[powerWeight, w, Units.power_to_weight_text(w)] for w in (0, 1, 250, 333.3, 1000)]
    Units.current.riderWeightKg = 0.0
    for _ in range(60):
        calibration = {"offset_watts": round(rng.uniform(-10, 10), 1), "scale": round(rng.uniform(0.75, 1.25), 4)}
        watts = rng.randint(-5, 1500)
        result["calibration"].append([watts, calibration, PowerCalibration.apply(watts, calibration),
                                      PowerCalibration.describe(calibration)])
    write("units.json", result)


def plot_golden():
    """pyqtgraph's ticks (spacing, values, numbers) and view ranges (padding, limits), which awo_plot copies"""
    from PyQt6.QtWidgets import QApplication
    app = QApplication.instance() or QApplication([])
    import pyqtgraph as pg
    from ClassWorkoutGraph import TimeAxisItem
    ranges = [(0, 720, 1800), (-16.98, 737.4, 1799), (0, 1000, 160), (0, 400, 300), (0, 3.3, 500), (-5, 5, 100),
              (0, 7200, 1800), (100, 130, 800), (0, 0.004, 400), (0, 1e6, 600), (0, 25000, 900), (12.5, 12.75, 50),
              (0, 220, 160), (-8.2, 408.2, 157)]
    axis = pg.AxisItem("bottom")
    timeAxis = TimeAxisItem(orientation="bottom")
    ticks = []
    for mn, mx, size in ranges:
        levels = axis.tickValues(mn, mx, size)
        ticks.append({"min": mn, "max": mx, "size": size,
                      "spacing": [list(level) for level in axis.tickSpacing(mn, mx, size)],
                      "values": [[spacing, list(values)] for spacing, values in levels],
                      "strings": [axis.tickStrings(values, 1.0, spacing) for spacing, values in levels]})
    times = [[spacing, values, timeAxis.tickStrings(values, 1.0, spacing)]
             for spacing, values in ((60, [0, 60, 540, 3540, 3600, 3660, 7260]), (30, [0, 30, 90, 3570, 3630]),
                                     (600, [0, 600, 4200]))]
    views = []
    for width, height in ((1798.5, 160.1), (300, 100), (50, 20), (0, 0), (10000, 5000)):
        for (a, b), padding, limits in (((0, 720), None, None), ((0, 400), None, None), ((5, 5), None, None),
                                        ((0, 720), 0, None), ((-100, 50), 0, {"xMin": 0, "xMax": 3600}),
                                        ((3000, 4000), None, {"xMin": 0, "xMax": 3600, "minXRange": 60}),
                                        ((10, 20), 0, {"xMin": 0, "xMax": 3600, "minXRange": 60}),
                                        ((-50, 9000), None, {"xMin": 0, "xMax": 3600})):
            view = pg.ViewBox()
            view.resize(width, height)
            if limits:
                view.setLimits(**limits)
            view.setXRange(a, b, padding=padding)
            view.setYRange(a, b, padding=padding)
            views.append({"size": [width, height], "range": [a, b], "padding": padding, "limits": limits,
                          "x": view.viewRange()[0], "y": view.viewRange()[1],
                          "suggested": [view.suggestPadding(0), view.suggestPadding(1)]})
    write("plot.json", {"ticks": ticks, "times": times, "views": views})
    del app


#the rides hud_golden() drives the Python workout screen through: a workout (its .awo text) or an FTP test, at an FTP,
#for so many seconds with PreviewRide's Rider(seed); events: [tick, "difficulty", value] (the slider) or
#[tick, "pause"] (the pause button), each just before that tick
HUD_RIDES = [
    {"name": "mixed", "ftp": 250, "seed": 3, "seconds": 760,
     "workout": "T:Golden Ride\n0:60|Warm up\n1:120,0.6\n2:120,0.5,0.9\n3:3;1:30,1.2;0:30\n"
                "3:2;1:20,1.0;2:20,0.6,0.8;0:10|Spin\n1:60,250W\n3:2;1:30,1.1\n2:40,0.7,0.7\n",
     "events": [[150, "difficulty", 3], [250, "pause"], [280, "pause"], [400, "difficulty", -2], [620, "difficulty", 0]]},
    {"name": "ftp20m", "ftp": 220, "seed": 5, "seconds": 2800, "ftpTest": "20m", "events": []},
    {"name": "ramp10", "ftp": 0, "seed": 7, "seconds": 1500, "ftpTest": "ramp10", "events": []},
    #Musical Cadence Mode: music.json's click tracks (their analyses), the music silent and a second a tick
    {"name": "cadence", "ftp": 250, "seed": 9, "seconds": 130, "preferred": 85, "minSegment": 8,
     "music": [["a_100", None], ["b_75", "Slow One"], ["c_gap", None]],
     "events": [[50, "pause"], [56, "pause"]]},
]


def hud_golden():
    """The Python workout screen ridden by PreviewRide's Rider a second a tick (its clock frozen, so every tick is
    exactly a second): the power it asked for, each block's grade, and an FTP test's result"""
    from types import SimpleNamespace
    from PyQt6.QtWidgets import QApplication
    app = QApplication.instance() or QApplication([])
    import time as timeModule
    import ClassHUDWindow
    import PreviewRide
    AppTheme.install()
    clock = 1000.0
    realMonotonic = timeModule.monotonic
    timeModule.monotonic = lambda: clock
    results = []
    try:
        for ride in HUD_RIDES:
            PreviewRide.reset_ride_state()
            AppState.ftp = str(ride["ftp"])
            AppState.maxHr, AppState.wheelCircumference = "185", "2.105"
            AppState.workoutMode, AppState.cadenceMode = True, bool(ride.get("music"))
            AppState.ftpTestMode = ride.get("ftpTest")
            AppState.antHRM, AppState.antSpeedCadence, AppState.antCadence, AppState.antSpeed = True, True, False, False
            AppState.ftpRanges = FtpColors.power_ranges()
            FtpColors.update_graph_brushes()
            AppState.workoutMessages = []
            if AppState.ftpTestMode:
                test = FtpTest.TESTS[AppState.ftpTestMode]
                parser = WorkoutParser(ftp=ride["ftp"], maxBlockSeconds=max(1800, test.get("effortSeconds", 0)))
                AppState.ftp = str(ride["ftp"])
                assert parser.parse(FtpTest.workout_lines(AppState.ftpTestMode, AppState.ftp != "0"), test["name"])
                if FtpTest.is_ramp(AppState.ftpTestMode):
                    effortStart = AppState.workoutDurationSeconds
                    first = FtpTest.ramp_block(AppState.ftpTestMode, 0)
                    AppState.workoutList.append(first)
                    AppState.workoutDurationSeconds += first[1]
                else:
                    effortStart = AppState.workoutDurationSeconds - test["effortSeconds"]
                AppState.workoutMessages.extend(FtpTest.messages(AppState.ftpTestMode, effortStart, AppState.ftp))
            elif ride.get("music"):
                #as the setup screen's build_music_workout builds it
                import MusicCadence
                with open(os.path.join(HERE, "music.json"), encoding="utf-8") as f:
                    musicTracks = {t["name"]: t["analysis"] for t in json.load(f)["tracks"]}
                songs = [("C:\\music\\" + name + ".wav", title) for name, title in ride["music"]]
                analyses = {path: musicTracks[name] for (path, _), (name, _) in zip(songs, ride["music"])}
                blocks, tracks, songStarts, pulses = MusicCadence.build_workout(songs, analyses, ride["preferred"],
                                                                                ride["minSegment"])
                AppState.workoutList = [(0, d, 0) if rpm == 0 else (1, d, (rpm,)*21, 0) for d, rpm in blocks]
                AppState.workoutDurationSeconds = sum(d for d, _ in blocks)
                AppState.musicTracks = tracks
                AppState.musicSongStarts = {i: MusicCadence.song_header(tracks[t]) for i, t in songStarts.items()}
                AppState.musicPulses = pulses
                for _, title, start, duration in tracks:
                    messageStart = int(start)
                    messageEnd = min(messageStart + 8, AppState.workoutDurationSeconds)
                    if messageEnd > messageStart:
                        AppState.workoutMessages.append((messageStart, messageEnd, "Now playing: " + title))
                AppState.workoutTitle = "Musical Cadence Mode"
                AppState.ftpRanges[:] = FtpColors.cadence_ranges(FtpColors.CADENCE_SCALE_BOTTOM_RPM)
                FtpColors.update_graph_brushes()
            else:
                parser = WorkoutParser(ftp=ride["ftp"])
                assert parser.parse(ride["workout"].split("\n")[:-1]), parser.rejections
            AppState.workoutFilename = AppState.workoutFilenameShort = "golden.awo"
            #(the title bar's song, in Musical Cadence Mode)
            hud = ClassHUDWindow.HUDWindow(SimpleNamespace(startExe=PreviewRide.FakeBridge(), setWindowTitle=lambda t: None))
            hud.timer.stop()
            hud.smoothBarTimer.stop()
            hud.starTimer.stop()
            hud.hud_graph_data_update = lambda: None
            rider = PreviewRide.Rider(seed=ride["seed"])
            events = {}
            for event in ride["events"]:
                events.setdefault(event[0], []).append(event[1:])
            if hud.cadenceMode:
                hud.music_start = lambda: setattr(hud, "musicPlayer", PreviewRide.ViewerMusicPlayer())
            hud.start_workout()
            ticks = 0
            for tick in range(int(ride["seconds"])):
                if not hud.startedBool:
                    break
                for event in events.get(tick, []):
                    if event[0] == "difficulty":
                        hud.difficultySlider.setValue(event[1])
                    else:
                        hud.pause_recording()
                hud.lastTimestamp = clock - 1
                if hud.cadenceMode and hud.musicPlayer is not None:
                    hud.musicPlayer.seconds += 1
                hud.process_events()
                t = AppState.secondsPassed
                reading = rider.sample(t, hud.currentPower, hud.cadenceMode)
                PreviewRide.save_reading(t, reading)
                ticks += 1
            grades = {f"{k[0]},{k[1]},{k[2]}": plain(v) for k, v in AppState.blockGrades.items()}
            results.append({"ride": ride, "ticks": ticks,
                            "requestSeconds": list(AppState.workoutPowerRequestsSeconds),
                            "requestWatts": list(AppState.workoutPowerRequestsWatts),
                            "grades": grades, "gradeOrder": list(grades),
                            "ftpTestResult": plain(AppState.ftpTestResult),
                            "musicPlayed": plain(AppState.musicPlayed),
                            "workoutSeconds": AppState.workoutDurationSeconds,
                            "workoutList": plain(AppState.workoutList),
                            "secondsPassed": AppState.secondsPassed,
                            "workoutSecondsPassed": hud.workoutSecondsPassed,
                            "activeWorkout": hud.activeWorkout,
                            "completed": hud.workoutCompleteShown})
            hud.stop_timers()
            hud.deleteLater()
    finally:
        timeModule.monotonic = realMonotonic
    write("hud.json", results)
    del app


###Musical Cadence Mode (MusicCadence.py)
#click tracks made from Python's random (gauss), so the C++ test (tst_music) makes the same wav files byte for byte:
#(name, bpm, seconds, seed, silence (start, end) or None, change (from seconds, to bpm) or None)
MUSIC_TRACKS = [
    ("a_100", 100, 40, 1, None, None),
    ("b_75", 75, 40, 2, None, None),
    ("c_gap", 100, 40, 3, (15, 24), None),
    ("d_change", 90, 60, 4, None, (30, 128)),
    ("e_172", 172, 20, 5, None, None),
]
MUSIC_RATE = 22050


def golden_click_track(path, bpm, seconds, seed, silence=None, change=None, rate=MUSIC_RATE):
    """A 16 bit mono wav of a click every beat (a decaying noise burst) over a little noise"""
    import math
    import struct
    import wave
    rng = random.Random(seed)
    n = int(seconds*rate)
    samples = [0.0]*n
    length = int(0.03*rate)
    burst = [rng.gauss(0, 1)*math.exp(-8*i/(length - 1)) for i in range(length)]
    t = 0.0
    beat = 60.0/bpm
    while t < seconds:
        if change and t >= change[0]:
            beat = 60.0/change[1]
        start = int(t*rate)
        for i in range(min(length, n - start)):
            samples[start + i] += 0.6*burst[i]
        t += beat
    for i in range(n):
        samples[i] += 0.002*rng.gauss(0, 1)
    if silence:
        for i in range(int(silence[0]*rate), int(silence[1]*rate)):
            samples[i] = 0.0
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % n, *(int(max(-1.0, min(1.0, s))*32767) for s in samples)))
    return path


def music_golden():
    import zlib
    import numpy as np
    import MusicCadence as M
    folder = tempfile.mkdtemp()
    tracks = []
    paths = {}
    for name, bpm, seconds, seed, silence, change in MUSIC_TRACKS:
        path = golden_click_track(os.path.join(folder, name + ".wav"), bpm, seconds, seed, silence, change)
        paths[name] = path
        with open(path, "rb") as f:
            crc = zlib.crc32(f.read())
        samples = M.decode(path)
        envelope = M.onset_envelope(samples)
        classes, confidences = M.tempo_track(envelope)
        beats = M.track_beats(envelope, classes)
        analysis = M.analyze_file(path)
        tracks.append({
            "name": name, "bpm": bpm, "seconds": seconds, "seed": seed, "silence": silence, "change": change,
            "crc": crc,
            "samples": {"count": len(samples), "sum": float(np.sum(samples.astype(np.float64))),
                        "first": [float(x) for x in samples[:64]], "at": [float(samples[i]) for i in range(0, len(samples), 9973)]},
            "envelope": [float(x) for x in envelope],
            "classes": [float(c) for c in classes], "confidences": [float(c) for c in confidences],
            "beats": [int(b) for b in beats],
            "analysis": analysis,
            "segments": [[s, e, b] for s, e, b in M.song_segments(analysis)],
            "segments4": [[s, e, b] for s, e, b in M.song_segments(analysis, 4)],
            "cadence": M.song_cadence(analysis),
            "cadence100": M.song_cadence(analysis, preferred=100),
        })
    analyses = {paths[t["name"]]: t["analysis"] for t in tracks}
    order = [(paths["a_100"], None), (paths["b_75"], "Slow One"), (paths["c_gap"], None), (paths["d_change"], None),
             (paths["e_172"], "Fast")]
    workouts = []
    for preferred, minSegment in ((85, 8), (100, 4), (60, 16)):
        blocks, rideTracks, songStarts, pulses = M.build_workout(order, analyses, preferred, minSegment)
        workouts.append({"preferred": preferred, "minSegment": minSegment, "blocks": plain(blocks),
                         "tracks": [[os.path.basename(p), title, start, duration] for p, title, start, duration in rideTracks],
                         "songStarts": {str(k): v for k, v in songStarts.items()}, "pulses": [float(p) for p in pulses],
                         "headers": [M.song_header(t) for t in rideTracks]})
    #random picks (Python's random, seeded): (songs, target, durations, seed, cadences)
    picks = []
    songs = [f"s{i}" for i in range(14)]
    durations = {song: 60 + 37*(i % 6) for i, song in enumerate(songs)}
    durations["s13"] = 0
    cadences = {song: (None if i % 5 == 0 else 55 + 7*i) for i, song in enumerate(songs)}
    for seed in range(6):
        for target in (300, 900, 2000):
            picks.append({"seed": seed, "target": target, "cadences": False,
                          "picked": M.random_selection(songs, target, durations, random.Random(seed))})
            picks.append({"seed": seed, "target": target, "cadences": True,
                          "picked": M.random_selection(songs, target, durations, random.Random(seed), cadences)})
    folds = [[bpm, previous, preferred, M.fold_rpm(bpm, previous, preferred)]
             for bpm in (33.3, 45, 59.9, 60, 77.7, 100, 119.9, 121, 160, 200, 250, 333)
             for previous in (None, 50, 64, 110) for preferred in (60, 85, 110)]
    write("music.json", {"rate": MUSIC_RATE, "cacheVersion": M.cache_version(), "tracks": tracks, "workouts": workouts,
                         "picks": {"songs": songs, "durations": durations, "cadences": cadences, "cases": picks},
                         "folds": folds})


if __name__ == "__main__":
    from PyQt6.QtGui import QColor
    QColorArgb = QColor.NameFormat.HexArgb
    awo_golden()
    zwo_golden()
    text_golden()
    grading_golden()
    ftptest_golden()
    session_golden()
    colors_golden()
    units_golden()
    plot_golden()
    music_golden()
    hud_golden()
