"""AntDevicesPoller: ANT readings onto the workout screen's gauges, the hold-last-value smoothing, the
cadence/speed fallbacks and the sensor history, fed by a real bridge on a fake stick"""

import pytest

from AppState import AppState
from ClassAntDevicesPoller import AntDevicesPoller, SPEED_CALIBRATION_MIN_SAMPLES
import fakes
import PowerCalibration
import WorkoutData
from WorkoutHelpers import HOLD_CADENCE_READINGS, HOLD_HR_READINGS, HOLD_POWER_READINGS, HOLD_SPEED_READINGS
from test_ant_bridge import bridge, clock, connect, native, power  # noqa: F401 (fixtures)


class Gauge:
    def __init__(self):
        self.ui = type("ui", (), {})()
        self.ui.widget = self
        self.values = []
        self.value_decimals = 1
        self.outer_circle_bg = None

    def setValue(self, value):
        self.values.append(value)


class HUD:
    def __init__(self, bridge):
        self.parent = type("main", (), {"startExe": bridge})()
        self.wattGaugeDisplay, self.hrGaugeDisplay, self.cadenceGaugeDisplay = Gauge(), Gauge(), Gauge()
        self.speedValueLabel = self.distValueLabel = None
        self.speeds, self.distances = [], []

    def set_speed(self, text):
        self.speeds.append(float(text))

    def set_distance(self, text):
        self.distances.append(float(text))


@pytest.fixture
def ride(bridge, native):
    """A started poller: (poller, hud, feed) where feed(message) delivers one reading"""
    WorkoutData.clear()
    AppState.wheelCircumference = "2.0"

    def start(selection, hrm=False, speedCadence=False, cadence=False, speed=False, cadenceMode=False):
        AppState.workoutMode = AppState.cadenceMode = cadenceMode
        AppState.antHRM, AppState.antSpeedCadence, AppState.antCadence, AppState.antSpeed = \
            hrm, speedCadence, cadence, speed
        connect(bridge, selection)
        hud = HUD(bridge)
        poller = AntDevicesPoller(hud)
        poller.start()
        return poller, hud
    return start


def meter(watts, cadence=90):
    return fakes.power_page(100, watts, cadence=cadence, channel=0, transmission_type=5,
                            event_count=fakes.unique_payload_byte())


###Power
def test_power_readings_and_hold(ride, native):
    poller, hud = ride({"power": (11, 100)})
    for watts in (200, 210, 0, 0, 0, 0, 220):
        native.deliver(meter(watts))
    #three zeros could be misreads: the last good value shows instead
    assert hud.wattGaugeDisplay.values == [200, 210, 210, 210, 210, 0, 220]
    assert [row[1] for row in WorkoutData.power.rows()] == [200, 210, 0, 0, 0, 0, 220]
    assert AppState.savedPowerVals[1] == [200, 210, 210, 210, 210, 0, 220]
    assert hud.wattGaugeDisplay.outer_circle_bg is not None
    assert HOLD_POWER_READINGS == 3


def test_power_calibration_is_applied(ride, native):
    PowerCalibration.save(11, 100, offset_watts=10, scale=1.0)
    poller, hud = ride({"power": (11, 100)})
    native.deliver(meter(210))
    assert hud.wattGaugeDisplay.values == [200]


def test_power_graph_keeps_its_window(ride, native):
    poller, hud = ride({"power": (11, 100)})
    window = AppState.graphSecondsShown - AppState.graphBreakpoint
    for second in range(window + 50):
        AppState.secondsPassed = float(second)
        native.deliver(meter(200 + second % 7))
    times = AppState.savedPowerVals[0]
    assert times[-1] - times[0] <= window and len(times) == window + 1


def test_inactive_poller_ignores_readings(ride, native):
    poller, hud = ride({"power": (11, 100)})
    poller.active = False
    native.deliver(meter(200))
    assert hud.wattGaugeDisplay.values == []


###Heart rate
def test_heart_rate_and_hold(ride, native):
    poller, hud = ride({"power": (11, 100), "hrm": (120, 200)}, hrm=True)
    native.deliver(meter(100))
    bpms = [150] + [0]*(HOLD_HR_READINGS + 1)
    for i, bpm in enumerate(bpms):
        native.deliver(fakes.hr_page(200, bpm, i, 1000 + i, channel=0, transmission_type=1))
    assert hud.hrGaugeDisplay.values == [150]*(HOLD_HR_READINGS + 1) + [0]
    assert len(WorkoutData.heartRate) == len(bpms)


