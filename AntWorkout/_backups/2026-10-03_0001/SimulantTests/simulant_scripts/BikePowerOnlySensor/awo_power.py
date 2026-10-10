# AntWorkout hardware test: Bike Power (power only) sensor, following the test timeline (see
# SimulantTests\README.md). Run it from SimulANT+'s Script panel on a Bike Power Only Sensor simulator.
import awo_simulant
reload(awo_simulant)


def apply(simulator, values):
    simulator.InstantaneousPower = values["power"]
    simulator.Cadence = values["cadence"]


runner = awo_simulant.Runner(simulator, "pm", apply)


def stopScript():
    runner.stop()


runner.start()
