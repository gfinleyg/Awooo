import pytest

import WorkoutGrading as G

TICK = 0.1


def ride(grader, duration, target, actual, start=0.0):
    """Feeds grader 100ms ticks from start to duration; target/actual are values or functions of block seconds"""
    steps = int(round((duration - start)/TICK))
    for i in range(steps):
        t = start + i*TICK
        tv = target(t) if callable(target) else target
        av = actual(t) if callable(actual) else actual
        grader.add(t, TICK, tv, av)
    return grader


@pytest.mark.parametrize("pct, grade", [
    (None, "--"), (100.01, "S"), (100, "A+"), (97, "A+"), (96.99, "A"), (93, "A"), (90, "A-"), (87, "B+"),
    (83, "B"), (80, "B-"), (77, "C+"), (73, "C"), (70, "C-"), (67, "D+"), (63, "D"), (60, "D-"), (59.99, "F"),
    (0, "F"),
])
def test_letter(pct, grade):
    assert G.letter(pct) == grade


def test_grade_color():
    assert G.grade_color("A+") == G.GRADE_COLORS["A"]
    assert G.grade_color("S") == G.GRADE_COLORS["S"]
    assert G.grade_color("--") == G.NO_GRADE_COLOR


def test_grace_periods():
    assert G.start_grace(300, 0) == 3
    assert G.start_grace(300, -200) == pytest.approx(7)
    assert G.start_grace(300, 5000) == G.GRACE_MAX_SECONDS
    assert G.start_grace(8, 200) == pytest.approx(2)      #a quarter of the block at most
    assert G.end_grace(300) == 2
    assert G.end_grace(10) == pytest.approx(1)


def test_on_target_steady_block():
    grade = ride(G.BlockGrader(60, 200, 250, False), 60, 200, 200).finish()
    #0.8 FTP: the intensity bonus takes a perfect ride over 100%, an S
    assert grade["avg"] == pytest.approx(100)
    assert grade["low"] == pytest.approx(100) and grade["high"] == pytest.approx(100)
    assert grade["bonus"] == pytest.approx(100*0.04*0.3)
    assert grade["pct"] == pytest.approx(101.2)
    assert G.letter(grade["pct"]) == "S"
    assert grade["graded"] == pytest.approx(60 - 3 - 2, abs=0.11)
    assert not grade["endedEarly"] and grade["ridden"] == 60


def test_under_target():
    grade = ride(G.BlockGrader(60, 200, 250, False), 60, 200, 180).finish()
    assert grade["avg"] == pytest.approx(90)
    assert grade["penalty"] == 0
    assert grade["pct"] == pytest.approx(90 + 1.2)


def test_over_target_is_penalized():
    grade = ride(G.BlockGrader(60, 200, 250, False), 60, 200, 240).finish()
    #20% over at 0.8 FTP: 0.5 x (0.2 - 0.03) over the average, 0.2 x (1.2 - 1.1) surge
    assert grade["overAvg"] == pytest.approx(8.5)
    assert grade["surge"] == pytest.approx(2)
    assert grade["pct"] == pytest.approx(100 - 10.5 + 1.2)


def test_over_target_penalty_is_capped():
    grade = ride(G.BlockGrader(60, 200, 250, False), 60, 200, 1000).finish()
    assert grade["penalty"] == pytest.approx(100*G.MAX_OVER_PENALTY)


def test_recovery_blocks_are_stricter_about_going_over():
    easy = ride(G.BlockGrader(60, 100, 250, False), 60, 100, 120).finish()   #0.4 FTP
    #recovery slope: 1.0 x (0.2 - 0.03)
    assert easy["overAvg"] == pytest.approx(17)


def test_hard_blocks_have_more_tolerance():
    hard = ride(G.BlockGrader(60, 500, 250, False), 60, 500, 530).finish()   #2x FTP, 6% over
    #tolerance 8% at 2x FTP: nothing over the average, no surge
    assert hard["overAvg"] == 0 and hard["surge"] == 0
    assert hard["bonus"] == pytest.approx(4)


def test_one_slow_stretch_lowers_the_low():
    grade = ride(G.BlockGrader(60, 200, 250, False), 60, 200, lambda t: 100 if 20 <= t < 25 else 200).finish()
    assert grade["low"] == pytest.approx(50, abs=1)
    assert grade["pct"] < 95


def test_dropouts_are_left_out():
    grade = ride(G.BlockGrader(60, 200, 250, False), 60, 200, lambda t: None if 20 <= t < 30 else 200).finish()
    assert grade["dropped"] == pytest.approx(10, abs=0.11)
    assert grade["avg"] == pytest.approx(100)


def test_start_and_end_grace_are_not_graded():
    #way off during the grace periods only
    grade = ride(G.BlockGrader(60, 0, 250, False), 60, 200,
                 lambda t: 0 if t < 6.5 or t > 58.5 else 200).finish()
    #a 200W step: 3 + 4 seconds of grace at the start
    assert grade["avg"] == pytest.approx(100)
    assert grade["grace"] == pytest.approx(7 + 2, abs=0.11)


