"""The HUD's spiral (ClassPowerSpiral.py): the workout's ring and the block's spiral, what was ridden along them, where
everything is drawn, the cadence plotted across the lane and filled under in the power's color"""

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


###Cadence to height
def test_cadence_share():
    assert cadence_share(0) == 0
    assert cadence_share(39) == 0
    assert cadence_share(ClassPowerSpiral.CADENCE_MIN_RPM) == 0
    assert cadence_share((ClassPowerSpiral.CADENCE_MIN_RPM + ClassPowerSpiral.CADENCE_MAX_RPM)/2) == pytest.approx(0.5)
    assert cadence_share(ClassPowerSpiral.CADENCE_MAX_RPM) == 1
    assert cadence_share(140) == 1
    #no cadence to show: the lane's outer edge
    assert cadence_share(None) == 1


###A timeline's bins
def test_timeline_bins_average_and_carry_on():
    timeline = Timeline(BINS) #a second a bin
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


def bar_middle(theta):
    """The middle of the bin at angle theta"""
    step = 2*math.pi/BINS
    return (math.floor(theta/step) + 0.5)*step


def on_ring(spiral, image, fraction, offset=0.0):
    """The image's color on the ring's middle (or offset from it), on the bin fraction of the way round"""
    g = spiral.geometry_now()
    return at(image, g, bar_middle(2*math.pi*fraction), g["ringR"] + offset)


def on_spiral(spiral, image, fraction):
    """The image's color on the spiral's middle, on the bin fraction of the way along it"""
    g = spiral.geometry_now()
    theta = bar_middle(2*math.pi*BLOCK_TURNS*fraction)
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
    spiral.add_sample(330, 0, 250, 20)   #then coasting: only the line, on the lane's inner edge
    spiral.set_progress(360, 600, 0, 60)
    image = drawn(spiral)
    expected = FtpColors.zone_color(250, FtpColors.power_ranges())
    assert close(on_ring(spiral, image, 0.25), expected)
    assert on_ring(spiral, image, 0.25).alpha() == 255
    #nothing but the faint track where the rider coasted, and past where the workout's got to
    assert on_ring(spiral, image, 0.57).alpha() < 60
    assert on_ring(spiral, image, 0.8).alpha() < 60


def test_filled_under_the_cadence_line_see_through_above(qapp):
    """Halfway between CADENCE_MIN_RPM and CADENCE_MAX_RPM: the line across the lane's middle, the power's color
    under it, nothing but the faint track over it, and no gaps along it"""
    AppState.ftp = "200"
    spiral = spiral_of((600, 600))
    ride(spiral, 300, 150, (ClassPowerSpiral.CADENCE_MIN_RPM + ClassPowerSpiral.CADENCE_MAX_RPM)/2, block=False)
    spiral.set_progress(300, 600, 0, 60)
    image = drawn(spiral)
    lane = spiral.geometry_now()["lane"]
    fill = FtpColors.zone_color(150, FtpColors.power_ranges())
    for fraction in (0.1, 0.2, 0.2 + 0.5/BINS, 0.3):
        under = on_ring(spiral, image, fraction, -lane/4)
        assert under.alpha() == 255 and close(under, fill), fraction
        assert on_ring(spiral, image, fraction, lane/4).alpha() < 60, fraction
    #the line darker than what's under it
    assert on_ring(spiral, image, 0.2).lightness() < fill.lightness() - 40


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
    spiral = spiral_of((600, 600), block=90) #(big enough that a bar on the inner turns is a few pixels across)
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
    ride(spiral, 50, 200, 20)   #(coasting: only the line, on the lane's inner edge)
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
    (False, 9.5, None, None),   #no cadence sensor: the lane's outer edge
    (True, 9.5, 9.5, 90),       #both coming in: the cadence
    (True, 9.5, 2.0, None),     #the cadence sensor's gone quiet, the power hasn't: the lane's outer edge
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
    hud = SimpleNamespace(workoutSecondsPassed=5.0, SPIRAL_CADENCE_AVERAGE_SECONDS=1.0,
                          spiralWorkoutProgress=SimpleNamespace(add_sample=lambda *sample: got.append(sample)))
    hud.recent_average = lambda saved, *seconds: HUDWindow.recent_average(hud, saved, *seconds)
    hud.spiral_block_time = lambda: (3.0, 60)
    HUDWindow.record_spiral_sample(hud)
    power = 200 if powerAt > 5 else 0
    assert got == [(5.0, 3.0, power, expected)]


