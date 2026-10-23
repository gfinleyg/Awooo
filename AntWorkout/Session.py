"""
Session.py

The rides since the app was opened (or since Clear Session on the setup screen), kept in memory only
(AppState.sessionRides). The synopsis shows them all, joined end to end with a divider where each starts, and the
HUD's spiral ring carries on from them.

A ride joins the session when its synopsis opens (Exit to Synopsis); one left with Back to Setup doesn't. Each is a
dict, its times in seconds from its own start (AppState.startTimestamp) unless said otherwise:

  kind              "free", "workout", "cadence" (Musical Cadence Mode) or "ftp" (an FTP test)
  name              what the title bar showed for it (AppState.workout_display_name), "" for a free ride
  graded            its blocks were graded (a workout or a Musical Cadence Mode ride)
  ftpTestMode       the FTP test's FtpTest.TESTS key, else None
  ftpTestResult     the FTP test's AppState.ftpTestResult
  start             where it starts in the session (the seconds of the rides before it)
  seconds           how long it is in the session: until it ended, or its last reading if that came later
  secondsPassed     AppState.secondsPassed as it ended
  startTimestamp    AppState.startTimestamp
  rideOffset        what to add to an AppState.secondsPassed time for its seconds (see SynopsisWindow)
  data              its WorkoutData histories' copies, by name
  powerRequests     its (AppState.workoutPowerRequestsSeconds, ...Watts, ...SysTime)
  workoutList       AppState.workoutList as ridden
  blockGrades       AppState.blockGrades
  songs             [(title, start, end)] of each song heard (AppState.musicPlayed), in AppState.secondsPassed
  songHeaders       {workoutList index: song title} of each song's first block, for the grade list
  ftp               the FTP as START was pressed
  powerRanges       the power zones' limits as START was pressed (FtpColors.power_ranges); it's drawn with these
  targetRanges      AppState.ftpRanges as START was pressed (cadence zones, as FtpColors.CadenceRanges, in Musical
                    Cadence Mode): its target bars' and block labels' zones
  hrm, speed, cadenceSensor, speedCadence    the sensors it had (AppState.antHRM, has_speed_sensor, ...)
  wheelCircumference    in meters
  ring              (seconds, [(seconds, power, cadence)]) for the spiral's ring: what a workout's ring had (from
                    START), a free ride's from its readings (filled in by the synopsis); None until then

Each ride keeps the FTP it was ridden at: an FTP changed after it (typed in, or an FTP test's) doesn't recolor it.
The zones' colors (the palette, the theme) are the same for every ride.
"""

import bisect

from AppState import AppState
import FtpColors
import WorkoutData
import WorkoutGrading
from WorkoutHelpers import IsSongHeader, MinimizedDuration

#the WorkoutData histories a ride keeps
HISTORIES = ("power", "heartRate", "speedCadence", "cadence", "speed", "fallbackCadence")
#each kind's heading
KIND_NAMES = {"free": "Free Ride", "workout": "Workout", "cadence": "Musical Cadence", "ftp": "FTP Test"}
#a ride's ring from its readings: the average power over the last this many seconds, and cadence, as the HUD's
#spiral takes them (HUDWindow.SPIRAL_AVERAGE_SECONDS, SPIRAL_CADENCE_AVERAGE_SECONDS)
RING_POWER_SECONDS = 3.0
RING_CADENCE_SECONDS = 1.0


def rides():
    return AppState.sessionRides


def seconds():
    """How long the session's rides are, one after the other"""
    return sum(ride["seconds"] for ride in AppState.sessionRides)


def clear():
    AppState.sessionRides[:] = []


def start_values():
    """What a ride keeps from when START was pressed"""
    targetRanges = list(AppState.ftpRanges)
    if AppState.workoutMode and AppState.cadenceMode:
        targetRanges = FtpColors.CadenceRanges(targetRanges)
    return {"ftp": int(AppState.ftp or 0), "powerRanges": FtpColors.power_ranges(), "targetRanges": targetRanges}


def begin_ride():
    """START on the setup screen: a new ride, not in the session until its synopsis opens"""
    AppState.rideStart = start_values()
    AppState.rideRecorded = False


def ride_kind():
    if not AppState.workoutMode:
        return "free"
    if AppState.ftpTestMode:
        return "ftp"
    return "cadence" if AppState.cadenceMode else "workout"


