# Hardware tests with SimulANT+

Runs AntWorkout's real ANT bridge, device poller and workout screen (Free Mode) against sensors simulated by
SimulANT+ over the air, then checks what the workout screen showed against what SimulANT+ sent.

Needs both ANT sticks: AntWorkout uses one, SimulANT+ the other.

## Running a test

1. Close AntWorkout and SimulANT+.
2. Start the harness (it takes the first free stick and waits up to 10 minutes for the devices):

       python SimulantTests\run_simulant_test.py sensors
       python SimulantTests\run_simulant_test.py trainer

   It copies the SimulANT+ scripts into `Documents\SimulANT+\Scripts` each time it starts.
3. Start SimulANT+. Only the stick AntWorkout isn't using is listed. For each simulator the harness prints:
   **Add Simulator** on that device, pick the profile, select the new simulator, pick its `awo_...py` script in
   the Script panel's list and click **Run**. (No need to press TurnOn - the script does.)

   | scenario  | simulators                                                                   |
   |-----------|------------------------------------------------------------------------------|
   | `sensors` | Bike Power Sensor (power only), Bike Speed & Cadence Sensor, Heart Rate Sensor |
   | `trainer` | Bike Trainer (controllable FE), Heart Rate Sensor                            |

4. Once all of them are heard, the harness connects, opens the workout screen and the timed test starts by
   itself (about 5 minutes; the scripts follow it however far apart they were started). At the end it prints
   the report; stop the scripts in SimulANT+ then.

## What's tested

`sensors`: readings shown correctly (power, cadence, speed, heart rate); stopping pedaling (everything goes to
0 and stays there); the speed/cadence sensor dropping out while riding (cadence from the power meter, estimated
speed, no distance added) and coming back; the heart rate monitor and the power meter each dropping out and
coming back.

`trainer`: the target power commands reach the trainer (and are re-sent while held); release (basic
resistance 0%); the trainer dropping out and getting its target again when it's back; target 0 when the
workout stops.

## Files

- `simulant_scripts/` - the SimulANT+ scripts (IronPython 2.7); `modules/awo_timeline.py` is the timeline both
  sides follow.
- `run/` - one folder per run: `report.txt`, `app_samples.csv` (what the workout screen showed every 100ms),
  `simulant_*.csv` (what each script sent/received) and the app's own `antWorkoutLog.txt`.
