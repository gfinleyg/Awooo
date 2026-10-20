"""
WorkoutGrading.py

Grades each workout block on how closely the power ridden followed the workout's target power.

HUDWindow feeds a BlockGrader every 100ms tick while the workout runs (paused time never reaches it),
with the target power at that moment (ramp steps and difficulty changes included) and the latest power
reading, or None while the power sensor has dropped out. Dropouts are left out of the grade entirely.
When the block ends, finish() returns its grade:

  steady/ramp block, as fractions of the target (1.0 = on target):
    r_avg   = sum(actual) / sum(target)            over the graded time (a ramp is compared second by second)
    r_low   = lowest 3 s average                   capped at 1
    r_high  = 95th percentile of the 3 s averages
    I       = average target / FTP                 the block's intensity
    base    = (r_low + 5*min(r_avg, 1)) / 6
    overAvg = OVER_SLOPE * (r_avg - 1 - tol)       when positive; tol = 3% at or below FTP, rising to 8% at 2x FTP,
                                                   and twice the slope for recovery blocks (under 0.75 FTP)
    surge   = 0.2 * (r_high - 1 - tolHigh)         when positive; tolHigh = 10% at or below FTP, rising to 20% at 2x FTP
    bonus   = 0.04 * clamp(I - 0.5, 0, 1)
    score   = base - min(overAvg + surge, 0.20) + bonus
  blocks under SHORT_BLOCK_SECONDS skip the low/high terms: base = min(r_avg, 1)
  free ride block: score = time with power above FREE_RIDE_MIN_WATTS / graded time

Grace periods at the start (time to reach the new target) and end of each block aren't graded, nor is
the time getting back up to the target after a pause (see resume). A workout ended part way through a
block grades that block up to then (finish(endedAt)), weighted in the workout's average by the time
ridden; blocks never reached aren't graded at all.
A score over 100% is an S; otherwise A+ to F, see letter().
"""

import bisect
import math


#grace at the start of a block: GRACE_BASE_SECONDS plus a second per GRACE_WATTS_PER_SECOND of target
#change from the block before, at most GRACE_MAX_SECONDS and GRACE_MAX_FRACTION of the block
GRACE_BASE_SECONDS = 3.0
GRACE_WATTS_PER_SECOND = 50.0
GRACE_MAX_SECONDS = 10.0
GRACE_MAX_FRACTION = 0.25
#grace at the end of a block, at most END_GRACE_MAX_FRACTION of the block
END_GRACE_SECONDS = 2.0
END_GRACE_MAX_FRACTION = 0.10

#a power reading older than this counts as a dropout (the sensor sends ~4 per second)
DROPOUT_SECONDS = 2.0

SHORT_BLOCK_SECONDS = 10.0
ROLLING_SECONDS = 3.0
LOW_WEIGHT, AVG_WEIGHT = 1, 5
HIGH_PERCENTILE = 0.95

OVER_TOLERANCE, OVER_TOLERANCE_EXTRA = 0.03, 0.05 #+ extra x (I - 1), I capped at 2
OVER_SLOPE = 0.5
RECOVERY_INTENSITY, RECOVERY_OVER_SLOPE = 0.75, 1.0
SURGE_TOLERANCE, SURGE_TOLERANCE_EXTRA = 0.10, 0.10
SURGE_SLOPE = 0.2
MAX_OVER_PENALTY = 0.20
BONUS_MAX, BONUS_FROM_INTENSITY = 0.04, 0.5

FREE_RIDE_MIN_WATTS = 10

#(lowest %, letter), best first; over 100% is an S
GRADE_BANDS = ((97, "A+"), (93, "A"), (90, "A-"), (87, "B+"), (83, "B"), (80, "B-"), (77, "C+"), (73, "C"),
               (70, "C-"), (67, "D+"), (63, "D"), (60, "D-"))
#the letter's color, by its first character
GRADE_COLORS = {"S": "#f5c400", "A": "#2f9e47", "B": "#7fb52c", "C": "#e0c33a", "D": "#e3801a", "F": "#d63e34"}
NO_GRADE = "--"
NO_GRADE_COLOR = "#9fc5d1"


def letter(pct):
    """The letter grade for a score in %, NO_GRADE for None"""
    if pct is None:
        return NO_GRADE
    if pct > 100:
        return "S"
    for lowest, grade in GRADE_BANDS:
        if pct >= lowest:
            return grade
    return "F"


def grade_color(grade):
    return GRADE_COLORS.get(grade[:1], NO_GRADE_COLOR)


def _clamp(value, low, high):
    return max(low, min(high, value))


def start_grace(duration, step):
    """Seconds left ungraded at the start of a block whose target changed by step watts"""
    return min(GRACE_BASE_SECONDS + abs(step)/GRACE_WATTS_PER_SECOND, GRACE_MAX_SECONDS, GRACE_MAX_FRACTION*duration)


