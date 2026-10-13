"""Reading .awo workout files: the app's reader (WorkoutSetupWindow) and awo-verifier.py, which should agree"""

import glob
import os

import pytest

from conftest import APP_DIR
from AppState import AppState
from workout_files import WorkoutParser, verify, verify_file

WORKOUT_DIR = os.path.join(APP_DIR, "awoWorkouts")
SCRATCH_DIR = os.path.join(WORKOUT_DIR, "scratchpad")
SHIPPED = sorted(glob.glob(os.path.join(WORKOUT_DIR, "*.awo")))
SCRATCH = sorted(glob.glob(os.path.join(SCRATCH_DIR, "*.awo")))


@pytest.fixture
def parser():
    return WorkoutParser(ftp=200)


def watts_by_difficulty(power):
    """A steady block's 21 powers at FTP 200"""
    return tuple(int(power*(200 + int(200*i*.02))) for i in range(-10, 11))


###What a workout reads as
def test_free_steady_and_title(parser):
    assert parser.parse(["T: My Ride ", "0:60,3", "1:300,0.75", ""])
    assert AppState.workoutTitle == "My Ride"
    assert AppState.workoutList == [(0, 60, 3), (1, 300, watts_by_difficulty(0.75), 0)]
    assert AppState.workoutList[1][2][10] == 150 and AppState.workoutList[1][2][0] == 120
    assert AppState.workoutDurationSeconds == 360
    assert AppState.workoutMessages == []


def test_explicit_watts_move_with_difficulty_but_not_ftp(parser):
    assert parser.parse(["1:60,250W"])
    powers = AppState.workoutList[0][2]
    assert powers[10] == 250 and powers[0] == 200 and powers[20] == 300
    WorkoutParser(ftp=300).parse(["1:60,250W"])
    assert AppState.workoutList[0][2][10] == 250


def test_ramp_events_cover_the_block(parser):
    assert parser.parse(["2:120,0.5,1.0,2"])
    block, events = AppState.workoutList[0], AppState.workoutList[1:]
    assert block[0] == 2 and block[1] == 120 and block[3] == 2
    assert block[2][10] == (100, 200)
    assert all(event[0] == 4 and event[3] == 120 for event in events)
    #the events' durations add up to the ramp, ending at its end power
    assert sum(event[1] for event in events) == 120
    assert events[-1][2][10] == 200
    #power only goes up, at most one event a second
    powers = [event[2][10] for event in events]
    assert powers == sorted(powers) and all(event[1] >= 1 for event in events)


def test_ramp_down(parser):
    assert parser.parse(["2:60,1.0,0.5"])
    powers = [event[2][10] for event in AppState.workoutList[1:]]
    assert powers == sorted(powers, reverse=True) and powers[-1] == 100


def test_ramp_too_shallow_for_a_whole_watt_still_gets_an_event(parser):
    assert parser.parse(["2:20,0.600,0.602"])
    events = AppState.workoutList[1:]
    assert len(events) == 1 and events[0][1] == 20


def test_intervals(parser):
    assert parser.parse(["3:3;1:30,1.2,3;2:20,0.5,0.6;0:10,1"])
    header = AppState.workoutList[0]
    assert header[:2] == (3, 3) and len(header) == 5
    blocks = [entry for entry in AppState.workoutList if entry[0] == 5]
    #each rep's blocks, counting the reps down
    assert [(b[1], b[2]) for b in blocks] == [(3, 0), (3, 1), (3, 2), (2, 0), (2, 1), (2, 2), (1, 0), (1, 1), (1, 2)]
    #each rep's ramp has its own events, right after its ramp block
    for i, entry in enumerate(AppState.workoutList):
        if entry[0] == 5 and entry[2] == 1:
            assert AppState.workoutList[i + 1][0] == 4
    assert AppState.workoutDurationSeconds == 3*(30 + 20 + 10)


def test_ramp_with_the_same_start_and_end_is_steady(parser):
    assert parser.parse(["2:60,0.5,0.5,3", "3:2;2:30,250W,250W;0:30"])
    assert AppState.workoutList[0] == (1, 60, watts_by_difficulty(0.5), 3)
    #in an interval too: a steady block, no ramp events after it
    assert AppState.workoutList[1][2][0] == 1
    assert [entry[0] for entry in AppState.workoutList[1:]] == [3, 5, 5, 5, 5]
    assert AppState.workoutDurationSeconds == 180


