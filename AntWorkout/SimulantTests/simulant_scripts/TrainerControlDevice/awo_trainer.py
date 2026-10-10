# AntWorkout hardware test: FE-C controllable trainer, following the test timeline (see SimulantTests\README.md).
# Run it from SimulANT+'s Script panel on a Bike Trainer (controllable FE) simulator.
#
# Behaves like a trainer in ERG mode: once given a target power (page 0x31) it reports riding at that power;
# after a basic resistance command (page 0x30, how AntWorkout releases it) it reports the "free" power instead.
# Every command received is logged for the harness to check.
import awo_simulant
reload(awo_simulant)

held = {"target": None}


def apply(simulator, values):
    simulator.SpecificTrainerPageEnabled = True  # (instantaneous power/cadence ride in the trainer data page)
    simulator.Cadence = values["cadence"]
    simulator.Speed = values["speed"]
    target = held["target"]
    simulator.InstantaneousPower = target if target is not None else values["free"]


def on_page(page):
    try:
        number = page.DataPageNumber
        if number == 0x31:
            watts = page.TargetPower / 4.0
            held["target"] = int(round(watts))
            runner.script.log("rx_target", "%.2f" % watts)
        elif number == 0x30:
            held["target"] = None
            runner.script.log("rx_resistance", "%.1f" % (page.TotalResistance / 2.0))
    except Exception as e:
        runner.script.log("error", "on_page " + repr(e))


runner = awo_simulant.Runner(simulator, "fe", apply)
simulator.DataPageReceived += on_page


def stopScript():
    simulator.DataPageReceived -= on_page
    runner.stop()


runner.start()