###Cadence
def combined(i, cadence_rpm=90.0, wheel_mps=8.0, step_ticks=1024):
    """The i'th combined speed/cadence page, a second (step_ticks) apart"""
    cadence_revs = round(i*cadence_rpm/60)
    speed_revs = round(i*wheel_mps/2.0)
    return fakes.combined_page(300, (i*step_ticks) & 0xFFFF, cadence_revs & 0xFFFF, (i*step_ticks) & 0xFFFF,
                               speed_revs & 0xFFFF, channel=0, transmission_type=1)


def test_combined_sensor_cadence_speed_and_distance(ride, native):
    poller, hud = ride({"power": (11, 100), "speed_cadence": (121, 300)}, speedCadence=True)
    for i in range(4):
        native.deliver(combined(i, cadence_rpm=60))
    #the first reading is only the baseline
    assert hud.cadenceGaugeDisplay.values == pytest.approx([60, 60, 60])
    #4 revolutions of a 2m wheel a second: 8 m/s
    assert hud.speeds[-1] == pytest.approx(8*2.23694, abs=0.1)
    assert AppState.totalRevolutions == 12
    assert hud.distances[-1] == pytest.approx(24*0.000621371, abs=0.01)
    assert len(WorkoutData.speedCadence) == 4


def test_cadence_rolls_over(ride, native):
    poller, hud = ride({"speed_cadence": (121, 300)}, speedCadence=True)
    #the event time and revolution counters wrap at 16 bits
    native.deliver(fakes.combined_page(300, 65000, 65535, 65000, 65534, channel=0, transmission_type=1))
    native.deliver(fakes.combined_page(300, (65000 + 1024) & 0xFFFF, 1, (65000 + 1024) & 0xFFFF, 2, channel=0,
                                       transmission_type=1))
    assert hud.cadenceGaugeDisplay.values == pytest.approx([120])


def test_cadence_hold_and_known_garbage(ride, native):
    poller, hud = ride({"speed_cadence": (121, 300)}, speedCadence=True)
    native.deliver(fakes.combined_page(300, 1000, 10, 1000, 10, channel=0, transmission_type=1))
    native.deliver(fakes.combined_page(300, 2024, 11, 2024, 14, channel=0, transmission_type=1))
    #a cadence event time the sensor is known to glitch to is skipped
    native.deliver(fakes.combined_page(300, 513, 12, 3048, 18, channel=0, transmission_type=1))
    assert hud.cadenceGaugeDisplay.values == pytest.approx([60])
    #(a stop: the same counters, a later speed event)
    native.deliver(fakes.combined_page(300, 3048, 12, 4072, 18, channel=0, transmission_type=1))
    assert hud.cadenceGaugeDisplay.values[-1] == pytest.approx(60)


def test_cadence_falls_back_to_the_power_meter(ride, native, bridge):
    poller, hud = ride({"power": (11, 100), "cadence": (122, 400)}, cadence=True)
    native.deliver(fakes.bsc_page(122, 400, 1024, 1, channel=1, transmission_type=1))
    native.deliver(fakes.bsc_page(122, 400, 2048, 2, channel=1, transmission_type=1))
    assert hud.cadenceGaugeDisplay.values == pytest.approx([60])
    #the cadence sensor drops out
    bridge.selected_device("cadence").mark_state("LOST")
    native.deliver(meter(200, cadence=85))
    assert hud.cadenceGaugeDisplay.values[-1] == 85
    assert hud.cadenceGaugeDisplay.value_decimals == 0
    assert [row[1] for row in WorkoutData.fallbackCadence.rows()] == [85]
    #and the fallback's 0 isn't replaced with the sensor's old reading
    native.deliver(meter(0, cadence=0))
    assert hud.cadenceGaugeDisplay.values[-1] == 85
    #back again: the baseline starts over
    bridge.selected_device("cadence").mark_state("ACTIVE")
    native.deliver(fakes.bsc_page(122, 400, 3072, 3, channel=1, transmission_type=1))
    assert hud.cadenceGaugeDisplay.values[-1] == 85
    native.deliver(fakes.bsc_page(122, 400, 4096, 4, channel=1, transmission_type=1))
    assert hud.cadenceGaugeDisplay.values[-1] == pytest.approx(60)
    assert hud.cadenceGaugeDisplay.value_decimals == 1


def test_cadence_holds_through_short_zeros(ride, native):
    #Musical Cadence Mode: cadence from the power meter alone
    poller, hud = ride({"power": (11, 100)}, cadenceMode=True)
    assert hud.cadenceGaugeDisplay is poller.cadenceGauge
    native.deliver(meter(200, cadence=80))
    for _ in range(HOLD_CADENCE_READINGS + 1):
        native.deliver(meter(0, cadence=0))
    assert hud.cadenceGaugeDisplay.values[:-1] == [80]*(HOLD_CADENCE_READINGS + 1)
    assert hud.cadenceGaugeDisplay.values[-1] == 0


