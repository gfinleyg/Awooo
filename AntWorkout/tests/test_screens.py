"""Every screen built for real, in every mode, the way LayoutViewer.py shows them (no ANT stick: its fake bridge
stands in): the setup screen with a workout loaded, a simulated ride through the workout screen (HUDWindow's own
timer steps, grading included), the synopsis of the whole ride and both pickers. Then checks that no two keys are
live for the same thing on any screen."""

import os
import sys
from collections import defaultdict

import pytest
from PyQt6.QtCore import QEvent, QPointF, Qt
from PyQt6.QtWidgets import QApplication

from AppState import AppState
import KeyShortcuts
import WorkoutGrading


pytestmark = pytest.mark.whole_app


@pytest.fixture
def viewer(qapp, tmp_path, monkeypatch):
    #(LayoutViewer moves to the project folder when it's imported: back to this test's own after)
    import LayoutViewer
    os.chdir(tmp_path)
    import AppStyle
    import AppTheme
    KeyShortcuts.install(qapp)
    monkeypatch.setattr(AppTheme, "register_fonts", lambda: [])
    #(once: it re-themes every screen still around, the kept ones too)
    if not _keptWindows:
        AppTheme.clear()
    errors = []
    #an exception in a Qt slot would otherwise end the test run
    monkeypatch.setattr(sys, "excepthook", lambda *info: errors.append(info))
    display = LayoutViewer.DisplayWindow(AppStyle.APP_STYLESHEET)
    display.resize(1920, 1080)
    display.show()
    yield LayoutViewer, display, errors
    display.pause()
    for view in display.views.values():
        for timerName in ("timer", "blinkTimer", "smoothBarTimer", "selectionTimer", "starTimer"):
            if hasattr(view, timerName):
                getattr(view, timerName).stop()
        view.hide()
    display.hide()
    import AppLayout
    AppLayout._window = None
    #kept, not destroyed: tearing these screens down mid-run can crash Qt, which the app never does either (its
    #screens go when it exits, without Python's teardown). The session ends the same way, see conftest.py
    _keptWindows.append(display)
    QApplication.processEvents()


_keptWindows = []


def live_key_conflicts(window):
    """Keys more than one binding would act on right now (bindings for a focused field aside)"""
    live = defaultdict(list)
    for binding in KeyShortcuts._live_bindings(window):
        if binding.field is None and binding.usable():
            for key in binding.keys:
                live[key].append(binding.label or binding.widget.objectName() or type(binding.widget).__name__)
    return {key: names for key, names in live.items() if len(names) > 1}


@pytest.mark.parametrize("mode", ["workout", "free", "20m", "1h", "ramp10", "ramp20", "cadence"])
def test_every_screen_builds_and_rides(viewer, mode):
    LayoutViewer, display, errors = viewer
    workout = os.path.join(LayoutViewer.WORKOUT_DIR, LayoutViewer.DEFAULT_WORKOUT)
    display.build(mode, workout, 40)
    QApplication.processEvents()
    assert errors == []
    expected = {"Init", "Workout Picker", "Music Picker", "Synopsis", "Free Ride" if mode == "free" else "Workout"}
    assert set(display.views) == expected

    if mode != "free":
        assert AppState.workoutList, "the setup screen loaded nothing to ride"
    #the synopsis rode the whole thing: a workout is graded
    if mode in ("workout", "cadence"):
        assert AppState.blockGrades
        assert WorkoutGrading.workout_average(AppState.blockGrades) is not None
        assert WorkoutGrading.workout_rows(AppState.workoutList, AppState.blockGrades)

    #every key is a key (a tuple of keys inside the tuple would never match)
    bindings = KeyShortcuts._live_bindings(display)
    assert bindings and all(isinstance(key, Qt.Key) for b in bindings for key in b.keys)

    for view in LayoutViewer.VIEWS:
        display.show_view(view)
        QApplication.processEvents()
        assert errors == [], (view, errors)
        assert live_key_conflicts(display) == {}, view