def test_one_block_interval_of_a_steady_block_is_one_longer_block(parser):
    assert parser.parse(["3:4;1:30,1.2,3|Go|5", "3:2;0:45"])
    assert AppState.workoutList == [(1, 120, watts_by_difficulty(1.2), 3), (0, 90, 0)]
    assert AppState.workoutDurationSeconds == 210
    assert AppState.workoutMessages == [(0, 5, "Go"), (30, 35, "Go"), (60, 65, "Go"), (90, 95, "Go")]


def test_one_block_interval_of_a_same_power_ramp_is_one_steady_block(parser):
    assert parser.parse(["3:3;2:20,0.5,0.50|Up|5|10"])
    assert AppState.workoutList == [(1, 60, watts_by_difficulty(0.5), 0)]
    assert AppState.workoutMessages == [(10, 15, "Up"), (30, 35, "Up"), (50, 55, "Up")]


def test_one_block_interval_up_to_a_block_long(parser):
    assert parser.parse(["3:2;0:900"])
    assert AppState.workoutList == [(0, 1800, 0)]


def test_messages_are_placed_in_workout_time(parser):
    assert parser.parse(["0:30", "1:60,0.5|Start|10|0|Later|5|50", "1:20,0.5|Whole block"])
    assert AppState.workoutMessages == [(30, 40, "Start"), (80, 85, "Later"), (90, 110, "Whole block")]


def test_interval_messages_show_every_rep(parser):
    assert parser.parse(["0:10", "3:2;1:30,1.2|Go|5;0:20|Rest||5"])
    assert AppState.workoutMessages == [(10, 15, "Go"), (45, 60, "Rest"), (60, 65, "Go"), (95, 110, "Rest")]


@pytest.mark.parametrize("lines, reason", [
    (["T:A", "T:B"], "Only one title"),
    (["Title"], "Title line must be"),
    (["T:"], "Title line must be"),
    (["4:60"], "Invalid block type"),
    (["1:60"], "Steady state needs 2 or 3 values"),
    (["1:60,0.5,1,2"], "Steady state needs 2 or 3 values"),
    (["0:60:1"], "exactly one colon"),
    (["0:1801"], "between 1 and 1800s"),
    (["0:0"], "between 1 and 1800s"),
    (["0:6.5"], "durations must be integers"),
    (["1:60,3.5"], "Power must be"),
    (["1:60,0.01"], "Power must be"),
    (["1:60,abc"], "Power must be"),
    (["1:60,2001W"], "Watts must be"),
    (["1:60,0W"], "Watts must be"),
    (["1:60,0.5,6"], "Position must be 0 to 5"),
    (["0:60,x"], "Position must be 0 to 5"),
    (["2:60,0.5"], "Ramp needs 3 or 4 values"),
    (["3:2;1:30,1;1:30,1;1:30,1;1:30,1"], "Interval needs 1 to 3 blocks"),
    (["3:0;1:30,1;0:30"], "repeats must be"),
    (["3:x;1:30,1;0:30"], "repeats must be"),
    (["3:2;1:30,1;"], "Empty interval block"),
    (["3:2|Hi;1:30,1;0:30"], "not on 3:<repeats>"),
    (["3:2;1:30,1;7:30"], "Invalid block type in interval"),
    (["3;1:30,1;0:30"], "Interval must start with 3:<repeats>"),
    (["3:3;2:60,0.5,1.0"], "can't be a ramp"),
    (["3:10;1:400,0.6"], "at most 1800s in all"),
    (["3:2;0:901"], "at most 1800s in all"),
    (["3:2;1:30,1;3:2;1:30,1"], "Invalid block type in interval"),
    (["1:60,0.5|"], "Message text can't be empty"),
    (["1:60,0.5|Hi|10|60"], "start must be before the block ends"),
    (["1:60,0.5|Hi|0"], "seconds must be a whole number"),
    (["1:60,0.5|Hi|20|50"], "must end by the end of its block"),
    (["1:60,0.5|Hi|5|0|Bye"], "Multiple messages need"),
    (["3:2;1:30,1|Hi|40;0:30"], "must end by the end of its block"),
])
def test_rejections(parser, lines, reason):
    assert parser.parse(lines) is False
    assert len(parser.rejections) == 1
    assert reason in parser.rejections[0][0]


def test_rejection_says_where(parser):
    parser.parse(["T:x", "1:60,0.5", "1:60,9"])
    reason, lnNum, line = parser.rejections[0]
    assert (lnNum, line) == (3, "1:60,9")


###Files
def write(name, data, mode="w"):
    with open(name, mode, **({} if "b" in mode else {"encoding": "utf-8", "newline": ""})) as f:
        f.write(data)
    return os.path.abspath(name).replace("\\", "/")


