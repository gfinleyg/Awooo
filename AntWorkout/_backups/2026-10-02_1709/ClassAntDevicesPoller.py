import time as time_module

from PyQt6.QtCore import QObject
from PyQt6.QtGui import QColor

from ClassAnalogGauge import *
from ClassLongVerticalLabel import *
from ClassLongVerticalLabelSeries import *
from ClassSpiralProgressBar import *
from ClassWorkoutGraph import *
from ClassCustomTitleBar import *

from ClassAntDevices import (
    ROLE_HRM,
    ROLE_POWER,
    ROLE_SPEED_CADENCE,
    ROLE_CADENCE,
    ROLE_SPEED,
)
from AppState import AppState
import FtpColors
import PowerCalibration
import WorkoutData


DEVICE_TYPE_POWER_METER = 11
DEVICE_TYPE_FITNESS_EQUIPMENT = 17
DEVICE_TYPE_SPEED_CADENCE_COMBINED = 121

# How long a live power/speed pairing stays useful for the power->speed
# fallback lookup (see _estimate_speed_from_power). Old enough samples are
# dropped so a stale calibration - e.g. from before the rider manually
# touched their trainer's own resistance knob - ages out rather than
# lingering forever.
SPEED_CALIBRATION_MAX_AGE_S = 90
SPEED_CALIBRATION_MAX_SAMPLES = 900
SPEED_CALIBRATION_MIN_SAMPLES = 5