###Speed
def speed_page(i, revs_per_second=4):
    return fakes.bsc_page(123, 500, (i*1024) & 0xFFFF, (i*revs_per_second) & 0xFFFF, channel=1, transmission_type=1)


def test_speed_sensor_and_hold(ride, native):
    poller, hud = ride({"power": (11, 100), "speed": (123, 500)}, speed=True)
    for i in range(3):
        native.deliver(speed_page(i))
    #stopped
    for i in range(3, 3 + HOLD_SPEED_READINGS + 1):
        native.deliver(fakes.bsc_page(123, 500, (i*1024) & 0xFFFF, 8, channel=1, transmission_type=1))
    mph = 8*2.23694
    assert hud.speeds == pytest.approx([round(mph, 1)]*(2 + HOLD_SPEED_READINGS) + [0])
    assert len(WorkoutData.speed) == 3 + HOLD_SPEED_READINGS + 1


def test_speed_estimate_while_the_sensor_is_out(ride, native, bridge):
    poller, hud = ride({"power": (11, 100), "speed": (123, 500)}, speed=True)
    #learn the power -> speed curve from real readings
    for i in range(SPEED_CALIBRATION_MIN_SAMPLES + 1):
        native.deliver(meter(100 + 20*i))
        native.deliver(speed_page(i, revs_per_second=2 + i))
    assert len(poller._speedCalSamples) == SPEED_CALIBRATION_MIN_SAMPLES
    distance = hud.distances[-1]
    bridge.selected_device("speed").mark_state("LOST")
    native.deliver(meter(150))
    #an estimate is shown, but adds no distance
    assert 0 < hud.speeds[-1] < 60
    assert hud.distances[-1] == distance


def test_no_speed_estimate_from_a_trainer(ride, native, bridge):
    native.deliver(fakes.trainer_page(300, 200, channel=0, transmission_type=5))
    poller, hud = ride({"power": (17, 300), "speed": (123, 500)}, speed=True)
    poller._speedCalSamples = [(0, w, w/10) for w in range(100, 400, 50)]
    native.deliver(speed_page(0))
    bridge.selected_device("speed").mark_state("LOST")
    native.deliver(fakes.trainer_page(300, 250, channel=0, transmission_type=5))
    assert hud.speeds == []


@pytest.mark.parametrize("watts, mph", [(0, 0.0), (-5, 0.0), (50, 5.0), (100, 10.0), (150, 12.5), (200, 15.0),
                                        (250, 17.5), (300, 20.0), (500, 20.0)])
def test_speed_estimate_curve(ride, watts, mph):
    poller, hud = ride({"power": (11, 100)})
    poller._speedCalSamples = [(0, 100, 10.0), (0, 200, 15.0), (0, 200, 15.0), (0, 300, 20.0), (0, 300, 20.0)]
    assert poller._estimate_speed_from_power(watts) == pytest.approx(mph)


def test_speed_estimate_needs_enough_samples(ride):
    poller, hud = ride({"power": (11, 100)})
    poller._speedCalSamples = [(0, 100, 10.0)]
    assert poller._estimate_speed_from_power(100) is None


def test_speed_calibration_ages_out(ride, monkeypatch):
    import ClassAntDevicesPoller as module
    poller, hud = ride({"power": (11, 100)})
    now = [1000.0]
    monkeypatch.setattr(module.time_module, "monotonic", lambda: now[0])
    poller._record_speed_calibration(100, 10)
    now[0] += module.SPEED_CALIBRATION_MAX_AGE_S + 1
    poller._record_speed_calibration(200, 15)
    assert [(p, s) for _, p, s in poller._speedCalSamples] == [(200, 15)]


###Stopping
def test_stop_holds_a_trainer_at_zero(ride, native, bridge):
    native.deliver(fakes.trainer_page(300, 200, channel=0, transmission_type=5))
    poller, hud = ride({"power": (17, 300)})
    bridge.set_trainer_target_power(250)
    poller.stop()
    assert bridge._trainer_target["watts"] == 0 and not poller.active
    #stopping twice is fine
    poller.stop()


def test_ftp_color_follows_the_power_zones(ride):
    poller, hud = ride({"power": (11, 100)})
    AppState.ftp = "200"
    assert poller.getFtpColor(0).startswith("#44")
    assert poller.getFtpColor(150) != poller.getFtpColor(400)