def test_workout_screen_plays_and_pauses(viewer):
    LayoutViewer, display, errors = viewer
    display.build("workout", os.path.join(LayoutViewer.WORKOUT_DIR, LayoutViewer.DEFAULT_WORKOUT), 10)
    hud = display.workout_hud()
    before = hud.workoutSecondsPassed
    display.play()
    for _ in range(3):
        display.play_reading()
        hud.lastTimestamp -= 1   #a second of the HUD's own clock
        hud.process_events()
    display.pause()
    assert hud.workoutSecondsPassed > before and not display.playing
    assert errors == []


@pytest.mark.parametrize("workout", ["01_quick_message_test.awo", "04_ramp_ladder_positions.awo", "ten_x.awo",
                                     "free_first_interval.awo", "shallow_ramp.awo", "ramp_timing.awo"])
def test_scratchpad_workouts_ride_to_the_end(viewer, workout):
    LayoutViewer, display, errors = viewer
    display.build("workout", os.path.join(LayoutViewer.WORKOUT_DIR, "scratchpad", workout), 100)
    QApplication.processEvents()
    assert errors == []
    assert AppState.blockGrades
    rows = WorkoutGrading.workout_rows(AppState.workoutList, AppState.blockGrades)
    assert rows and all(row["kind"] == "interval" or row["label"] for row in rows)


def test_spiral_follows_the_ride(viewer):
    """The workout screen's spiral: the workout's ring and the block's spiral as far along as the ride (an interval's
    spiral all its reps), with what was ridden in them"""
    LayoutViewer, display, errors = viewer
    #(10x 4s/2s, then 3x 2s/2s/2s: 40% of the way is in the first interval's reps)
    display.build("workout", os.path.join(LayoutViewer.WORKOUT_DIR, "scratchpad", "ten_x.awo"), 40)
    QApplication.processEvents()
    assert errors == []
    hud = display.workout_hud()
    spiral = hud.spiralWorkoutProgress
    assert hud.currentEventText[0] == 3 and len(hud.currentEventText) == 4
    assert spiral.workout.duration == AppState.workoutDurationSeconds
    assert spiral.workout.fraction() == pytest.approx(hud.workoutSecondsPassed/AppState.workoutDurationSeconds)
    #the interval's every rep along the one spiral
    assert spiral.block.duration == 10*(4 + 2)
    assert spiral.block.seconds == pytest.approx(hud.int0PassedTime + hud.int1PassedTime)
    #what was ridden is in them
    assert spiral.workout.values()[0][0] is not None and spiral.block.samples
    assert all(0 <= seconds <= timeline.duration
               for timeline in (spiral.workout, spiral.block) for seconds, _, _ in timeline.samples)


def test_converted_blocks_ride_to_the_end(viewer, tmp_path):
    """One block intervals and same power ramps, as the setup screen writes them out, ridden start to finish"""
    LayoutViewer, display, errors = viewer
    path = tmp_path / "converted.awo"
    from test_app_flow import CONVERTED_WORKOUT
    path.write_text(CONVERTED_WORKOUT, encoding="utf-8")
    display.build("workout", str(path), 100)
    QApplication.processEvents()
    assert errors == []
    assert AppState.workoutDurationSeconds == 20 + 60 + 40 + 20 + 40
    #(LayoutViewer's simulated ride stops a tick before the workout's end: its last block isn't graded there. The
    #whole thing is graded in test_app_flow.py's ride of the same workout)
    assert len(AppState.blockGrades) >= 7