def test_file_with_bom_and_trailing_newline(parser):
    path = write("bom.awo", "﻿T:Bom\n1:60,0.5\n", "w")
    assert parser.process_workout_file(path)
    assert AppState.workoutTitle == "Bom" and len(AppState.workoutList) == 1


def test_file_not_utf8(parser):
    path = write("latin.awo", b"T:Caf\xe9\n1:60,0.5\n", "wb")
    assert parser.process_workout_file(path) is False
    assert "UTF-8" in parser.rejections[0][0]


def test_file_wrong_type(parser):
    path = write("workout.txt", "1:60,0.5\n")
    assert parser.process_workout_file(path) is False
    assert ".awo or .zwo" in parser.rejections[0][0]


def test_zwo_file_is_read_through_the_converter(parser):
    path = write("ride.zwo", """<workout_file><name>Z</name><workout>
        <SteadyState Duration="60" Power="0.5"/><Ramp Duration="60" PowerLow="0.5" PowerHigh="0.75"/>
    </workout></workout_file>""")
    assert parser.process_workout_file(path)
    assert AppState.workoutTitle == "Z"
    assert [entry[0] for entry in AppState.workoutList][:2] == [1, 2]


def test_broken_zwo_file_is_rejected(parser):
    path = write("broken.zwo", "<workout_file><workout></workout_file>")
    assert parser.process_workout_file(path) is False
    assert parser.rejections


###The files in the project
@pytest.mark.parametrize("path", SHIPPED, ids=os.path.basename)
def test_shipped_workouts_load(path, parser):
    assert parser.process_workout_file(path.replace("\\", "/")), parser.rejections
    assert AppState.workoutDurationSeconds > 0
    assert verify_file(path) == ""


@pytest.mark.parametrize("path", SCRATCH, ids=os.path.basename)
def test_scratchpad_workouts(path, parser):
    loaded = parser.process_workout_file(path.replace("\\", "/"))
    bad = os.path.basename(path).startswith("bad_")
    assert loaded != bad, parser.rejections
    assert (verify_file(path) != "") == bad


###The verifier agrees with the app
AGREED_LINES = [
    "T:Title", "0:60", "0:60,5", "1:60,0.5", "1:60,250W", "1:60,0.5,3", "2:60,0.5,1.5", "2:60,100W,200W,1",
    "3:4;1:30,1.2,3;0:30", "3:2;1:30,1.2;2:30,0.5,0.6;0:10",
    "1:60,0.5|Msg|10|5", "1:60,0.5|Msg", "3:2;1:30,1.2|Go|5;0:30|Rest|10|5",
    "T:", "4:60", "0:0", "0:1801", "1:60", "1:60,9", "1:60,0W", "1:60,0.5,7", "2:60,0.5", "0:60:2",
    "3:0;1:30,1;0:30", "3:2;1:30,1;", "3:2;1:30,1;7:30", "1:60,0.5|", "1:60,0.5|Hi|10|60",
    "1:60,0.5|Hi|20|50", "1:60,0.5|Hi|5|0|Bye", "3:2|Hi;1:30,1;0:30", "3:2;1:30,1;3:2;1:30,1",
    "3:2;1:30,1", "3:4;2:60,0.5,0.8", "2:60,0.5,0.5", "3:2;2:30,0.5,0.5;0:30", "3;1:30,1;0:30",
    "3:3;2:20,0.5,0.50", "3:3;2:20,250W,250w", "3:3;2:20,100W,0.5", "3:2;0:900", "3:2;0:901", "3:10;1:400,0.6",
]

@pytest.mark.parametrize("line", AGREED_LINES)
def test_verifier_agrees_with_the_app(line, parser):
    assert (verify([line]) == "") == parser.parse([line]), (verify([line]), parser.rejections)


def test_verifier_survives_an_interval_without_a_colon():
    """"3;1:30,1;0:30" has no repeats: the verifier reports it, it doesn't crash"""
    assert "must be 3:<multiplier>" in verify(["3;1:30,1;0:30"])


def test_verifier_skips_files_without_an_extension(tmp_path, monkeypatch):
    (tmp_path / "README").write_text("not a workout")
    (tmp_path / "ride.v2.awo").write_text("4:60\n")
    monkeypatch.chdir(tmp_path)
    import contextlib
    import importlib.util
    import io
    out = io.StringIO()
    spec = importlib.util.spec_from_file_location("awo_verifier_run", os.path.join(APP_DIR, "awo-verifier.py"))
    with contextlib.redirect_stdout(out):
        spec.loader.exec_module(importlib.util.module_from_spec(spec))
    assert "ride.v2.awo" in out.getvalue() and "Invalid block type" in out.getvalue()