def test_power_rings_put_the_power_now_in_the_middle(qapp):
    """The power gauge's rings: the power now innermost, then out from it each finished block's average, newest to
    oldest (a reading every tick: 100 W, 300 W from 3 s, the last reading 200 W)"""
    import ClassHUDWindow
    AppState.ftp = "250"
    AppState.secondsPassed = 0.0
    AppState.savedPowerVals = ([], [])
    gauge = ClassHUDWindow.new_power_gauge()
    widget = gauge.ui.widget
    widget.center_ring_min_px = None
    widget.center_ring_count = 5
    rings = ClassHUDWindow.PowerRings()
    for tick in range(1, 51):
        AppState.secondsPassed = tick/10
        power = 200 if tick == 50 else 100 if tick < 30 else 300
        AppState.savedPowerVals[0].append(AppState.secondsPassed)
        AppState.savedPowerVals[1].append(power)
        widget.setValue(power)
        rings.update(gauge)
    #the blocks end half a tick after the first (0.1 s): the newest finished one ends at 4.55 s, the oldest of the four
    #shown (2.55 to 3.05 s) has four 100 W readings and one 300 W
    color = lambda power: FtpColors.zone_color(power, FtpColors.power_ranges()).name()
    assert [c.name() for c in widget.center_rings[0]] == [color(140), color(300), color(300), color(300), color(200)]


def test_cadence_rings_gear_turns_and_beats_with_the_chainring(qapp):
    """The cadence gauge's rings: the outermost a gear turned with the chainring behind it (its teeth's tips at the
    ring's edge, the gaps between them in from it), and outside Musical Cadence Mode brightest as the chainring comes
    round, fading over BEAT_SECONDS"""
    from types import SimpleNamespace
    import ClassHUDWindow
    from ClassAnalogGauge import AnalogGaugeWidget, Gauge
    gauge = Gauge(None, 'Chainring.svg', spin=True)
    chainring, widget = gauge.ui.svg, gauge.ui.widget
    chainring.spinTimer.stop()
    assert widget.center_gear_angle == 0.0
    widget.setMaxValue(200)
    widget.setValue(60)
    chainring.step() #(the first step from stopped only starts timing)
    chainring.lastStep -= 0.5
    chainring.step()
    assert chainring.angle == pytest.approx(180, abs=5)
    assert widget.center_gear_angle == chainring.angle
    gear = widget.center_gear(100, 10)
    assert len(gear) == AnalogGaugeWidget.CENTER_GEAR_TEETH*4
    assert sorted({round(math.hypot(p.x(), p.y()), 6) for p in gear}) == [90, 100]
    #(60 rpm: 360 degrees a second)
    hud = SimpleNamespace(cadenceMode=False, currentPower=0, cadenceGaugeDisplay=gauge)
    for angle, level in ((0, 1.0), (360*ClassHUDWindow.BEAT_SECONDS/2, 0.5), (180, 0.0)):
        chainring.angle = angle
        ClassHUDWindow.HUDWindow.update_center_rings(hud)
        assert widget.center_rings[1] == pytest.approx(level)
    #stopped: no beat
    chainring.angle = 0
    widget.setValue(0)
    ClassHUDWindow.HUDWindow.update_center_rings(hud)
    assert widget.center_rings[1] == 0


###The session's earlier rides (see Session.py)
def test_ring_carries_on_from_the_sessions_rides(qapp):
    """The ring starts with the rides before this workout, each in the zones it was ridden in, a tick where each
    ends; this workout goes on from there"""
    AppState.ftp = "200"
    now = FtpColors.power_ranges()
    #ridden when the FTP was 400: 250 W was a low zone then
    then = [limit*2 for limit in now]
    spiral = PowerSpiral()
    spiral.resize(300, 300)
    spiral.clear(300, [(300, [(t + 0.5, 250, 110) for t in range(300)], then)])
    spiral.set_block(60)
    #half the ring is the earlier ride, this workout's yet to start
    assert spiral.workout.duration == 600 and spiral.workout.fraction() == pytest.approx(0.5)
    ride(spiral, 290, 250, 110, block=False)
    spiral.set_progress(290, 300, 0, 60)
    assert spiral.workout.seconds == pytest.approx(590)
    image = drawn(spiral)
    assert close(on_ring(spiral, image, 0.25), FtpColors.zone_color(250, then))
    assert close(on_ring(spiral, image, 0.75), FtpColors.zone_color(250, now))
    assert FtpColors.zone_color(250, then) != FtpColors.zone_color(250, now)
    #the tick across the ring where the earlier ride ends, in the line's color
    g = spiral.geometry_now()
    tick = at(image, g, math.pi, g["ringR"])
    line = QColor(AppTheme.THEME.map_hex(ClassPowerSpiral.LINE_COLOR))
    assert close(tick, line, by=40)

    #what this workout added, for the session's next ride: its own seconds, one sample a second
    seconds, samples = spiral.ridden()
    assert seconds == pytest.approx(290)
    #(the ride's 0.1s steps add up to just under 290)
    assert len(samples) in (290, 291) and samples[0] == (0.5, pytest.approx(250), pytest.approx(110))
    assert all(b[0] - a[0] == 1 for a, b in zip(samples, samples[1:]))


def test_ring_without_rides_before_is_the_workout(qapp):
    spiral = spiral_of((300, 300), workout=600)
    assert spiral.workout.duration == 600 and spiral.historyRanges == []
    spiral.set_progress(60, 600, 0, 60)
    assert spiral.workout.fraction() == pytest.approx(0.1)
    assert spiral.ridden()[0] == pytest.approx(60)