def test_resume_grace():
    grader = G.BlockGrader(120, 200, 250, False)
    ride(grader, 30, 200, 200)
    grader.resume(30, 200)
    #getting back up after the pause isn't graded
    ride(grader, 120, 200, lambda t: 0 if t < 36.9 else 200, start=30)
    assert grader.finish()["avg"] == pytest.approx(100)


def test_ramp_is_compared_second_by_second():
    grade = ride(G.BlockGrader(120, 100, 250, False), 120, lambda t: 100 + t, lambda t: 100 + t).finish()
    assert grade["avg"] == pytest.approx(100)
    assert grade["low"] == pytest.approx(100)


def test_short_block_uses_the_average_only():
    grade = ride(G.BlockGrader(8, 200, 250, False), 8, 200, 180).finish()
    assert grade["low"] is None and grade["high"] is None
    assert grade["pct"] == pytest.approx(90 + 1.2)


def test_block_with_readings_only_in_its_grace_is_graded_on_those():
    #(a 200W step into a 20s block: 5s of start grace, 2s at the end)
    grader = ride(G.BlockGrader(20, 0, 250, False), 20, 200, lambda t: 180 if t < 4.9 or t > 18.1 else None)
    grade = grader.finish()
    assert grade["avg"] == pytest.approx(90)
    assert grade["grace"] == 0 and grade["graded"] == pytest.approx(6.8, abs=0.11)


def test_free_ride():
    grade = ride(G.BlockGrader(60, 0, 250, True), 60, 0, lambda t: 0 if t < 15 else 150).finish()
    assert grade["free"]
    #(the last 2s are end grace)
    assert grade["pedaling"] == pytest.approx(100*43/58, abs=0.3)
    assert grade["pct"] == grade["pedaling"]


def test_nothing_graded():
    grade = G.BlockGrader(60, 0, 250, False).finish()
    assert grade["pct"] is None and grade["graded"] == 0
    grade = ride(G.BlockGrader(60, 0, 250, False), 60, 200, None).finish()
    assert grade["pct"] is None and grade["dropped"] > 0
    #a zero target has nothing to compare to
    assert ride(G.BlockGrader(60, 0, 250, False), 60, 0, 100).finish()["pct"] is None
    #zero length ticks are ignored
    grader = G.BlockGrader(60, 0, 250, False)
    grader.add(10, 0, 200, 200)
    assert grader.firstTarget is None


def test_ended_early():
    grader = ride(G.BlockGrader(300, 200, 250, False), 100, 200, 200)
    grade = grader.finish(endedAt=100)
    assert grade["endedEarly"] and grade["ridden"] == 100
    assert grader.finish(endedAt=300)["endedEarly"] is False


def test_rolling_ratios_and_percentile():
    samples = [(1.0, 100, 100)]*5 + [(1.0, 100, 200)]*5
    ratios = G.rolling_ratios(samples)
    assert ratios[0] == pytest.approx(1.0) and ratios[-1] == pytest.approx(2.0)
    assert len(ratios) == 8
    #less than one window: the whole
    assert G.rolling_ratios([(0.5, 100, 50)]) == [0.5]
    assert G.rolling_ratios([(0.5, 0, 50)]) == [0.0]
    assert G.percentile([5, 1, 3, 2, 4], 0.95) == 5
    assert G.percentile([5, 1, 3, 2, 4], 0.5) == 3
    assert G.percentile([7], 0.95) == 7


def test_latest_power():
    assert G.latest_power([], [], 10) is None
    assert G.latest_power([8, 9], [100, 150], 10) == 150
    assert G.latest_power([7], [100], 10) is None


def _record(pct, ridden, label="Seated 200 Watts 1m", endedEarly=False):
    return {"pct": pct, "ridden": ridden, "label": label, "endedEarly": endedEarly}


def test_workout_rows():
    steady = (1, 60, (200,)*21, 0)
    workout = [steady, (3, 3, steady, (0, 30, 0)), (5, 3, 0, steady), steady]
    grades = {
        (0, 0, 0): _record(95, 60),
        (1, 0, 0): _record(101, 60, "Seated 300 Watts 1m"),
        (1, 0, 1): _record(85, 60, "Seated 300 Watts 1m"),
        (1, 1, 0): _record(None, 30, "Seated 30s"),
        (1, 0, 2): _record(70, 30, "Seated 300 Watts 1m", endedEarly=True),
        #block 3 never reached
    }
    rows = G.workout_rows(workout, grades)
    assert [row["kind"] for row in rows] == ["block", "interval", "sub", "sub"]
    assert rows[0]["grade"] == "A" and rows[0]["note"] == ""
    assert rows[1]["label"] == "3x"
    #reps weighted by the time ridden
    assert rows[2]["pct"] == pytest.approx((101*60 + 85*60 + 70*30)/150)
    assert rows[2]["note"] == "S · B · C- (ended)"
    assert rows[3]["pct"] is None and rows[3]["grade"] == "--"


def test_workout_average():
    assert G.workout_average({}) is None
    assert G.workout_average({1: _record(None, 60)}) is None
    grades = {1: _record(100, 60), 2: _record(50, 20), 3: _record(None, 60), 4: _record(10, 0)}
    assert G.workout_average(grades) == pytest.approx((100*60 + 50*20)/80)
