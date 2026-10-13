import pytest

from AppState import AppState
import FtpTest


def test_warmup_length():
    assert FtpTest.WARMUP_SECONDS == 180 + 60 + 120 + 60 + 480


def test_test_follows_app_state():
    assert FtpTest.test() is None
    AppState.ftpTestMode = "ramp20"
    assert FtpTest.test()["stepWatts"] == 20
    assert FtpTest.is_ramp("ramp10") and not FtpTest.is_ramp("1h")


@pytest.mark.parametrize("mode, effort", [("20m", 1200), ("1h", 3600)])
def test_effort_test_lines(mode, effort):
    lines = FtpTest.workout_lines(mode)
    assert lines == ["T:" + FtpTest.TESTS[mode]["name"]] + FtpTest.WARMUP_LINES + [f"0:{effort}"]
    #no FTP: the warmup is a free ride as long
    assert FtpTest.workout_lines(mode, ftpSet=False)[1:] == [f"0:{FtpTest.WARMUP_SECONDS}", f"0:{effort}"]


def test_ramp_test_lines_and_blocks():
    #a ramp's blocks are added while it's ridden
    assert FtpTest.workout_lines("ramp10") == ["T:10W delta FTP Ramp"] + FtpTest.WARMUP_LINES
    assert FtpTest.ramp_block("ramp10", 0) == (1, 60, (50,)*21, 0)
    assert FtpTest.ramp_block("ramp10", 3) == (1, 60, (80,)*21, 0)
    assert FtpTest.ramp_block("ramp20", 2) == (1, 60, (140,)*21, 0)


@pytest.mark.parametrize("mode", FtpTest.TESTS)
@pytest.mark.parametrize("ftpSet", [True, False])
def test_workout_lines_are_valid_awo(mode, ftpSet):
    """The lines an FTP test is built from read as a workout, by the app (which takes an effort longer than a
    workout file's blocks can be, as build_ftp_test does) and, apart from that, by the verifier"""
    from workout_files import WorkoutParser, verify
    lines = FtpTest.workout_lines(mode, ftpSet)
    effort = FtpTest.TESTS[mode].get("effortSeconds", 0)
    errors = verify(lines)
    assert errors == "" or (effort > 1800 and errors.endswith(f"between 1 and 1800s:\n0:{effort}"))
    parser = WorkoutParser(ftp=250 if ftpSet else 0, maxBlockSeconds=max(1800, effort))
    assert parser.parse(lines), parser.rejections
    assert AppState.workoutTitle == FtpTest.TESTS[mode]["name"]


@pytest.mark.parametrize("mode", ["20m", "1h"])
def test_effort_messages(mode):
    start = 900
    messages = FtpTest.messages(mode, start, 250)
    effortEnd = start + FtpTest.TESTS[mode]["effortSeconds"]
    for msgStart, msgEnd, text in messages:
        assert msgStart < msgEnd and text
    texts = {text: (s, e) for s, e, text in messages}
    assert texts["GO!"] == (start, start + FtpTest.GO_SECONDS)
    assert texts["Get Ready..."] == (start - 10, start)
    assert any("250" in text for text in texts)
    twoMinutes = [s for s, e, text in messages if text.startswith("Two minutes left")]
    assert twoMinutes == [effortEnd - 120]
    assert (len(messages) == 6) == (mode == "20m")


def test_ramp_messages():
    messages = FtpTest.messages("ramp10", 900, 0)
    assert [(s, e) for s, e, _ in messages] == [(870, 885), (885, 900)]
    assert "10W" in messages[0][2]


@pytest.mark.parametrize("mode, average, ftp", [("20m", 300, 285), ("1h", 251.6, 252), ("ramp10", 400, 300)])
def test_ftp_from_average(mode, average, ftp):
    assert FtpTest.ftp_from_average(mode, average) == ftp


def test_effort_average_is_time_weighted_and_skips_dropouts():
    average = FtpTest.EffortAverage()
    assert average.average() is None
    average.add(1, 100)
    average.add(3, 200)
    average.add(5, None)    #dropout
    average.add(0, 1000)    #no time
    average.add(-1, 1000)
    assert average.average() == pytest.approx(175)


def test_best_rolling_average():
    best = FtpTest.BestRollingAverage(window=60)
    for _ in range(30):
        best.add(1, 100)
    assert best.average() is None   #not a full window yet
    for _ in range(30):
        best.add(1, 300)
    assert best.average() == pytest.approx(200)
    for _ in range(30):
        best.add(1, 300)
    assert best.average() == pytest.approx(300)
    #worse later on doesn't lower the best
    for _ in range(60):
        best.add(1, None)  #not pedaling counts as 0
    assert best.average() == pytest.approx(300)


def test_best_rolling_average_new_window():
    best = FtpTest.BestRollingAverage(window=10)
    for _ in range(5):
        best.add(1, 500)
    best.start_new_window()
    for _ in range(10):
        best.add(1, 100)
    #the 500s from before don't count toward the next window
    assert best.average() == pytest.approx(100)
    best.add(0, 1000)
    assert best.average() == pytest.approx(100)
