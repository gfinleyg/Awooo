# AntWorkout hardware test: Heart Rate sensor, following the test timeline (see SimulantTests\README.md).
# Run it from SimulANT+'s Script panel on a Heart Rate Sensor simulator.
import awo_simulant
reload(awo_simulant)


def apply(simulator, values):
    simulator.HeartRate = values["hr"]


runner = awo_simulant.Runner(simulator, "hr", apply)


def stopScript():
    runner.stop()


runner.start()