def add_current_ride(hud=None):
    """The ride just ridden (AppState and WorkoutData) joins the session, once. hud: its HUDWindow, whose spiral's
    ring it keeps"""
    if AppState.rideRecorded:
        return
    AppState.rideRecorded = True
    begun = AppState.rideStart or start_values()
    kind = ride_kind()
    startTimestamp = AppState.startTimestamp
    startSeconds = startTimestamp.timestamp()
    data = {name: getattr(WorkoutData, name).copy() for name in HISTORIES}
    lastReading = max((history.times[-1] - startSeconds for history in data.values() if len(history)), default=0.0)
    requests = (list(AppState.workoutPowerRequestsSeconds), list(AppState.workoutPowerRequestsWatts),
                list(AppState.workoutPowerRequestsSysTime))
    rideOffset = 0.0
    if requests[0] and requests[2]:
        rideOffset = (requests[2][0] - startTimestamp).total_seconds() - requests[0][0]
    tracks = AppState.musicTracks if kind == "cadence" else []
    songs = [(tracks[index][1], start, end) for index, start, end in AppState.musicPlayed if index < len(tracks)]
    #(Musical Cadence Mode's song headers are "<length> : <Artist> - <Title>")
    songHeaders = {index: header.split(" : ", 1)[1] for index, header in AppState.musicSongStarts.items()
                   if IsSongHeader(header)} if kind == "cadence" else {}
    spiral = getattr(hud, "spiralWorkoutProgress", None) if AppState.workoutMode else None
    AppState.sessionRides.append({
        "kind": kind,
        "name": AppState.workout_display_name() if AppState.workoutMode else "",
        "graded": kind in ("workout", "cadence"),
        "ftpTestMode": AppState.ftpTestMode if kind == "ftp" else None,
        "ftpTestResult": dict(AppState.ftpTestResult or {}) if kind == "ftp" else None,
        "start": seconds(),
        "seconds": max(AppState.secondsPassed, lastReading),
        "secondsPassed": AppState.secondsPassed,
        "startTimestamp": startTimestamp,
        "rideOffset": rideOffset,
        "data": data,
        "powerRequests": requests,
        "workoutList": list(AppState.workoutList),
        "blockGrades": dict(AppState.blockGrades),
        "songs": songs,
        "songHeaders": songHeaders,
        "ftp": begun["ftp"],
        "powerRanges": list(begun["powerRanges"]),
        "targetRanges": begun["targetRanges"],
        "hrm": AppState.antHRM,
        "speed": AppState.has_speed_sensor(),
        "cadenceSensor": AppState.has_cadence_sensor(),
        "speedCadence": AppState.any_speed_cadence(),
        "wheelCircumference": float(AppState.wheelCircumference or 0),
        "ring": spiral.ridden() if spiral is not None else None,
    })


def heading(ride):
    """A ride's heading: its kind and name"""
    kind = KIND_NAMES[ride["kind"]]
    if ride["kind"] == "free" or not ride["name"]:
        return kind
    #(an FTP test's name says it's one)
    return ride["name"] if ride["kind"] == "ftp" else f"{kind}: {ride['name']}"


def note(ride):
    """What a ride's heading row says at its right: how long it was, and if it wasn't graded"""
    text = MinimizedDuration(round(ride["secondsPassed"]))
    return text if ride["graded"] else text + " · not graded"


def average(ride):
    return WorkoutGrading.workout_average(ride["blockGrades"]) if ride["graded"] else None


def ring_history():
    """The rides' rings, (seconds, samples, power ranges) each, for the HUD's spiral to start with"""
    return [(ride["ring"][0], ride["ring"][1], ride["powerRanges"]) for ride in AppState.sessionRides if ride["ring"]]


def per_second(samples):
    """(seconds, power, cadence) samples as one a second: each second's averages (cadence None if none had one)"""
    seconds = {}
    for at, power, cadence in samples:
        sums = seconds.setdefault(int(at), [0.0, 0, 0.0, 0])
        sums[0] += power
        sums[1] += 1
        if cadence is not None:
            sums[2] += cadence
            sums[3] += 1
    return [(second + 0.5, power/count, cadenceSum/cadenceCount if cadenceCount else None)
            for second, (power, count, cadenceSum, cadenceCount) in sorted(seconds.items())]


def ring_from_readings(length, power, cadence, hasCadence):
    """A ride's ring (seconds, samples a second apart) from its readings, (times, values) each in its own seconds:
    the power and cadence as the HUD's spiral would have taken them (see HUDWindow.record_spiral_sample)"""
    samples = []
    for second in range(int(length)):
        now = second + 1.0
        powerNow = recent_average(*power, now, RING_POWER_SECONDS)
        cadenceNow = None
        if hasCadence:
            cadenceNow = recent_average(*cadence, now, RING_CADENCE_SECONDS)
            if cadenceNow is None and powerNow is None:
                cadenceNow = 0
        samples.append((second + 0.5, 0 if powerNow is None else powerNow, cadenceNow))
    return (float(length), samples)


def recent_average(times, values, now, seconds):
    """The readings' average over the seconds up to now, the last one if none came in that time, or None after
    WorkoutGrading.DROPOUT_SECONDS without any (see HUDWindow.recent_average)"""
    end = bisect.bisect_right(times, now)
    if end == 0 or now - times[end - 1] > WorkoutGrading.DROPOUT_SECONDS:
        return None
    start = bisect.bisect_right(times, now - seconds, 0, end)
    if start == end:
        return values[end - 1]
    return sum(values[start:end])/(end - start)
