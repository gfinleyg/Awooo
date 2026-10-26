# Changes from the Python app

Everything the C++ app does differently from `..\AntWorkout` that a rider (or someone using the tools) could notice,
and why. Anything not listed here is meant to behave exactly as before; the tests in `tests\` check it against the
Python app's own results (`tests\golden\`).

## Workout files

- **awo-verifier reports every bad line in the app's words.** It uses the app's own reader now instead of its own
  copy of the rules, so the two can't disagree. It takes files and folders on the command line (the current folder
  by default, as before), and exits with 1 when a file isn't valid, so it can be used in scripts.
- **Interval repeats are capped at 1000** ("Interval repeats can be at most 1000"). The Python app took any number,
  and a huge one never finished loading.
- **A workout file that can't be opened is reported** ("Can't open the file (...)"). The Python app raised an error
  instead.

## Zwift files

- **A Duration, power or other number of "inf" or "nan" is reported** as "isn't a number". The Python app accepted it
  and then failed while converting.
- **The wording after "Not a readable workout file"** comes from Qt's XML reader rather than Python's, e.g.
  "Start tag expected.: line 1, column 1" instead of "syntax error: line 1, column 0".
- **zwo2awo reports an .awo file it couldn't write** instead of raising an error.

## Times shown

- **Durations of a day or more keep counting hours** ("25h"). The Python app's went back to 0h after 24.

## Settings and log

- **savedSettingsANT.json is written with its keys sorted and 4-space indents** (Qt's JSON writer). The values are the
  same, and either app reads the other's file.
- **Crashes are logged**, with a minidump (`AntWorkout_crash.dmp`) beside the log, and so are uncaught C++ exceptions,
  in place of the Python tracebacks.
