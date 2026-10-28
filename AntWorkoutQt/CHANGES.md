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

## Screens

- **Text that was shrunk to fit grows back when shorter text replaces it**, in fixed size labels too. The Python app
  measured a label against Qt's size hint, which is never smaller than the label's own minimum size, so a fixed size
  label looked exactly full and its shrunk text stayed small until the next full refit (a resize or a theme change).

- **The block and workout timer bars' fill sits inside a thin frame** of the bar's own color. The fill used to run to
  the bar's edges, where in the light themes (and with no theme) it was about the shade of the window behind it, so
  the filled part seemed to vanish into the background. Both bars and the "Block time remaining" box are rounded now,
  like the workout timer boxes.

- **The device popups' title bars show their names**: "Device Selection" and "Connect Devices". The Python app's
  title bars took the window's title before it was set, so both said "CustomTitleBar".

## Musical Cadence Mode

- **Less of a song with a steady beat becomes free ride.** A stretch whose beat only just stands out (a triplet
  feel, an odd meter or a busy mix spreads the analysis' confidence over several tempos) still gets a cadence when its
  tempo holds steady: when it agrees with at least two of the stretches 8 to 12 seconds either side of it. In six
  surf bands' songs, free ride went from 4.8% of the music to 1.4%, and songs more than a tenth free ride from 90 to 33.
  It's worked out from the cached analysis, so nothing is analyzed again, and the Python app reads the same cache.
- **Sections of a song 1 or 2 RPM apart are ridden as one**, at their average cadence over their time (the closest
  ones joined first, so a slow drift doesn't all become one; free ride between them keeps them apart). Tempos far
  enough apart to be separate sections can still come out that close as cadences, mostly when ridden at half the
  beat (112 and 108 BPM as 56 and 54 RPM). In the same six bands' songs, 51 songs lost a block (1650 to 1558).
- **Pick Random Songs picks the unchecked songs first.** The songs checked when it's pressed are picked only once
  none of the others fits, so pressing it again gives a new set of songs (as far as the list has them).

## LayoutViewer

- **The workout screen stops exactly where the Workout progress slider says.** The Python viewer's simulated ride
  counted each second as a second plus however long the computer took over it, so it ended up a little past the
  point picked (up to a second or so in a long workout), and a different amount each time.
- **The app's picked devices are put back when LayoutViewer closes.** Its views are built with stand-in devices
  picked, which the Python viewer left in the app's selected devices file, so the app looked for devices that weren't
  there the next time it started.

## Settings and log

- **savedSettingsANT.json is written with its keys sorted and 4-space indents** (Qt's JSON writer). The values are the
  same, and either app reads the other's file.
- **Crashes are logged**, with a minidump (`AntWorkout_crash.dmp`) beside the log, and so are uncaught C++ exceptions,
  in place of the Python tracebacks.

## Sensors

- **A battery whose voltage reads low is warned about before the device says so.** Devices go on reporting "Ok" at
  2.7V and below on a 3V coin cell, so under 2.75V (cleared again at 2.85V) the setup, workout and synopsis screens
  show "... low battery voltage (2.70V)", and the log line says "- low battery voltage". Once the device itself says
  low or critical, its own warning is the one shown. Batteries under 2V (single 1.5V cells) are judged only by what
  the device says.
- **Stopping pedaling no longer flags the speed/cadence sensor as stalled.** A stopped sensor repeats its last
  message just as a stuck one does, so it's only flagged while a power meter or trainer shows the rider pedaling.
  The Python app (whose AntNative build predates this) flagged it about 6 seconds into every stop, logging
  "DEVICE_DATA_STALLED ... check battery/contact".
- **Workouts are logged**: when one starts (its name, or free ride), completes, is restarted or ended (to the
  synopsis, or back to setup with its data cleared), and going back to setup, which disconnects the devices.