def end_grace(duration):
    return min(END_GRACE_SECONDS, END_GRACE_MAX_FRACTION*duration)


class BlockGrader:
    def __init__(self, duration, previousTarget, ftp, freeRide):
        self.duration = float(duration)
        self.previousTarget = previousTarget
        self.ftp = max(1, int(ftp))
        self.freeRide = freeRide
        self.startGrace = None #set from the first target seen
        self.endGrace = end_grace(self.duration)
        #(dt, target, actual) for each graded tick, and for each tick in a grace period (graded instead when
        #nothing else could be, a block too short to have time outside its grace periods)
        self.samples = []
        self.graceSamples = []
        self.droppedSeconds = 0.0
        self.graceSeconds = 0.0
        self.firstTarget = None
        self.resumeGraceUntil = 0.0 #block seconds, after a pause

    def resume(self, blockSeconds, target):
        """The workout was paused (the trainer drops to 0 W) and is going again blockSeconds into the block:
        the time to get back up to target is grace, the same as at the start of a block"""
        if not self.freeRide:
            self.resumeGraceUntil = blockSeconds + start_grace(self.duration, target)

    def add(self, blockSeconds, dt, target, actual):
        """One tick: blockSeconds into the block, dt long, actual None during a dropout"""
        if dt <= 0:
            return
        if self.firstTarget is None:
            self.firstTarget = target
            self.startGrace = 0.0 if self.freeRide else start_grace(self.duration, target - self.previousTarget)
        if (blockSeconds < self.startGrace or blockSeconds < self.resumeGraceUntil
                or blockSeconds > self.duration - self.endGrace):
            self.graceSeconds += dt
            if actual is not None:
                self.graceSamples.append((dt, target, actual))
            return
        if actual is None:
            self.droppedSeconds += dt
            return
        self.samples.append((dt, target, actual))

    def finish(self, endedAt=None):
        """The block's grade: a dict with "pct" (None when nothing could be graded) and the parts
        that went into it, for the synopsis. endedAt: the block seconds the workout was ended at, when that
        was part way through the block ("ridden" is how much of the block was ridden either way)"""
        samples, grace = self.samples, self.graceSeconds
        if not samples and self.graceSamples:
            samples, grace = self.graceSamples, 0.0
        graded = sum(dt for dt, _, _ in samples)
        endedEarly = endedAt is not None and endedAt < self.duration
        ridden = endedAt if endedEarly else self.duration
        result = {"pct": None, "free": self.freeRide, "graded": graded, "dropped": self.droppedSeconds,
                  "grace": grace, "duration": self.duration, "ridden": ridden, "endedEarly": endedEarly}
        if graded <= 0:
            return result
        if self.freeRide:
            pedaling = sum(dt for dt, _, actual in samples if actual > FREE_RIDE_MIN_WATTS)
            result["pedaling"] = 100*pedaling/graded
            result["pct"] = result["pedaling"]
            return result
        targetSum = sum(dt*target for dt, target, _ in samples)
        if targetSum <= 0:
            return result
        actualSum = sum(dt*actual for dt, _, actual in samples)
        rAvg = actualSum/targetSum
        intensity = targetSum/graded/self.ftp
        overTol = OVER_TOLERANCE + OVER_TOLERANCE_EXTRA*_clamp(intensity - 1, 0, 1)
        overSlope = RECOVERY_OVER_SLOPE if intensity < RECOVERY_INTENSITY else OVER_SLOPE
        overAvg = overSlope*max(0.0, rAvg - 1 - overTol)
        bonus = BONUS_MAX*_clamp(intensity - BONUS_FROM_INTENSITY, 0, 1)
        if ridden < SHORT_BLOCK_SECONDS:
            rLow = rHigh = None
            surge = 0.0
            base = min(rAvg, 1.0)
        else:
            rolling = rolling_ratios(samples)
            rLow = min(rolling)
            rHigh = percentile(rolling, HIGH_PERCENTILE)
            surgeTol = SURGE_TOLERANCE + SURGE_TOLERANCE_EXTRA*_clamp(intensity - 1, 0, 1)
            surge = SURGE_SLOPE*max(0.0, rHigh - 1 - surgeTol)
            base = (LOW_WEIGHT*min(rLow, 1.0) + AVG_WEIGHT*min(rAvg, 1.0))/(LOW_WEIGHT + AVG_WEIGHT)
        penalty = min(overAvg + surge, MAX_OVER_PENALTY)
        score = max(0.0, base - penalty + bonus)
        result.update({"pct": 100*score, "avg": 100*rAvg, "low": None if rLow is None else 100*rLow,
                       "high": None if rHigh is None else 100*rHigh, "overAvg": 100*overAvg, "surge": 100*surge,
                       "penalty": 100*penalty, "bonus": 100*bonus, "intensity": intensity})
        return result


