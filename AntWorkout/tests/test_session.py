"""The session's rides (Session.py): what a ride keeps as it joins, their headings, the rings worked out from a free
ride's readings, and the zones each keeps"""

import datetime

import pytest

from AppState import AppState
import FtpColors
import Session
import WorkoutData


@pytest.fixture(autouse=True)
def no_readings():
    WorkoutData.clear()
    yield
    WorkoutData.clear()


def ridden(seconds, workoutMode=True, cadenceMode=False, ftpTestMode=None, ftp="200"):
    """A ride as the app leaves it at its synopsis: power read every second from START"""
    AppState.ftp = ftp
    AppState.workoutMode, AppState.cadenceMode, AppState.ftpTestMode = workoutMode, cadenceMode, ftpTestMode
    AppState.ftpRanges[:] = FtpColors.power_ranges()
    AppState.workoutTitle, AppState.workoutFilenameShort = "Test Ride", "test.awo"
    AppState.startTimestamp = datetime.datetime.now() - datetime.timedelta(seconds=seconds)
    Session.begin_ride()
    start = AppState.startTimestamp.timestamp()
    for t in range(1, seconds):
        WorkoutData.power.times.append(start + t)
        WorkoutData.power.columns[0].append(200)
    AppState.secondsPassed = float(seconds)
    AppState.blockGrades = {(0, 0, 0): {"pct": 90.0, "ridden": seconds, "start": 0, "end": seconds, "label": "200W"}}


def test_a_ride_joins_once_after_the_ones_before():
    ridden(60)
    Session.add_current_ride()
    #(the synopsis can be built again for the same ride: it's still just the one)
    Session.add_current_ride()
    assert len(Session.rides()) == 1
    first = Session.rides()[0]
    assert first["kind"] == "workout" and first["graded"] and first["start"] == 0
    assert first["seconds"] == pytest.approx(60)
    assert len(first["data"]["power"]) == 59
    #its readings are its own copy: the next ride's start over
    WorkoutData.clear()
    assert len(first["data"]["power"]) == 59

    ridden(30, workoutMode=False)
    Session.add_current_ride()
    second = Session.rides()[1]
    assert second["kind"] == "free" and not second["graded"]
    assert second["start"] == pytest.approx(60)
    assert Session.seconds() == pytest.approx(90)

    Session.clear()
    assert Session.rides() == []


def test_each_ride_keeps_the_zones_from_its_start():
    ridden(60, ftp="200")
    zones = FtpColors.power_ranges()
    #the FTP changed after START (an FTP test's Apply FTP), before the ride joined
    AppState.ftp = "300"
    Session.add_current_ride()
    ride = Session.rides()[0]
    assert ride["ftp"] == 200 and ride["powerRanges"] == zones
    assert ride["powerRanges"] != FtpColors.power_ranges()


def test_a_musical_cadence_ride_keeps_its_zones_as_cadence_zones():
    ridden(60, cadenceMode=True)
    AppState.ftpRanges[:] = FtpColors.cadence_ranges(60)
    Session.begin_ride()
    Session.add_current_ride()
    ranges = Session.rides()[0]["targetRanges"]
    assert isinstance(ranges, FtpColors.CadenceRanges)
    #they still fade as cadence zones once another mode is selected
    AppState.cadenceMode = False
    assert FtpColors.fade_from(ranges) == FtpColors.CADENCE_FADE_FROM
    assert FtpColors.fade_from(list(ranges)) == 0.0


@pytest.mark.parametrize("kind, name, expected", [
    ("free", "", "Free Ride"),
    ("workout", "Tempo", "Workout: Tempo"),
    ("cadence", "Songs", "Musical Cadence: Songs"),
    ("ftp", "20 Minute FTP Test", "20 Minute FTP Test"),
    ("workout", "", "Workout"),
])
def test_headings(kind, name, expected):
    assert Session.heading({"kind": kind, "name": name}) == expected


def test_notes_say_how_long_and_if_not_graded():
    assert Session.note({"secondsPassed": 3600, "graded": True}) == "1h"
    assert Session.note({"secondsPassed": 90, "graded": False}).endswith("not graded")


def test_per_second_averages():
    samples = Session.per_second([(0.1, 100, 80), (0.6, 200, None), (1.2, 300, 90)])
    assert samples == [(0.5, 150, 80), (1.5, 300, 90)]


def test_ring_from_readings():
    power = ([0.5, 1.5, 2.5, 3.5], [100, 200, 300, 400])
    cadence = ([0.5, 1.5], [80, 90])
    length, samples = Session.ring_from_readings(8, power, cadence, True)
    assert length == 8 and len(samples) == 8
    #the last 3 seconds' power, the last second's cadence
    assert samples[0] == (0.5, 100, 80)
    assert samples[3][1] == pytest.approx(300)
    #the cadence gone quiet with the power still coming in: the lane's outer edge
    assert samples[3][2] is None
    #both quiet: not pedaling
    assert samples[7] == (7.5, 0, 0)
    #no cadence to show
    assert all(cadence is None for _, _, cadence in Session.ring_from_readings(4, power, ([], []), False)[1])
