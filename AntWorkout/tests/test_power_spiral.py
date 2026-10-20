"""The HUD's spiral (ClassPowerSpiral.py): the workout's ring and the block's spiral, what was ridden along them, where
everything is drawn, the line's thickness from the cadence and its color from the power"""

import math

import pytest
from PyQt6.QtCore import QPoint, QPointF
from PyQt6.QtGui import QColor, QImage, QRegion
from PyQt6.QtWidgets import QWidget

from AppState import AppState
import AppTheme
import FtpColors
import ClassPowerSpiral
from ClassPowerSpiral import BINS, BLOCK_TURNS, PowerSpiral, Timeline, cadence_share


@pytest.fixture(autouse=True)
def fresh_theme():
    AppTheme.THEME.reset()
    FtpColors._values.clear()
    yield
    AppTheme.THEME.reset()
    FtpColors._values.clear()


###Cadence to thickness
def test_cadence_share():
    assert cadence_share(0) == 0
    assert cadence_share(39) == 0
    assert cadence_share(ClassPowerSpiral.CADENCE_MIN_RPM) == 0
    assert cadence_share((ClassPowerSpiral.CADENCE_MIN_RPM + ClassPowerSpiral.CADENCE_MAX_RPM)/2) == pytest.approx(0.5)
    assert cadence_share(ClassPowerSpiral.CADENCE_MAX_RPM) == 1
    assert cadence_share(140) == 1
    #no cadence to show: the whole lane
    assert cadence_share(None) == 1


###A timeline's bins
def test_timeline_bins_average_and_carry_on():
    timeline = Timeline(180) #a second a bin
    timeline.add(0.2, 100, 80)
    timeline.add(0.7, 200, 90)
    timeline.add(5.5, 300, None)
    values = timeline.values()
    assert values[0] == (150, 85)
    #bins nothing landed in show the bin before
    assert values[1] == values[4] == (150, 85)
    #a sample without a cadence keeps the power
    assert values[5] == (300, None)
    assert values[BINS - 1] == (300, None)