###AntWorkout.exe text file handling
###
### Was a QThread running its own 100ms while-loop, polling
### bridge.selected_device(...) on every tick regardless of whether
### anything new had actually arrived. Converted to a plain QObject that
### reacts to AntWorkoutBridge's dataUpdated signal instead - the signal
### only fires when a role's decoded data actually changed (see
### ClassAntDevices.py's _on_message), so gauge updates now land the
### instant a new ANT page decodes rather than up to 100ms late, and the
### CPU/thread stop entirely being idle-woken 10x/second with nothing to
### do most of the time.
###
### start()/stop() keep their original names/semantics (connect/
### disconnect from the bridge's signal instead of starting/stopping a
### thread) so ClassMainWindow.py's existing
### self.workoutHudWidget.antDevices.start()/.stop() call sites don't
### need to change.
###
### The loop body itself is unchanged line-for-line below (now living in
### _on_data_updated as a single pass instead of one while-loop
### iteration) - it was already self-gated on each role's own
### pages_received counter, so it's safe and correct to run the whole
### thing on every dataUpdated signal, whichever role triggered it.
class AntDevicesPoller(QObject):
    def __init__(self, parent):
        super(AntDevicesPoller, self).__init__()
        self.powerGauge = parent.wattGaugeDisplay
        if AppState.antHRM:
            self.hrGauge = parent.hrGaugeDisplay
        if AppState.shows_cadence():
            self.cadenceGauge = parent.cadenceGaugeDisplay
            self.speedLabel = parent.speedValueLabel
            self.distanceLabel = parent.distValueLabel
        self.parent = parent
        self.active = False

    def start(self):
        #AntWorkoutBridge (see ClassAntDevices.py) owns the live ANT
        #connection - self.parent is the HUDWindow instance, whose own
        #parent is MainWindow, matching the existing
        #self.parent.parent.go_back_to_setup() idiom used elsewhere.
        self.bridge = self.parent.parent.startExe

        #Per-role "pages_received" bookkeeping: a role's profile only has new
        #data to act on when its pages_received count has moved since the
        #last time we looked. Instance attributes now (rather than run()
        #locals) since each dataUpdated signal is a separate, independent
        #call into _on_data_updated rather than one iteration of a
        #standing loop - this state has to persist across those calls.
        self.lastPowerPages = -1
        self.lastHrPages = -1

        #Separate pages_received trackers for the cadence/speed sources,
        #since the "which source" can itself change mid-workout (a
        #dedicated sensor going stale/lost/stalled and falling back to
        #the power meter's own cadence field, or recovering back). Each
        #possible source gets its own counter so switching between them
        #doesn't get confused by a stale comparison against a different
        #device's page count.
        self.lastCadenceSourcePages = -1
        self.lastCadenceFallbackPmPages = -1
        self.lastSpeedSourcePages = -1
        self.lastSpeedFallbackPmPages = -1

        #Initialize these to zero, they'll get updated to non-zero once values are added
        self.lastNonZeroPower = 0
        self.lastNonZeroHR = 0
        self.lastNonZeroCadence = 0
        self.lastNonZeroSpeed = 0

        #Index values to track how many times 0 was seen
        self.lastNonZeroPowerIndex = 0
        self.lastNonZeroHRIndex = 0
        self.lastNonZeroCadenceIndex = 0
        self.lastNonZeroSpeedIndex = 0

        #reinitialize these to prevent lines between restarts
        AppState.savedHRVals = ([],[])
        AppState.savedPowerVals = ([],[])
        AppState.savedCadenceVals = ([],[])
        AppState.totalRevolutions = 0

        #Rollover-tracking state for cadence/speed raw-tick math (see
        #_cadence_from_ticks/_speed_from_ticks). Keyed by (device_type,
        #device_number) so a source change - falling back, recovering, or
        #the user having picked a different sensor - resets the delta
        #baseline instead of diffing across two unrelated counters.
        self._cadenceTrackerKey = None
        self._cadenceLastTicks = None
        self._cadenceLastRevs = None
        self._speedTrackerKey = None
        self._speedLastTicks = None
        self._speedLastRevs = None

        #Cadence fallback bookkeeping (from the power meter's own
        #reported crank cadence - a plain instantaneous value, no
        #rollover math needed).
        self._latestPowerMeterCadenceRpm = None
        self._powerDeviceHealthy = False
        self._powerDeviceIsFixedResistance = False

        #Live power->speed calibration for the speed fallback: a rolling
        #set of (timestamp, watts, mph) samples recorded only from real,
        #simultaneous power+speed sensor readings, used to look up an
        #estimated speed from the current power reading while the real
        #speed sensor is stale/lost/stalled. Only valid for a fixed-
        #resistance power source (see _powerDeviceIsFixedResistance) -
        #an FE-C/controllable trainer can change its own resistance
        #independent of speed, which breaks the whole premise.
        self._speedCalSamples = []

        self.cumulativeSpeedRevolutionsLast = "0"
        self.speedEventTimeLast = "0"
        self.cumulativeCadenceRevolutionsLast = "0"
        self.cadenceEventTimeLast = "0"

        #Saved power meter calibrations (see PowerCalibration.py / Calibrate
        #Power on the setup screen), read once per workout
        self._powerCalibrations = PowerCalibration.load_all()

        self.active = True
        self.bridge.dataUpdated.connect(self._on_data_updated)

    def _on_data_updated(self, role):
        #Connected to AntWorkoutBridge.dataUpdated - fires once per ANT
        #page decode for a role-relevant device (see ClassAntDevices.py's
        #_on_message). `role` isn't dispatched on below: every block here
        #is already self-gated on its own pages_received counter, so
        #running the whole pass on every signal (whichever role actually
        #triggered it) is harmless and exactly mirrors what the old
        #100ms-tick while-loop body did each iteration.
        if not self.active:
            return

        bridge = self.bridge
        lastPowerPages = self.lastPowerPages
        lastHrPages = self.lastHrPages
        lastCadenceSourcePages = self.lastCadenceSourcePages
        lastCadenceFallbackPmPages = self.lastCadenceFallbackPmPages
        lastSpeedSourcePages = self.lastSpeedSourcePages
        lastSpeedFallbackPmPages = self.lastSpeedFallbackPmPages

        if True:  # noqa: preserves the old while-loop body's indentation verbatim below
            #Pull decoded values straight off the live ANTDevice/profile
            #instead of polling files.
            powerDevice = bridge.selected_device(ROLE_POWER)
            pmLatest = None
            if powerDevice is not None and powerDevice.profile is not None:
                pmLatest = powerDevice.profile.latest
                if not hasattr(pmLatest, "instantaneous_power_watts"):
                    #no power reading decoded yet (an FE-C trainer only fills this from its trainer data page)
                    pmLatest = None
                pmPages = powerDevice.profile.readings_received
                if pmLatest is not None and pmPages != lastPowerPages:
                    lastPowerPages = pmPages
                    pmVal = self._calibrated_power(powerDevice, pmLatest)
                    WorkoutData.power.append(pmVal)
                    if pmVal == 0:
                        if self.lastNonZeroPowerIndex < 3:
                            #possible misread, keep the last value for a few cycles
                            pmVal = self.lastNonZeroPower
                            self.lastNonZeroPowerIndex += 1
                    else:
                        #reset the counter
                        self.lastNonZeroPowerIndex = 0
                        self.lastNonZeroPower = pmVal
                    self.powerGauge.ui.widget.setValue(int(pmVal))
                    AppState.savedPowerVals[0].append(AppState.secondsPassed)
                    AppState.savedPowerVals[1].append(int(pmVal))
                    c0 = self.getFtpColor(int(pmVal))
                    self.powerGauge.ui.widget.outer_circle_bg = [[0.25, QColor(0,0,0,0)], [0.18, QColor(c0)],]
                    while AppState.savedPowerVals[0][-1] - AppState.savedPowerVals[0][0] > AppState.graphSecondsShown-AppState.graphBreakpoint:
                        AppState.savedPowerVals[0].pop(0)
                        AppState.savedPowerVals[1].pop(0)

            #Track the power meter's own reported cadence + whether the
            #power source itself looks healthy and is a fixed-resistance
            #device - independent of the hold-last-value smoothing above
            #(that's purely for the watt gauge display). Used by the
            #cadence/speed fallback logic below.
            if powerDevice is not None and pmLatest is not None:
                self._latestPowerMeterCadenceRpm = getattr(
                    pmLatest, "power_meter_cadence_rpm", None
                )
                self._powerDeviceHealthy = (
                    powerDevice.state == "ACTIVE"
                    and not powerDevice.data_stalled
                )
                self._powerDeviceIsFixedResistance = (
                    powerDevice.device_type == DEVICE_TYPE_POWER_METER
                )
            else:
                self._latestPowerMeterCadenceRpm = None
                self._powerDeviceHealthy = False
                self._powerDeviceIsFixedResistance = False

            if AppState.antHRM:
                hrDevice = bridge.selected_device(ROLE_HRM)
                if hrDevice is not None and hrDevice.profile is not None:
                    hrLatest = hrDevice.profile.latest
                    hrPages = hrDevice.profile.readings_received
                    if hrLatest is not None and hrPages != lastHrPages:
                        lastHrPages = hrPages
                        hrmVal = hrLatest.computed_heart_rate
                        WorkoutData.heartRate.append(hrmVal)
                        if hrmVal == 0:
                            if self.lastNonZeroHRIndex < 10:
                                #possible misread, keep the last value for a few cycles
                                hrmVal = self.lastNonZeroHR
                                self.lastNonZeroHRIndex += 1
                        else:
                            #reset the counter
                            self.lastNonZeroHRIndex = 0
                            self.lastNonZeroHR = hrmVal
                        self.hrGauge.ui.widget.setValue(int(hrmVal))
                        AppState.savedHRVals[0].append(AppState.secondsPassed)
                        AppState.savedHRVals[1].append(int(hrmVal))
                        while AppState.savedHRVals[0][-1] - AppState.savedHRVals[0][0] > AppState.graphSecondsShown-AppState.graphBreakpoint:
                            AppState.savedHRVals[0].pop(0)
                            AppState.savedHRVals[1].pop(0)

            if AppState.shows_cadence():
                #--- Cadence: dedicated sensor (combined or discrete)
                #first, falling back to the power meter's own cadence
                #field if the dedicated source has gone stale/lost/
                #stalled (RecoveryManager). ---
                cadenceDevice = None
                if AppState.antCadence:
                    cadenceDevice = bridge.selected_device(ROLE_CADENCE)
                elif AppState.antSpeedCadence:
                    cadenceDevice = bridge.selected_device(ROLE_SPEED_CADENCE)

                dedicatedCadenceHealthy = (
                    cadenceDevice is not None
                    and cadenceDevice.profile is not None
                    and cadenceDevice.state == "ACTIVE"
                    and not cadenceDevice.data_stalled
                )

                if not dedicatedCadenceHealthy:
                    #Reset the rollover baseline so a recovered sensor
                    #doesn't diff its first fresh reading against ticks
                    #from before the outage.
                    self._cadenceTrackerKey = None

                newCadence = None
                cadenceIsFallback = False

                if dedicatedCadenceHealthy:
                    cadLatest = cadenceDevice.profile.latest
                    cadPages = cadenceDevice.profile.pages_received
                    if cadLatest is not None and cadPages != lastCadenceSourcePages:
                        lastCadenceSourcePages = cadPages
                        #raw counters for SynopsisWindow's replay, every new page whether or not
                        #it has a new pedal stroke (so a combined sensor's speed while coasting
                        #is kept too, and no gap long enough for its 64s event time to wrap)
                        if cadenceDevice.device_type == DEVICE_TYPE_SPEED_CADENCE_COMBINED:
                            #both readings together: cumulativeSpeedRevolutions, speedEventTime,
                            #cumulativeCadenceRevolutions, cadenceEventTime (see
                            #SynopsisWindow.process_synopsis_data)
                            WorkoutData.speedCadence.append(
                                cadLatest.speed_revolution_count,
                                cadLatest.speed_event_ticks,
                                cadLatest.cadence_revolution_count,
                                cadLatest.cadence_event_ticks,
                            )
                        else:
                            WorkoutData.cadence.append(cadLatest.revolution_count, cadLatest.event_ticks)
                        newCadence = self._cadence_from_ticks(cadenceDevice, cadLatest)
                elif (
                    self._powerDeviceHealthy
                    and self._latestPowerMeterCadenceRpm is not None
                    and pmLatest is not None
                    and lastCadenceFallbackPmPages != powerDevice.profile.readings_received
                ):
                    lastCadenceFallbackPmPages = powerDevice.profile.readings_received
                    newCadence = float(self._latestPowerMeterCadenceRpm)
                    cadenceIsFallback = True
                    #for SynopsisWindow's replay - there are no raw counters to record here
                    WorkoutData.fallbackCadence.append(self._latestPowerMeterCadenceRpm)

                if newCadence is not None:
                    cadence = newCadence
                    if cadence <= 200:
                        if cadence < 10:
                            if self.lastNonZeroCadenceIndex < 30:
                                #possible misread, keep the last value for a few cycles
                                cadence = self.lastNonZeroCadence
                                self.lastNonZeroCadenceIndex += 1
                        else:
                            #reset the counter
                            self.lastNonZeroCadence = cadence
                            self.lastNonZeroCadenceIndex = 0
                        #the power meter fallback only reports whole RPM, so drop the decimal for it
                        self.cadenceGauge.ui.widget.value_decimals = 0 if cadenceIsFallback else 1
                        self.cadenceGauge.ui.widget.setValue(cadence)
                        AppState.savedCadenceVals[0].append(AppState.secondsPassed)
                        AppState.savedCadenceVals[1].append(int(cadence))
                        while AppState.savedCadenceVals[0][-1] - AppState.savedCadenceVals[0][0] > AppState.graphSecondsShown-AppState.graphBreakpoint:
                            AppState.savedCadenceVals[0].pop(0)
                            AppState.savedCadenceVals[1].pop(0)

                #(Musical Cadence Mode shows cadence, from the power meter, with no speed/cadence source selected)
                if AppState.any_speed_cadence():
                    #--- Speed: dedicated sensor (combined or discrete)
                    #first, falling back to a live-calibrated power->speed
                    #estimate if the dedicated source has gone stale/lost/
                    #stalled. Only offered for a fixed-resistance power
                    #source (a plain power meter, not an FE-C/controllable
                    #trainer, which can change its own resistance
                    #independent of speed) - see _powerDeviceIsFixedResistance. ---
                    speedDevice = None
                    if AppState.antSpeed:
                        speedDevice = bridge.selected_device(ROLE_SPEED)
                    elif AppState.antSpeedCadence:
                        speedDevice = bridge.selected_device(ROLE_SPEED_CADENCE)

                    dedicatedSpeedHealthy = (
                        speedDevice is not None
                        and speedDevice.profile is not None
                        and speedDevice.state == "ACTIVE"
                        and not speedDevice.data_stalled
                    )

                    if not dedicatedSpeedHealthy:
                        self._speedTrackerKey = None

                    newSpeed = None
                    speedIsFallback = False

                    if dedicatedSpeedHealthy:
                        spdLatest = speedDevice.profile.latest
                        spdPages = speedDevice.profile.pages_received
                        if spdLatest is not None and spdPages != lastSpeedSourcePages:
                            lastSpeedSourcePages = spdPages
                            if speedDevice.device_type != DEVICE_TYPE_SPEED_CADENCE_COMBINED:
                                #raw counters for SynopsisWindow's replay, see the cadence equivalent above
                                WorkoutData.speed.append(spdLatest.revolution_count, spdLatest.event_ticks)
                            newSpeed = self._speed_from_ticks(speedDevice, spdLatest)
                            if (
                                newSpeed is not None
                                and pmLatest is not None
                                and self._powerDeviceIsFixedResistance
                            ):
                                #Real, simultaneous power+speed sample -
                                #feed the fallback calibration. Never
                                #recorded from an estimate.
                                self._record_speed_calibration(
                                    self._calibrated_power(powerDevice, pmLatest),
                                    newSpeed,
                                )
                    elif (
                        self._powerDeviceHealthy
                        and self._powerDeviceIsFixedResistance
                        and pmLatest is not None
                        and lastSpeedFallbackPmPages != powerDevice.profile.readings_received
                    ):
                        lastSpeedFallbackPmPages = powerDevice.profile.readings_received
                        estimated = self._estimate_speed_from_power(
                            self._calibrated_power(powerDevice, pmLatest)
                        )
                        if estimated is not None:
                            newSpeed = estimated
                            speedIsFallback = True

                    if newSpeed is not None:
                        speed = newSpeed
                        if speed <= 60:
                            if speed < 5:
                                if self.lastNonZeroSpeedIndex < 3:
                                    #possible misread, keep the last value for a few cycles
                                    speed = self.lastNonZeroSpeed
                                    self.lastNonZeroSpeedIndex += 1
                            else:
                                #reset the counter
                                self.lastNonZeroSpeed = speed
                                self.lastNonZeroSpeedIndex = 0
                            self.parent.set_speed(str(round(speed,1)))
                            if not speedIsFallback:
                                #Distance is accumulated from real wheel
                                #revolutions only (see _speed_from_ticks) - an
                                #estimated speed during a dropout moves the
                                #gauge/label so the ride doesn't look frozen,
                                #but deliberately does NOT add fabricated
                                #distance to the permanent total.
                                distance = float(AppState.wheelCircumference) * AppState.totalRevolutions * 0.000621371 #0.000621371 miles per meter
                                self.parent.set_distance(str(round(distance,2)))

        #Write the (possibly-updated) local trackers back to instance
        #state so the next dataUpdated call picks up where this one left
        #off - see the mirroring reads at the top of this method.
        self.lastPowerPages = lastPowerPages
        self.lastHrPages = lastHrPages
        self.lastCadenceSourcePages = lastCadenceSourcePages
        self.lastCadenceFallbackPmPages = lastCadenceFallbackPmPages
        self.lastSpeedSourcePages = lastSpeedSourcePages
        self.lastSpeedFallbackPmPages = lastSpeedFallbackPmPages

    def _calibrated_power(self, device, latest):
        """The power meter's reading with its saved calibration applied"""
        calibration = self._powerCalibrations.get(f"{device.device_type} {device.device_number}")
        return PowerCalibration.apply(latest.instantaneous_power_watts, calibration)

    def _cadence_from_ticks(self, device, latest):
        """Rollover-safe cadence (RPM) from raw event-tick/revolution
        deltas - works for both the combined sensor's cadence_* fields
        and a discrete cadence sensor's plain fields, keyed by device
        identity so a source change resets the baseline instead of
        diffing across two unrelated counters.
        """
        if device.device_type == DEVICE_TYPE_SPEED_CADENCE_COMBINED:
            ticks = latest.cadence_event_ticks
            revs = latest.cadence_revolution_count
        else:
            ticks = latest.event_ticks
            revs = latest.revolution_count

        #Known odd/garbage value quirk from real hardware testing with
        #the previous C/C++ receiver - only ever validated against the
        #combined sensor's cadence ticks field, so scoped to that case.
        if device.device_type == DEVICE_TYPE_SPEED_CADENCE_COMBINED and ticks in (513, 2305):
            return None

        key = (device.device_type, device.device_number)
        if self._cadenceTrackerKey != key:
            self._cadenceTrackerKey = key
            self._cadenceLastTicks = ticks
            self._cadenceLastRevs = revs
            return None

        dtTicks = (ticks - self._cadenceLastTicks) & 0xFFFF
        dRevs = (revs - self._cadenceLastRevs) & 0xFFFF
        self._cadenceLastTicks = ticks
        self._cadenceLastRevs = revs

        if dtTicks == 0:
            return None

        #cadence revs/minute, 61440 = 1/1024 x 60 seconds/minute
        cadence = (61440 * dRevs) / dtTicks
        if cadence > 200:
            return None
        return cadence

    def _speed_from_ticks(self, device, latest):
        """Rollover-safe speed (MPH) from raw event-tick/revolution
        deltas, same shape as _cadence_from_ticks. Also accumulates
        AppState.totalRevolutions - the one place real wheel distance is
        tracked, deliberately never touched by the fallback estimate.
        """
        if device.device_type == DEVICE_TYPE_SPEED_CADENCE_COMBINED:
            ticks = latest.speed_event_ticks
            revs = latest.speed_revolution_count
        else:
            ticks = latest.event_ticks
            revs = latest.revolution_count

        key = (device.device_type, device.device_number)
        if self._speedTrackerKey != key:
            self._speedTrackerKey = key
            self._speedLastTicks = ticks
            self._speedLastRevs = revs
            return None

        dtTicks = (ticks - self._speedLastTicks) & 0xFFFF
        dRevs = (revs - self._speedLastRevs) & 0xFFFF
        self._speedLastTicks = ticks
        self._speedLastRevs = revs

        if dtTicks == 0:
            return None

        #speed in MPH, 2290.62656 constant is because time events are in 1/1024 seconds units x 2.23694mph=1m/s
        speed = (float(AppState.wheelCircumference) * dRevs * 2290.62656) / dtTicks
        if speed > 60:
            return None

        AppState.totalRevolutions += dRevs
        return speed

    def _record_speed_calibration(self, power_watts, speed_mph):
        """Remembers a real, simultaneous (power, speed) sample for the
        speed fallback in _estimate_speed_from_power. Only called with
        genuine sensor data (see call site) - never with an estimate.
        """
        now = time_module.monotonic()
        self._speedCalSamples.append((now, power_watts, speed_mph))

        cutoff = now - SPEED_CALIBRATION_MAX_AGE_S
        while self._speedCalSamples and self._speedCalSamples[0][0] < cutoff:
            self._speedCalSamples.pop(0)
        while len(self._speedCalSamples) > SPEED_CALIBRATION_MAX_SAMPLES:
            self._speedCalSamples.pop(0)

    def _estimate_speed_from_power(self, power_watts):
        """Estimates speed from the current power reading using a live,
        piecewise-linear lookup built from recent real (power, speed)
        samples - not a fixed physical model, so it adapts to whatever
        this particular trainer's actual resistance curve looks like.

        Only valid on a stationary trainer with a fixed (non-FE-C)
        resistance unit: at a given wheel speed, that kind of unit
        always applies the same resistance, so power ends up being a
        function of wheel speed alone - independent of gear, since the
        rider's gear choice only changes how hard/fast they have to
        pedal to reach that wheel speed, not the resistance felt at the
        wheel itself. An FE-C/controllable trainer breaks this by being
        able to change its own resistance (e.g. ERG mode holding a
        target power regardless of speed) - callers gate on
        _powerDeviceIsFixedResistance before ever calling this.

        Returns None when there isn't enough recent calibration data yet
        (e.g. very early in the ride, or the speed sensor never worked
        this session) rather than guessing from too little.
        """
        if len(self._speedCalSamples) < SPEED_CALIBRATION_MIN_SAMPLES:
            return None

        points = sorted((p, s) for (_, p, s) in self._speedCalSamples)

        if power_watts <= points[0][0]:
            return points[0][1]
        if power_watts >= points[-1][0]:
            return points[-1][1]

        for i in range(1, len(points)):
            p0, s0 = points[i - 1]
            p1, s1 = points[i]
            if p0 <= power_watts <= p1:
                if p1 == p0:
                    return s0
                fraction = (power_watts - p0) / (p1 - p0)
                return s0 + fraction * (s1 - s0)

        return points[-1][1]

    def getFtpColor(self, power):
        #the gauge's ring behind the reading (see FtpColors.py), see-through
        #(power zones even in Musical Cadence Mode, where AppState.ftpRanges are cadence zones)
        return "#44" + FtpColors.gauge_ring(power, FtpColors.power_ranges())

    def stop(self):
        self.active = False
        #workout over (both the END and back-to-setup paths come through here) - don't leave an
        #FE-C trainer holding the last requested power; a disconnect releases it entirely
        bridge = getattr(self, "bridge", None)
        if bridge is not None and bridge.trainer_selected():
            bridge.set_trainer_target_power(0)
        try:
            self.bridge.dataUpdated.disconnect(self._on_data_updated)
        except (TypeError, RuntimeError):
            #Already disconnected, or start() was never called this
            #session - either way there's nothing to tear down.
            pass