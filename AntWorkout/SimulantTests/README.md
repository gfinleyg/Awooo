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

   Or let `simulant_automation\start_simulators.ps1 <scenario>` do all of step 3 (see that folder's README).

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

## The whole app

`run_app_test.py` drives the real app (main window, setup screen, workout screen, synopsis, exit) the way a rider
would, against the same simulators, which send their steady pairing values the whole time (no timeline). Run it
with the same simulators running as the test above, and only those (a trainer heard alongside a power meter makes
the power device a choice on the setup screen):

    python SimulantTests\run_app_test.py trainer
    python SimulantTests\run_app_test.py sensors

`trainer` (about 3 minutes): Workout Mode on the trainer - ERG through steady blocks, a ramp and intervals, a block
over the Power Drive Cutoff (released), a difficulty change and a pause - then grading, the synopsis, Esc for the
End Workout prompt, back to setup and the app's own exit.

`sensors` (about 4 minutes): Musical Cadence Mode with two generated songs analyzed and played (silently), a
shortened 20m FTP test and Apply FTP, a Free Mode ride, going back to setup each time and the app's own exit.

Its run folder holds `report.txt`, the scripts' logs and the app's own files (settings, log).

## Files

- `simulant_scripts/` - the SimulANT+ scripts (IronPython 2.7); `modules/awo_timeline.py` is the timeline both
  sides follow.
- `run/` - one folder per run: `report.txt`, `app_samples.csv` (what the workout screen showed every 100ms),
  `simulant_*.csv` (what each script sent/received) and the app's own `antWorkoutLog.txt`.