def test_cadence_synopsis_shows_the_songs_heard(viewer):
    """Musical Cadence Mode's synopsis: a band for each song along the top of the graph, where it was heard"""
    LayoutViewer, display, errors = viewer
    display.build("cadence", os.path.join(LayoutViewer.WORKOUT_DIR, LayoutViewer.DEFAULT_WORKOUT), 100)
    QApplication.processEvents()
    assert errors == []
    synopsis = display.views["Synopsis"]
    strip = synopsis.songStrip
    assert strip is not None
    assert [title for _, _, title in strip.songs] == [title for title, _ in LayoutViewer.VIEWER_SONGS]
    #back to back, each as long as the song (the ride stops before the last one is over)
    for (_, end, _), (nextStart, _, _) in zip(strip.songs, strip.songs[1:]):
        assert end == nextStart
    for (start, end, _), (_, blocks) in zip(strip.songs[:-1], LayoutViewer.VIEWER_SONGS):
        assert abs((end - start) - sum(seconds for seconds, _ in blocks)) <= 1

    #clicking a song zooms to it, clicking it again zooms back out
    start, end, _ = strip.songs[1]
    synopsis.song_clicked(1)
    viewStart, viewEnd = synopsis.view_range()
    assert abs(viewStart - start) < 0.5 and abs(viewEnd - end) < 0.5
    assert [index for index, _, _ in strip.spans()] == [1]
    synopsis.song_clicked(1)
    viewStart, viewEnd = synopsis.view_range()
    assert viewStart < 0.5 and abs(viewEnd - synopsis.totalSeconds) < 0.5

    #a block's grade names its song
    key = min(AppState.blockGrades, key=lambda key: AppState.blockGrades[key]["start"])
    assert LayoutViewer.VIEWER_SONGS[0][0] in synopsis.grade_details_html(AppState.blockGrades[key])


def test_max_stars_ride_to_the_selection_max(viewer, monkeypatch):
    """Zooming away from the power max sends its star along the curve to the selection's max, where it settles
    above it; every max is marked by a star"""
    LayoutViewer, display, errors = viewer
    display.build("workout", os.path.join(LayoutViewer.WORKOUT_DIR, LayoutViewer.DEFAULT_WORKOUT), 40)
    display.show_view("Synopsis")
    QApplication.processEvents()
    synopsis = display.views["Synopsis"]
    import AppTheme
    import ClassSynopsisWindow
    clock = [1000.0]
    monkeypatch.setattr(ClassSynopsisWindow.time, "monotonic", lambda: clock[0])
    assert synopsis.maxMarkers and all("star" in marker and "target" not in marker for marker in synopsis.maxMarkers)
    power = synopsis.maxMarkers[0]
    assert power["vbox"] is synopsis.synopsisGraphLabel.powerVBox

    #a minute on the far side of the ride from the max
    total = synopsis.totalSeconds
    start = 0 if power["fullX"] > total / 2 else total - 60
    synopsis.synopsisGraphLabel.plotScreen.vb.setXRange(start, start + 60, padding=0)
    synopsis.update_selection()
    star = power["star"]
    assert power["x"] != power["fullX"] and star.x == power["x"]
    assert star.tripFrom == power["fullX"] and 0.5 <= star.tripSeconds <= 1.4

    #halfway: on its way, near the line
    clock[0] += star.tripSeconds / 2
    synopsis.animate_stars()
    assert min(start, power["x"]) <= star.at <= max(start + 60, power["x"])
    #there: resting above the marker
    clock[0] += star.tripSeconds
    for _ in range(40):
        synopsis.animate_stars()
    pt = power["vbox"].mapViewToScene(QPointF(power["x"], power["y"]))
    assert abs(power["star"].pos().x() - pt.x()) < 0.01
    assert abs(power["star"].pos().y() - (pt.y() - star.rest_offset())) < 0.01
    assert power["star"].isVisible()
    #the layout viewer lists the star's colors with the ones a theme can change
    assert {ClassSynopsisWindow.MaxStar.COLOR, ClassSynopsisWindow.MaxStar.EDGE_COLOR} <= LayoutViewer.widget_colors(synopsis)[0]
    assert errors == []


