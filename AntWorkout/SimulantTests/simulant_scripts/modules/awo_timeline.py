# AntWorkout hardware test timeline, shared by the SimulANT+ sensor scripts (IronPython 2.7) and the
# AntWorkout-side harness (run_simulant_test.py, Python 3) - so keep it Python 2 compatible: no f-strings.
#
# SimulANT+'s IronPython has no standard library (no json, os, ...), so only builtins here, and .NET for the
# clock if its time module is missing too.
#
# The harness writes CONTROL_FILE, one key=value per line: run=<folder for this run's logs>, scenario=<SCENARIOS
# key>, epoch=<seconds since 1970 the timeline starts at, or "none" until the devices are connected>. Until
# there's an epoch every sensor transmits its "pairing" values so the harness can find and connect to it; from
# the epoch on each script follows its column of the scenario, however long after the others it was started.

try:
    import time as _time

    def now():
        return _time.time()
except ImportError:
    from System import DateTime, DateTimeKind

    _UNIX_EPOCH = DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)

    def now():
        return (DateTime.UtcNow - _UNIX_EPOCH).TotalSeconds

#where the harness and the scripts meet; the harness checks this matches its own folder
RUN_DIR = r"C:\Users\gfinl\Documents\Awooo\AntWorkout\SimulantTests\run"
CONTROL_FILE = RUN_DIR + "\\control.txt"

#wheel circumference both sides use, so speed can be compared exactly
WHEEL_MM = 2100

#each phase: (name, seconds, {device: values, or None for switched off}); a device missing from a phase keeps
#the values it had. Devices: "pm" power meter, "sc" combined speed/cadence, "hr" heart rate, "fe" FE-C trainer.
#pm: power W, cadence rpm; sc: cadence rpm, speed km/h; hr: bpm; fe: cadence rpm, speed km/h, free W (the
#power it reports while not held at a target, see awo_trainer.py)
RIDE = {"pm": {"power": 200, "cadence": 85}, "sc": {"cadence": 85, "speed": 30.0}, "hr": {"hr": 130}}
PAIRING = {"pm": {"power": 180, "cadence": 80}, "sc": {"cadence": 80, "speed": 28.0}, "hr": {"hr": 110},
           "fe": {"cadence": 85, "speed": 30.0, "free": 120}}


def _merge(*parts):
    out = {}
    for part in parts:
        out.update(part)
    return out


SCENARIOS = {
    #power meter + combined speed/cadence + heart rate: readings, stopping pedaling, and each sensor dropping out
    "sensors": [
        ("ride",          40, RIDE),
        ("harder",        30, {"pm": {"power": 300, "cadence": 95}, "sc": {"cadence": 95, "speed": 36.0},
                               "hr": {"hr": 150}}),
        ("stopped",       35, {"pm": {"power": 0, "cadence": 0}, "sc": {"cadence": 0, "speed": 0.0},
                               "hr": {"hr": 120}}),
        ("resume",        30, RIDE),
        ("sc dropout",    25, {"pm": {"power": 220, "cadence": 88}, "sc": None, "hr": {"hr": 135}}),
        ("sc back",       25, {"sc": {"cadence": 88, "speed": 31.0}}),
        ("hr dropout",    25, {"hr": None}),
        ("hr back",       20, {"hr": {"hr": 140}}),
        ("pm dropout",    25, {"pm": None, "sc": {"cadence": 85, "speed": 30.0}}),
        ("pm back",       30, _merge(RIDE)),
    ],
    #FE-C trainer (+ heart rate): target power commands, release, a trainer dropout, the stop at the end
    "trainer": [
        ("target 150",    30, {"fe": {"cadence": 85, "speed": 30.0, "free": 120}, "hr": {"hr": 130}}),
        ("target 250",    30, {}),
        ("target 100",    30, {}),
        ("released",      25, {}),
        ("target 200",    25, {}),
        ("fe dropout",    20, {"fe": None}),
        ("fe back",       25, {"fe": {"cadence": 85, "speed": 30.0, "free": 120}}),
        ("stopped",       20, {}),
    ],
}

#what the harness does to the trainer in each "trainer" phase: watts on the free mode slider, "release", or "stop"
TRAINER_ACTIONS = {"target 150": 150, "target 250": 250, "target 100": 100, "released": "release",
                   "target 200": 200, "fe dropout": 200, "fe back": 200, "stopped": "stop"}


def phases(scenario):
    """[(name, start, end, {device: values or None})] with each device's values carried forward"""
    out = []
    current = {}
    start = 0.0
    for name, seconds, changes in SCENARIOS[scenario]:
        current = dict(current)
        current.update(changes)
        out.append((name, start, start + seconds, current))
        start += seconds
    return out


def total_seconds(scenario):
    return sum(seconds for _, seconds, _ in SCENARIOS[scenario])


def read_control():
    """{"run", "scenario", "epoch" (a float, or None before the timeline starts)}, or None without a control file"""
    try:
        f = open(CONTROL_FILE)
        try:
            text = f.read()
        finally:
            f.close()
    except (IOError, OSError):
        return None
    control = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            control[key.strip()] = value.strip()
    if "run" not in control or "scenario" not in control:
        return None
    try:
        control["epoch"] = float(control.get("epoch"))
    except (TypeError, ValueError):
        control["epoch"] = None
    return control


def control_text(control):
    epoch = control.get("epoch")
    return "run=%s\nscenario=%s\nepoch=%s\n" % (control["run"], control["scenario"],
                                                 "none" if epoch is None else "%.3f" % epoch)


def values_text(values):
    if values is None:
        return "off"
    return ";".join("%s=%s" % (key, values[key]) for key in sorted(values))


class Script(object):
    """One SimulANT+ simulator's side: what it should transmit now, and a log of what it did"""

    def __init__(self, device):
        self.device = device
        self.control = None
        self.logged = None
        self.lastHeartbeat = 0.0

    def now(self):
        """(phase name, values or None for off); "pairing" before the harness starts the timeline"""
        control = read_control()
        if control != self.control:
            self.control = control
            self.log("control", control_text(control).replace("\n", " ") if control else "none")
        if not control or control.get("epoch") is None or control.get("scenario") not in SCENARIOS:
            return "pairing", PAIRING.get(self.device)
        t = now() - control["epoch"]
        if t < 0:
            return "pairing", PAIRING.get(self.device)
        for name, start, end, values in phases(control["scenario"]):
            if t < end:
                return name, values.get(self.device, PAIRING.get(self.device))
        return "done", phases(control["scenario"])[-1][3].get(self.device, PAIRING.get(self.device))

    def log(self, event, detail=""):
        control = self.control or read_control()
        if not control or not control.get("run"):
            return
        try:
            f = open(control["run"] + "\\simulant_" + self.device + ".csv", "a")
            try:
                f.write("%.3f,%s,%s\n" % (now(), event, str(detail).replace(",", ";")))
            finally:
                f.close()
        except (IOError, OSError):
            pass

    def state(self, phase, values):
        """Logs what's being transmitted when it changes, and every 5s"""
        key = (phase, values_text(values))
        if key != self.logged or now() - self.lastHeartbeat >= 5:
            self.logged = key
            self.lastHeartbeat = now()
            self.log("tx", phase + "|" + values_text(values))