def rolling_ratios(samples):
    """actual/target over each ROLLING_SECONDS of graded time (dropouts and grace skipped), one
    per tick once that much has been graded; just the whole if there's less"""
    ratios = []
    windowDt = windowTarget = windowActual = 0.0
    start = 0
    for dt, target, actual in samples:
        windowDt += dt
        windowTarget += dt*target
        windowActual += dt*actual
        while windowDt - samples[start][0] >= ROLLING_SECONDS:
            oldDt, oldTarget, oldActual = samples[start]
            windowDt -= oldDt
            windowTarget -= oldDt*oldTarget
            windowActual -= oldDt*oldActual
            start += 1
        if windowDt >= ROLLING_SECONDS - 0.15 and windowTarget > 0:
            ratios.append(windowActual/windowTarget)
    if not ratios:
        total = sum(dt*target for dt, target, _ in samples)
        ratios.append(sum(dt*actual for dt, _, actual in samples)/total if total else 0.0)
    return ratios


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(math.floor(fraction*(len(ordered) - 1) + 0.5)))]


def latest_power(times, values, now):
    """The latest power reading, or None if there's been none for DROPOUT_SECONDS"""
    if not times or now - times[-1] > DROPOUT_SECONDS:
        return None
    return values[-1]


###----------------------------------------------------------------------------------------------
### The workout's blocks, for the grade lists
###----------------------------------------------------------------------------------------------
def workout_rows(workoutList, grades, songs=None):
    """Rows for a grade list, in workout order, from AppState.workoutList and the grades recorded (AppState.blockGrades,
    keyed (block index, sub-block index, rep)); only the blocks (and interval reps) that were ridden. Each row is a dict:
      {"kind": "block", "label", "pct", "grade", "note"}                  a block ("note": "ended early", or "")
      {"kind": "interval", "label": "3x"}                                 an interval's header, then its sub-blocks:
      {"kind": "sub", "label", "pct", "grade", "note": "S · A · B+"}      their average over the reps ridden, and
                                                                          each rep's grade
    songs: Musical Cadence Mode's {block index: song title} of each song's first block. Each song ridden gets a row
    before its blocks, graded by their average (weighted by the time ridden, as the workout's is); its blocks are
    "indent"ed beneath it:
      {"kind": "song", "label", "pct", "grade", "note": ""}"""
    songs = songs or {}
    rows = []
    songRow = None
    for index, block in enumerate(workoutList):
        if index in songs:
            songRow = {"kind": "song", "label": songs[index], "records": [], "note": ""}
            rows.append(songRow)
        if songRow is not None:
            songRow["records"] += [record for key, record in grades.items() if key[0] == index]
        if block[0] in (0, 1, 2):
            record = grades.get((index, 0, 0))
            if record:
                rows.append({"kind": "block", "label": record["label"], "pct": record["pct"],
                             "grade": letter(record["pct"]), "note": "ended early" if record.get("endedEarly") else "",
                             "indent": songRow is not None})
        elif block[0] == 3:
            multiplier, subBlocks = block[1], block[2:]
            subRows = []
            for sub in range(len(subBlocks)):
                records = [r for r in (grades.get((index, sub, rep)) for rep in range(multiplier)) if r]
                if not records:
                    continue
                scored = [r for r in records if r["pct"] is not None]
                pct = sum(r["pct"]*r["ridden"] for r in scored)/sum(r["ridden"] for r in scored) if scored else None
                note = " · ".join(letter(r["pct"]) for r in records)
                if records[-1].get("endedEarly"):
                    note += " (ended)"
                subRows.append({"kind": "sub", "label": records[0]["label"], "pct": pct, "grade": letter(pct),
                                "note": note})
            if subRows:
                rows.append({"kind": "interval", "label": f"{multiplier}x"})
                rows += subRows
    #(a song none of whose blocks were ridden has no row)
    for row in [row for row in rows if row["kind"] == "song"]:
        records = row.pop("records")
        if records:
            row["pct"] = ridden_average(records)
            row["grade"] = letter(row["pct"])
        else:
            rows.remove(row)
    return rows


def ridden_average(records):
    """Grade records' scores weighted by how long each block was ridden, over the graded ones; None if none"""
    scored = [g for g in records if g["pct"] is not None and g["ridden"] > 0]
    total = sum(g["ridden"] for g in scored)
    if not total:
        return None
    return sum(g["pct"]*g["ridden"] for g in scored)/total


def workout_average(grades):
    """The workout's grade: block scores weighted by how long each block was ridden, over the graded blocks;
    None if none"""
    return ridden_average(grades.values())
