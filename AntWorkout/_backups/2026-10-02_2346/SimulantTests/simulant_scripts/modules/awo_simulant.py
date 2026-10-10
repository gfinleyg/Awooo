# AntWorkout hardware test: the loop each SimulANT+ sensor script runs (IronPython 2.7, inside SimulANT+).
# Four times a second: what the timeline (awo_timeline.py) says this device sends now - switching the simulator
# off for a dropout and back on after - and a log of it in the run's folder for the harness to check against.

import awo_timeline
reload(awo_timeline)  # (SimulANT+ keeps modules loaded between script runs)

from System.Timers import Timer


class Runner(object):
    def __init__(self, simulator, device, apply):
        """apply(simulator, values) sets the simulator's transmitted values"""
        self.simulator = simulator
        self.apply = apply
        self.script = awo_timeline.Script(device)
        self.on = None
        self.timer = Timer(250)
        self.timer.AutoReset = False  # (restarted after each tick, so ticks never overlap)
        self.timer.Elapsed += self.tick
        self.stopped = False

    def start(self):
        self.script.log("script", "started")
        self.tick(None, None)

    def stop(self):
        self.stopped = True
        self.timer.Stop()
        self.simulator.TurnOff()
        self.script.log("script", "stopped")

    def tick(self, sender, args):
        try:
            phase, values = self.script.now()
            if values is None:
                if self.on is not False:
                    self.simulator.TurnOff()
                    self.on = False
                    self.script.log("off", phase)
            else:
                self.apply(self.simulator, values)
                if self.on is not True:
                    self.simulator.TurnOn()
                    self.on = True
                    self.script.log("on", phase)
                self.script.state(phase, values)
        except Exception as e:
            self.script.log("error", repr(e))
        if not self.stopped:
            self.timer.Start()
