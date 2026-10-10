# AntWorkout hardware test: combined Bike Speed & Cadence sensor, following the test timeline (see
# SimulantTests\README.md). Run it from SimulANT+'s Script panel on a Bike Speed & Cadence Sensor simulator.
import awo_simulant
reload(awo_simulant)


def apply(simulator, values):
    simulator.WheelCircumference = awo_simulant.awo_timeline.WHEEL_MM
    simulator.Cadence = values["cadence"]
    simulator.Speed = values["speed"]


runner = awo_simulant.Runner(simulator, "sc", apply)


def stopScript():
    runner.stop()


runner.start()
