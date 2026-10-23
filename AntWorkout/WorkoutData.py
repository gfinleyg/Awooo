"""
WorkoutData.py

The workout's full sensor history, recorded by AntDevicesPoller as each
reading arrives and replayed by SynopsisWindow.process_synopsis_data() once
the workout ends (its own noise filtering, best-power-range stats, etc.).
Replaces the old pmANT.txt/hrmANT.txt/scANT.txt files.

Kept in compact arrays rather than lists of Python objects: 8 bytes per
timestamp plus 4 per value, so roughly 0.6 MB per hour with power, heart
rate and a combined speed/cadence sensor (or a discrete cadence + speed
pair) all reporting at ~4 Hz.

Timestamps are time.time() seconds, the same clock as
AppState.startTimestamp.timestamp().
"""

import time
from array import array


class SensorHistory:
    def __init__(self, columns=1):
        self.times = array("d")
        self.columns = tuple(array("l") for _ in range(columns))

    def append(self, *values):
        self.times.append(time.time())
        for column, value in zip(self.columns, values):
            column.append(int(value))

    def rows(self):
        """(timestamp, value, ...) per reading, oldest first"""
        return zip(self.times, *self.columns)

    def copy(self):
        """Another history with the same readings (the session keeps each ride's, see Session.py)"""
        history = SensorHistory(len(self.columns))
        history.times.extend(self.times)
        for mine, theirs in zip(history.columns, self.columns):
            mine.extend(theirs)
        return history

    def clear(self):
        del self.times[:]
        for column in self.columns:
            del column[:]

    def __len__(self):
        return len(self.times)


#calibrated watts
power = SensorHistory()
#BPM
heartRate = SensorHistory()
#combined (121) sensor raw counters: cumulative speed revolutions, speed event time,
#cumulative cadence revolutions, cadence event time (event times in 1/1024 s)
speedCadence = SensorHistory(4)
#discrete cadence (122) / speed (123) sensor raw counters: cumulative revolutions, event time (1/1024 s)
cadence = SensorHistory(2)
speed = SensorHistory(2)
#RPM from the power meter's own cadence field, whenever the live cadence gauge fell back to it
#(no dedicated cadence sensor, or the dedicated one went stale/lost/stalled)
fallbackCadence = SensorHistory()


def clear():
    power.clear()
    heartRate.clear()
    speedCadence.clear()
    cadence.clear()
    speed.clear()
    fallbackCadence.clear()
