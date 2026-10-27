# SimulANT+ automation

Helpers that drive the SimulANT+ window through Windows UI Automation, so the SimulANT+ tests (see `..\README.md`)
can run without anyone clicking through SimulANT+. Windows PowerShell 5.1 or later; nothing to install.

- `start_simulators.ps1` - step 3 of a test run in one go: starts SimulANT+ if needed, adds the scenario's
  simulators and runs each one's `awo_*.py` script. `-Stop` stops the scripts and closes SimulANT+.
- `uia.ps1` - the building blocks, dot-sourced by the above: `Add-Sim`, `Run-SimScript`, `Stop-SimScript`,
  `TurnOff-Sim`, `Select-Sim`, `Get-Events` (SimulANT+'s event log), `Dump-Sim` (every control, for finding a new
  one's automation ID).

They work the same for the C++ app's harness (`..\..\..\AntWorkoutQt\hwtests\simulant_test.py` and `app_test.py`).

A run, from AntWorkout:

    python SimulantTests\run_simulant_test.py trainer        (leave it running; it waits for the simulators)
    SimulantTests\simulant_automation\start_simulators.ps1 trainer
    ... the harness prints its report when the test ends ...

Needs both ANT sticks, as the tests do: the harness takes the first free one and SimulANT+ lists only the other.

Leave the simulators running between runs of the same scenario: the scripts follow each new run by themselves
(the harness's control.txt), and start_simulators.ps1 leaves a running script alone. Don't restart a script after
the harness has started, or close SimulANT+ and start the next run straight away. The device goes quiet mid-test,
or AntWorkout's stick replays messages it buffered from the last run and the harness connects to simulators that
are no longer there; either way the first checks fail. `-Stop` (stops every script and closes SimulANT+) is for
when you're done.