def test_workout_graph_stars_ride_to_a_new_max(viewer, monkeypatch):
    """The workout screen's graph: a star over each sensor curve's highest reading in view; a new higher reading
    sends it along the curve to that one, where it settles"""
    LayoutViewer, display, errors = viewer
    display.build("workout", os.path.join(LayoutViewer.WORKOUT_DIR, LayoutViewer.DEFAULT_WORKOUT), 40)
    display.show_view("Workout")
    hud = display.views["Workout"]
    hud.progressGraph.setMinimumHeight(160) #(as tall as on a full size screen: the test window squeezes it)
    QApplication.processEvents()
    import time
    import ClassHUDWindow
    import ClassMaxStar
    clock = [1000.0]

    class Clock:
        #(the stars' and the workout screen's clock only: the gauges keep the real one)
        def __getattr__(self, name):
            return (lambda: clock[0]) if name == "monotonic" else getattr(time, name)
    for module in (ClassMaxStar, ClassHUDWindow):
        monkeypatch.setattr(module, "time", Clock())
    start = hud.progressGraph.plotScreen.vb.viewRange()[0][0]
    hud.update_max_stars(start)
    star, savedName = hud.maxStars[0]
    assert savedName == "savedPowerVals" and star.shown
    times, values = AppState.savedPowerVals
    best = max(v for t, v in zip(times, values) if t >= start)
    assert star.y == best
    oldX = star.x

    #a new best a second after the last reading
    times.append(times[-1] + 1)
    values.append(best + 50)
    hud.hud_graph_data_update()
    assert star.x == times[-1] and star.tripFrom == oldX
    clock[0] += star.tripSeconds + 0.1
    for _ in range(40): #(its gap over the line eases to a new one)
        hud.step_max_stars()
    pt = star.vbox.mapViewToScene(QPointF(star.x, star.y))
    assert abs(star.pos().x() - pt.x()) < 0.01
    #just over the new best, clear of its diamond, with room for it under the graph's top
    assert star.markerReach == hud.CURRENT_MARKER_REACH
    assert abs(star.pos().y() - (pt.y() - star.rest_offset())) < 0.01
    graphTop = star.vbox.mapViewToScene(QPointF(0, star.vbox.viewRange()[1][1])).y()
    assert pt.y() - star.headroom() >= graphTop - 0.01

    #its bubble, once grown in: the new best, beside the star and clear of it and of the line
    bubble = hud.maxBubbles[0]
    for _ in range(2): #(the bubble from before pops first)
        clock[0] += 1
        hud.step_max_stars()
    assert bubble.state == "shown" and bubble.isVisible() and bubble.text == f"Max Pwr: {best + 50:.0f} W"
    rect = bubble.sceneBoundingRect()
    assert not rect.intersects(star.sceneBoundingRect())
    assert rect.bottom() < pt.y() - star.line_reach()
    #with room for it under the graph's top, popping included, and still after the graph changes size
    assert pt.y() - bubble.headroom() >= graphTop - 0.01 and rect.top() >= graphTop
    hud.progressGraph.setMinimumHeight(120)
    QApplication.processEvents()
    graphTop = star.vbox.mapViewToScene(QPointF(0, star.vbox.viewRange()[1][1])).y()
    pt = star.vbox.mapViewToScene(QPointF(star.x, star.y))
    assert pt.y() - bubble.headroom() >= graphTop - 0.01
    #the next best pops it; a new one appears once the star gets there
    times.append(times[-1] + 1)
    values.append(best + 80)
    hud.hud_graph_data_update()
    hud.step_max_stars()
    assert bubble.state == "popping"
    clock[0] += bubble.POP_SECONDS
    hud.step_max_stars()
    assert not bubble.isVisible()
    clock[0] += star.tripSeconds
    hud.step_max_stars()
    assert bubble.state == "appearing" and bubble.text == f"Max Pwr: {best + 80:.0f} W"
    assert errors == []
