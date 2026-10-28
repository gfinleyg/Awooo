import pytest

from AppState import AppState
import WorkoutData
import WorkoutHelpers as H


@pytest.mark.parametrize("seconds, text", [
    (0, "0s"), (1, "1s"), (59, "59s"), (59.9, "59s"),
    (60, "1m"), (61, "1m 1s"), (599, "9m 59s"), (3599, "59m 59s"),
    (3600, "1h"), (3660, "1h 1m"), (3661, "1h 1m 1s"), (7322, "2h 2m 2s"),
    (-5, "0s"),
])
def test_minimized_duration(seconds, text):
    assert H.MinimizedDuration(seconds) == text


def _steady(watts, duration=300, position=0):
    return (1, duration, tuple(watts + i for i in range(-10, 11)), position)


def _ramp(start, end, duration=90, position=1):
    return (2, duration, tuple((start + i, end + i) for i in range(-10, 11)), position)


def test_block_text_free_steady_ramp():
    assert H.ProcessWorkoutLine((0, 60, 3), 10) == ["Stand 1m"]
    assert H.ProcessWorkoutLine(_steady(200), 10) == ["200W 5m"]
    #the difficulty picks the power
    assert H.ProcessWorkoutLine(_steady(200), 0) == ["190W 5m"]
    assert H.ProcessWorkoutLine(_steady(200), 20) == ["210W 5m"]
    assert H.ProcessWorkoutLine(_ramp(100, 200), 10) == ["↑RPM 100-200W 1m 30s"]


def test_block_text_intervals():
    assert H.ProcessWorkoutLine((3, 4, _steady(300, 30, 3), (0, 30, 0)), 10) == \
        ["4x", "Stand 300W 30s", "Free 30s"]
    three = (3, 2, _steady(300, 30, 4), _ramp(100, 150, 60, 5), (0, 15, 2))
    assert H.ProcessWorkoutLine(three, 10) == \
        ["2x", "1 Leg 300W 30s", "1 Leg ⇄ 100-150W 1m", "↓RPM 15s"]


def test_block_text_says_rpm_in_musical_cadence_mode():
    AppState.cadenceMode = True
    AppState.workoutMode = True
    assert H.ProcessWorkoutLine(_steady(90), 10) == ["90rpm 5m"]
    #(free mode never says RPM)
    AppState.workoutMode = False
    assert H.ProcessWorkoutLine(_steady(90), 10) == ["90W 5m"]


@pytest.mark.parametrize("text, targets", [
    ("200W 5m", (200, 0)),
    ("100-200W 1m", (100, 200)),
    ("90rpm 5m", (90, 0)),
    ("Stand 300W 30s", (300, 0)),
    ("↑RPM 100-200W 1m", (100, 200)),
    ("Free 5m", None),
    ("Stand 1m", None),
    ("↓RPM 15s", None),
    ("W 5m", None),
    ("abcW", None),
    ("4x", None),
])
def test_targets_from_text(text, targets):
    assert H.TargetsFromText(text) == targets


def test_targets_round_trip_through_block_text():
    for line in (_steady(250), _ramp(120, 330), _steady(250, position=1)):
        text = H.ProcessWorkoutLine(line, 10)[0]
        low, high = H.TargetsFromText(text)
        expected = line[2][10]
        assert (low, high) == ((expected, 0) if line[0] == 1 else expected)


def test_song_header_only_in_cadence_mode():
    assert not H.IsSongHeader("3m 20s : Artist - Title")
    AppState.cadenceMode = True
    assert H.IsSongHeader("3m 20s : Artist - Title")
    assert not H.IsSongHeader("90rpm 5m")


def test_max_helpers():
    assert H.GetMaxOrZero([]) == 0
    assert H.GetMaxOrZero([3, 9, 2]) == 9
    assert H.GetMaxOrZeroAndIndex([]) == (0, 0)
    assert H.GetMaxOrZeroAndIndex([3, 9, 2, 9]) == (9, 1)


def test_dropouts_short_gaps_are_joined():
    times, values = [0, 1, 2, 7], [100, 110, 120, 130]
    #a 5 second gap is still a straight line
    assert H.WithDropoutsAsZero(times, values) == (times, values)


def test_dropouts_long_gap_drops_to_zero():
    times, values = H.WithDropoutsAsZero([0, 1, 10], [100, 110, 130])
    edge = H.DROPOUT_EDGE_SECONDS
    assert times == [0, 1, 1 + edge, 10 - edge, 10]
    assert values == [100, 110, 0, 0, 130]


def test_dropouts_silent_at_the_end():
    times, values = H.WithDropoutsAsZero([0, 1], [100, 110], endTime=20)
    assert times == [0, 1, 1 + H.DROPOUT_EDGE_SECONDS, 20]
    assert values == [100, 110, 0, 0]
    #not long enough
    assert H.WithDropoutsAsZero([0, 1], [100, 110], endTime=5) == ([0, 1], [100, 110])
    assert H.WithDropoutsAsZero([], [], endTime=5) == ([], [])


@pytest.mark.parametrize("value, result", [
    ("1.5", True), ("2", True), ("-3e2", True), (" 4 ", True), ("abc", False), ("", False), (None, False),
])
def test_is_float(value, result):
    assert H.is_float(value) is result


def test_selected_device_lines_are_cleaned():
    H.WriteSelectedDeviceLines([
        "11 12345", "120 0023", "11 12345",   #duplicate after normalizing
        "17 1", "121 65535", "122 65536",     #out of range
        "123 0",                              #wildcard
        "99 5",                               #unknown type
        "11", "11 2 3", "11 abc", "",          #malformed
    ])
    assert H.ReadSelectedDeviceLines() == ["11 12345", "120 23", "17 1", "121 65535"]
    #a copy: changing it changes nothing
    H.ReadSelectedDeviceLines().append("120 5")
    assert len(H.ReadSelectedDeviceLines()) == 4
    H.ClearSelectedDeviceLines()
    assert H.ReadSelectedDeviceLines() == []


def test_sensor_history_records_rows():
    history = WorkoutData.SensorHistory(2)
    history.append(10, 20.7)
    history.append(11, 21)
    assert len(history) == 2
    rows = list(history.rows())
    assert [row[1:] for row in rows] == [(10, 20), (11, 21)]
    assert rows[0][0] <= rows[1][0]
    history.clear()
    assert len(history) == 0 and list(history.rows()) == []


def test_workout_data_clear_empties_every_history():
    for history in (WorkoutData.power, WorkoutData.heartRate, WorkoutData.fallbackCadence):
        history.append(1)
    WorkoutData.speedCadence.append(1, 2, 3, 4)
    WorkoutData.cadence.append(1, 2)
    WorkoutData.speed.append(1, 2)
    WorkoutData.clear()
    for history in (WorkoutData.power, WorkoutData.heartRate, WorkoutData.speedCadence, WorkoutData.cadence,
                    WorkoutData.speed, WorkoutData.fallbackCadence):
        assert len(history) == 0