def test_timeline_starts_with_its_first_sample():
    timeline = Timeline(60)
    assert timeline.values() == [(None, None)]*BINS
    #(the first sample comes a tick in: the bins before it show it, so the line starts at the start)
    timeline.add(1, 100, 80)
    timeline.add(30, 200, 90)
    values = timeline.values()
    assert values[0] == values[2] == (100, 80)
    assert values[BINS//2] == (200, 90)


def test_timeline_fraction_and_length_change():
    timeline = Timeline(100)
    timeline.seconds = 25
    assert timeline.fraction() == 0.25
    timeline.seconds = 250
    assert timeline.fraction() == 1
    #(a ramp test's workout grows): what was ridden moves to where it now falls
    timeline.add(50, 200, 90)
    assert timeline.values()[BINS//2][0] == 200
    timeline.add(90, 100, 90)
    timeline.set_duration(200)
    assert timeline.values()[BINS//4][0] == 200
    assert timeline.values()[BINS*90//200][0] == 100


def test_timeline_sample_past_the_end_lands_in_the_last_bin():
    timeline = Timeline(10, BINS*BLOCK_TURNS)
    timeline.add(12, 150, 90)
    assert len(timeline.values()) == BINS*BLOCK_TURNS
    assert timeline.values()[-1] == (150, 90)


###Where it's drawn
def spiral_of(size, block=60, workout=600):
    spiral = PowerSpiral()
    spiral.resize(*size)
    spiral.clear(workout)
    spiral.set_block(block)
    return spiral


def test_ring_outside_spiral_inside_down_to_the_hole(qapp):
    spiral = spiral_of((300, 400))
    g = spiral.geometry_now()
    #square, centered: the ring's outer edge touches the narrower side
    assert g["center"] == QPointF(150, 200)
    assert g["ringR"] + g["lane"]/2 == pytest.approx(150)
    #every lane the same width, SPACE_PX apart: the spiral starts a lane in from the ring
    assert g["pitch"] - g["lane"] == pytest.approx(ClassPowerSpiral.SPACE_PX)
    assert g["ringR"] - g["spiralR"] == pytest.approx(g["pitch"])
    #its last turn's inner edge on the hole
    assert spiral.spiral_radius(g)(2*math.pi*BLOCK_TURNS) - g["lane"]/2 == pytest.approx(g["rInner"])
    center, radius = spiral.inner_circle()
    assert center == g["center"] and radius < g["rInner"]


def test_grade_overlay_uses_the_hole(qapp):
    from ClassGradeWidgets import GradeOverlay
    spiral = spiral_of((300, 400))
    overlay = GradeOverlay(spiral)
    assert overlay.inner_circle() == spiral.inner_circle()


###What's drawn
def at(image, g, theta, r):
    return image.pixelColor(round(g["center"].x() + r*math.sin(theta)), round(g["center"].y() - r*math.cos(theta)))


def on_ring(spiral, image, fraction, offset=0.0):
    """The image's color on the ring's middle (or offset from it), fraction of the way round"""
    g = spiral.geometry_now()
    return at(image, g, 2*math.pi*fraction, g["ringR"] + offset)


def on_spiral(spiral, image, fraction):
    """The image's color on the spiral's middle, fraction of the way along it"""
    g = spiral.geometry_now()
    theta = 2*math.pi*BLOCK_TURNS*fraction
    return at(image, g, theta, spiral.spiral_radius(g)(theta))


def drawn(spiral):
    image = QImage(spiral.size(), QImage.Format.Format_ARGB32)
    image.fill(QColor(0, 0, 0, 0))
    #(without the window's background: what isn't drawn stays see-through)
    spiral.render(image, QPoint(), QRegion(), QWidget.RenderFlag.DrawChildren)
    return image


def ride(spiral, seconds, power, cadence, start=0.0, step=0.1, block=True):
    """Samples from start to seconds, into the workout and (block) the block at the same time"""
    t = start
    while t <= seconds:
        spiral.add_sample(t, t if block else 0, power, cadence)
        t += step


def close(a, b, by=3):
    return all(abs(x - y) <= by for x, y in zip(a.getRgb()[:3], b.getRgb()[:3]))


def test_ring_is_colored_by_power_and_as_thick_as_the_cadence(qapp):
    AppState.ftp = "200"
    spiral = spiral_of((300, 300))
    ride(spiral, 300, 250, 110)   #the first half spun at full width
    spiral.add_sample(330, 0, 250, 20)   #then coasting: no line
    spiral.set_progress(360, 600, 0, 60)
    image = drawn(spiral)
    expected = FtpColors.zone_color(250, FtpColors.power_ranges())
    assert close(on_ring(spiral, image, 0.25), expected)
    assert on_ring(spiral, image, 0.25).alpha() == 255
    #nothing but the faint track where the rider coasted, and past where the workout's got to
    assert on_ring(spiral, image, 0.57).alpha() < 60
    assert on_ring(spiral, image, 0.8).alpha() < 60


def test_finished_ring_meets_itself(qapp):
    spiral = spiral_of((300, 300))
    ride(spiral, 600, 150, 110, block=False)
    spiral.set_progress(600, 600, 0, 60)
    image = drawn(spiral)
    #all the way round, right up to North from either side
    for fraction in (0.002, 0.5, 0.998):
        assert on_ring(spiral, image, fraction).alpha() == 255, fraction


def test_spiral_is_the_same_length_whatever_the_block(qapp):
    """A 2 minute block and a 45 minute one, both half done, end in the same place"""
    for seconds in (120, 2700):
        spiral = spiral_of((300, 300), block=seconds, workout=3600)
        step = seconds/1200
        t = 0.0
        while t <= seconds/2:
            spiral.add_sample(t, t, 200, 100)
            t += step
        spiral.set_progress(seconds/2, 3600, seconds/2, seconds)
        image = drawn(spiral)
        assert on_spiral(spiral, image, 0.45).alpha() == 255, seconds
        assert on_spiral(spiral, image, 0.58).alpha() < 60, seconds


def test_spiral_is_colored_by_the_power_ridden_and_runs_through_its_turns(qapp):
    AppState.ftp = "200"
    spiral = spiral_of((300, 300), block=90)
    ride(spiral, 45, 120, 100)
    ride(spiral, 90, 300, 100, start=45.1)
    spiral.set_progress(90, 600, 90, 90)
    image = drawn(spiral)
    ranges = FtpColors.power_ranges()
    assert close(on_spiral(spiral, image, 0.2), FtpColors.zone_color(120, ranges))
    assert close(on_spiral(spiral, image, 0.8), FtpColors.zone_color(300, ranges))
    #every turn drawn, up to and across where they meet at North
    for fraction in [k/BLOCK_TURNS + d for k in range(1, BLOCK_TURNS) for d in (-0.003, 0.003)] + [0.999]:
        assert on_spiral(spiral, image, fraction).alpha() == 255, fraction


def test_without_cadence_the_line_is_full_width(qapp):
    spiral = spiral_of((300, 300))
    ride(spiral, 300, 150, None)
    spiral.set_progress(300, 600, 0, 60)
    g = spiral.geometry_now()
    #near the lane's edge too
    assert on_ring(spiral, drawn(spiral), 0.25, g["lane"]/2 - 1.5).alpha() > 200


def test_blinks(qapp):
    spiral = spiral_of((300, 300), block=100)
    ride(spiral, 50, 200, 100)
    spiral.set_progress(300, 600, 50, 100)
    spiral.set_blink("block")
    image = drawn(spiral)
    assert on_ring(spiral, image, 0.05).alpha() == 255
    assert on_spiral(spiral, image, 0.25).alpha() < 60
    spiral.set_blink("workout")
    image = drawn(spiral)
    assert on_ring(spiral, image, 0.05).alpha() < 60
    assert 100 < on_spiral(spiral, image, 0.25).alpha() < 200
    spiral.set_blink("default")
    assert on_spiral(spiral, drawn(spiral), 0.25).alpha() == 255


def test_a_new_block_is_drawn_as_its_own(qapp):
    """Two blocks the same length, the second ridden as far as the first was: it's drawn with its own cadence"""
    spiral = spiral_of((300, 300), block=100)
    ride(spiral, 50, 200, 110)
    spiral.set_progress(0, 600, 50, 100)
    assert on_spiral(spiral, drawn(spiral), 0.25).alpha() == 255
    spiral.set_block(100)
    ride(spiral, 50, 200, 20)   #(coasting: no line)
    spiral.set_progress(0, 600, 50, 100)
    assert on_spiral(spiral, drawn(spiral), 0.25).alpha() < 60


def test_redrawn_only_when_an_end_moves_enough_to_see(qapp, monkeypatch):
    spiral = spiral_of((300, 300))
    spiral.set_progress(0.0, 7200, 0.0, 60)
    drawn(spiral)
    updates = []
    monkeypatch.setattr(spiral, "update", lambda: updates.append(1))
    #a 2 hour workout's end barely moves in a frame
    spiral.set_progress(0.016, 7200, 0.0, 60)
    assert updates == []
    spiral.set_progress(0.016, 7200, 1.0, 60)
    assert updates


###What the HUD gives it
@pytest.mark.parametrize("cadenceSensor, powerAt, cadenceAt, expected", [
    (False, 9.5, None, None),   #no cadence sensor: the whole lane
    (True, 9.5, 9.5, 90),       #both coming in: the cadence
    (True, 9.5, 2.0, None),     #the cadence sensor's gone quiet, the power hasn't: the whole lane
    (True, 2.0, 2.0, 0),        #both quiet: not pedaling, no line
])
def test_hud_sample_cadence(cadenceSensor, powerAt, cadenceAt, expected):
    from types import SimpleNamespace
    from ClassHUDWindow import HUDWindow
    AppState.workoutMode, AppState.cadenceMode = True, False
    AppState.antSpeedCadence = AppState.antCadence = AppState.antSpeed = cadenceSensor
    AppState.secondsPassed = 10.0
    AppState.savedPowerVals = ([powerAt], [200])
    AppState.savedCadenceVals = ([cadenceAt], [90]) if cadenceAt is not None else ([], [])
    got = []
    hud = SimpleNamespace(workoutSecondsPassed=5.0,
                          spiralWorkoutProgress=SimpleNamespace(add_sample=lambda *sample: got.append(sample)))
    hud.recent_average = lambda saved: HUDWindow.recent_average(hud, saved)
    hud.spiral_block_time = lambda: (3.0, 60)
    HUDWindow.record_spiral_sample(hud)
    power = 200 if powerAt > 5 else 0
    assert got == [(5.0, 3.0, power, expected)]
