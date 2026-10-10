"""
FtpTest.py

The FTP tests, picked with their own buttons on the setup screen (AppState.ftpTestMode). Each is an ungraded workout
built by the setup screen like an .awo file, starting with the same warmup. Then either:

  effort  (20m, 1h)          one free ride effort. While it's ridden the HUD's spiral shows its average power (95%
                             of it for the 20m test, the FTP it works out to)
  ramp    (10W/20W delta)    1m steady blocks, each a step up from the last, added by the HUD 10s before the one being
                             ridden ends, until the rider stops pedaling for 10s. The spiral shows 75% of the best 60s
                             average power ridden in the ramp

The synopsis offers the FTP the test works out to as the new FTP.
"""

from collections import deque

from AppState import AppState

#warmup, as .awo lines: 3m at 50% FTP, 1m at 100%, 2m at 60%, 1m at 110%, then an 8m ramp from 60% to 80%
WARMUP_LINES = ["1:180,0.5", "1:60,1.0", "1:120,0.6", "1:60,1.1", "2:480,0.6,0.8"]
#with no FTP set (only a ramp test can be ridden without one) the warmup is a free ride as long
WARMUP_SECONDS = sum(int(line.split(":")[1].split(",")[0]) for line in WARMUP_LINES)

#mode (AppState.ftpTestMode): what kind of test, its name, the fraction of the power it measures that's the new FTP,
#and the spiral's heading over that FTP.
#effort tests: the effort's seconds and messages shown (<seconds before the effort starts>, <seconds shown>, <text>)
#on top of the ones every effort test has (see messages)
#ramp tests: the first block's watts and each block's step up, and the FTP the power zones are colored by while no
#FTP is set (see FtpColors.zone_ftp)
TESTS = {
    "20m": {"kind": "effort", "name": "20m FTP Test", "ftpFactor": 0.95, "averageTitle": "20m Avg:", "effortSeconds": 1200,
            "extraMessages": [(60, 30, "Average shown above will be 95% of the true average to match your new FTP over the 20m")]},
    "1h": {"kind": "effort", "name": "1h FTP Test", "ftpFactor": 1.0, "averageTitle": "Avg:", "effortSeconds": 3600,
           "extraMessages": []},
    "ramp10": {"kind": "ramp", "name": "10W delta FTP Ramp", "ftpFactor": 0.75, "averageTitle": "Ramp FTP:",
               "startWatts": 50, "stepWatts": 10, "unsetFtpWatts": 50},
    "ramp20": {"kind": "ramp", "name": "20W delta FTP Ramp", "ftpFactor": 0.75, "averageTitle": "Ramp FTP:",
               "startWatts": 100, "stepWatts": 20, "unsetFtpWatts": 100},
}

#how long "GO!" and the two minutes left message stay up
GO_SECONDS = 10
TWO_MINUTES_LEFT_SECONDS = 20

#ramp tests: each block's length, how long before it ends the next one is added, the best average power it measures,
#and not pedaling (below this many watts, or no power reading): how long before the warning, and before the test ends
RAMP_BLOCK_SECONDS = 60
RAMP_ADD_BEFORE_SECONDS = 10
RAMP_BEST_SECONDS = 60
NOT_PEDALING_WATTS = 10
NOT_PEDALING_WARN_SECONDS = 5
NOT_PEDALING_END_SECONDS = 10
NOT_PEDALING_MESSAGE = "Done yet? Keep pedaling to continue..."
#a ramp paused for longer than this starts over from its first block when it's unpaused
RAMP_PAUSE_SECONDS = 60


def test():
    """The selected test's TESTS entry, None outside an FTP test"""
    return TESTS.get(AppState.ftpTestMode)


def is_ramp(mode):
    return TESTS[mode]["kind"] == "ramp"


def workout_lines(mode, ftpSet=True):
    """The test as .awo lines (a ramp test's blocks are added after, see ramp_block); the warmup is a free ride
    while no FTP is set"""
    test = TESTS[mode]
    lines = ["T:" + test["name"]] + (WARMUP_LINES if ftpSet else ["0:%d" % WARMUP_SECONDS])
    if test["kind"] == "effort":
        lines.append("0:%d" % test["effortSeconds"])
    return lines


def ramp_block(mode, index):
    """A ramp test's index'th block (0 is the first) as an AppState.workoutList steady block, the same watts at every
    difficulty (the ramp doesn't follow FTP), seated"""
    test = TESTS[mode]
    watts = test["startWatts"] + index*test["stepWatts"]
    return (1, RAMP_BLOCK_SECONDS, (watts,)*21, 0)


def messages(mode, effortStart, ftp):
    """The test's timed messages, (start, end, text) in workout seconds like AppState.workoutMessages; effortStart is
    when the warmup ends"""
    test = TESTS[mode]
    if test["kind"] == "ramp":
        return [(effortStart - 30, effortStart - 15, f"Get ready for your {test['stepWatts']}W delta ramp test..."),
                (effortStart - 15, effortStart, "Stop pedaling for 10s to end the test and start your cooldown.")]
    effortName = test["name"].split(" ")[0]
    effortEnd = effortStart + test["effortSeconds"]
    timed = [(effortStart - before, effortStart - before + seconds, text) for before, seconds, text in test["extraMessages"]]
    timed += [(effortStart - 30, effortStart - 20, f"Get ready for your {effortName} effort"),
              (effortStart - 20, effortStart - 10, f"Your set FTP is currently: {ftp}, shoot for slightly higher than that"),
              (effortStart - 10, effortStart, "Get Ready..."),
              (effortStart, effortStart + GO_SECONDS, "GO!"),
              (effortEnd - 120, effortEnd - 120 + TWO_MINUTES_LEFT_SECONDS,
               "Two minutes left! If you kept the previous power, now is the chance to push it!")]
    return timed


def ftp_from_average(mode, average):
    """The FTP the power a test measures works out to"""
    return int(round(average*TESTS[mode]["ftpFactor"]))


class EffortAverage:
    """Time weighted average power over the effort; time with no power reading (sensor dropout) isn't counted"""
    def __init__(self):
        self.wattSeconds = 0.0
        self.seconds = 0.0

    def add(self, seconds, power):
        if power is None or seconds <= 0:
            return
        self.wattSeconds += power*seconds
        self.seconds += seconds

    def average(self):
        return None if self.seconds <= 0 else self.wattSeconds/self.seconds


class BestRollingAverage:
    """The best time weighted average power over any `window` seconds ridden; time with no power reading counts as 0
    (in a ramp test that's not pedaling). None until `window` seconds have been ridden"""
    def __init__(self, window=RAMP_BEST_SECONDS):
        self.window = window
        self.samples = deque() #(seconds, power)
        self.wattSeconds = 0.0
        self.seconds = 0.0
        self.best = None

    def add(self, seconds, power):
        if seconds <= 0:
            return
        power = power or 0
        self.samples.append((seconds, power))
        self.wattSeconds += power*seconds
        self.seconds += seconds
        #drop the oldest while the rest still cover the window
        while self.samples and self.seconds - self.samples[0][0] >= self.window:
            oldSeconds, oldPower = self.samples.popleft()
            self.seconds -= oldSeconds
            self.wattSeconds -= oldSeconds*oldPower
        if self.seconds >= self.window:
            average = self.wattSeconds/self.seconds
            self.best = average if self.best is None else max(self.best, average)

    def start_new_window(self):
        """Forgets the time ridden so far, keeping the best: the next `window` seconds don't reach back past here"""
        self.samples.clear()
        self.wattSeconds = 0.0
        self.seconds = 0.0

    def average(self):
        return self.best
