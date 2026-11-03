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

- **The block grades take over the left column 30 s into a last block longer than that**, when "Up Next:" and its
  "last block" note go, instead of leaving the column empty until the workout ends. What's still being ridden shows a
  blinking `?` for its grade, filled in when it's set: a last block's at the end, each repeat of a last interval's as
  it ends (each interval block's own grade once all its repeats have theirs), a Musical Cadence Mode song's at the
  end. The workout average at the bottom, its grade and its percentage, is a blinking `?` too until the end of the
  workout. The list opens scrolled to its end, and the
  zone legend goes with the rest of the column. FTP tests aren't graded, so they're the same as before.

- **Text that was shrunk to fit grows back when shorter text replaces it**, in fixed size labels too. The Python app
  measured a label against Qt's size hint, which is never smaller than the label's own minimum size, so a fixed size
  label looked exactly full and its shrunk text stayed small until the next full refit (a resize or a theme change).

- **The block and workout timer bars' fill sits inside a thin frame** of the bar's own color. The fill used to run to
  the bar's edges, where in the light themes (and with no theme) it was about the shade of the window behind it, so
  the filled part seemed to vanish into the background. Both bars and the "Block time remaining" box are rounded now,
  like the workout timer boxes.

- **The workout screen's zone legend shows both units, whatever the mode:** where each zone ends in RPM above its bar
  (the cadence zones, 40/65/90/115 rpm) and in watts below it (the FTP's power zones). The Python app's legend shows
  one row under the bar, in the mode's own unit (watts, or rpm in Musical Cadence Mode). The legend is taller for the
  second row, so the coming up list above it shows one block fewer on some screens. The `zone_legend` pixel comparison
  scene no longer matches the Python app's.

- **The workout screen's "Up Next:", "Coming up..." and "Workout Grades" headers, the setup screen's "Required Power
  Device:" and "Optional HR/Cadence/Speed Device(s):" headers and the zone legend's numbers get a one pixel outline
  where their text doesn't stand out** from the window's gradient behind them (contrast under WCAG AA, 4.5). The outline is the window's color behind the text, lightened toward the theme's lightest
  background color only as far as it needs to go to be seen against the text (3:1). In the Python app these headers could
  nearly vanish in themes whose text color is close to the top of the window's gradient, and the legend's numbers had a
  wide, bright halo (each number's outline here takes the gradient behind that number). The `zone_legend` and
  `setup_screen` pixel comparison scenes no longer match the Python app's where the outline shows.

- **The gauges' readings get the same one pixel outline where they don't stand out** from the center behind them, as
  when dark zone colors or a dark heart rate gradient make the rings dark. The reading stays black, and its outline is
  the rings' color taken toward white only as far as it needs to go (white rather than the theme's lightest background,
  which in a dark theme wouldn't show against black). The gauge pixel comparison scenes no longer match the Python
  app's where the outline shows.

- **The spiral's cadence lines stay where the cadence is between pedal strokes.** A cadence sensor only sends a new
  reading with a pedal stroke, and the Python app's spiral took more than 2 s without one (while the power kept
  coming) as the sensor gone quiet, so at a slow cadence or after a missed message its line jumped to the lane's outer
  edge, as if at the top cadence. The last reading is kept for up to 4 s now; past that it's still the outer edge, and
  with no power (coasting) it's the inner edge. The synopsis' rings rebuilt from a ride's readings do the same.

- **The heart rate gauge has no heart behind it any more; its center hearts are as big as the other gauges' center
  rings and colored by the heart rate.** They're light pink up to 90 BPM, then blend through red to dark red at 95%
  of the max heart rate set (no max set: of 200 BPM), the middle hearts paler so the reading over them stays easy to
  read. They still pulse with each beat. The reading sits in the middle as big as fits in the heart (raised a little
  into its widest part) with "BPM" below, like the power and cadence gauges. The three colors can be changed in a
  theme's colors (`#ffb3c6`, `#e8102a`, `#800014`), and LayoutViewer lists them. The Python app's resting/beating
  heart image and its small red-to-pink hearts are gone, so the `hr_gauge` parity scene is meant to differ.

- **The power and RPM gauges' target arc runs from the edge of the center rings to the edge of the scale.** The Python
  app's started inside the rings, over their outer part, and ran past the gauge's edge, so it was cut off flat against
  the block info above when it pointed up. It's now drawn under the center, and on the RPM gauge it starts just under
  the gear, filling the gaps between the teeth. Scenes with an arc (`power_gauge`, `cadence_mode_gauge`, the workout and
  Musical Cadence screens', `ftp_colors_tab`, `ftp_mockup`) are meant to differ from the Python app's.

- **On the RPM gauge's beat the gear stays as it is, and the circles inside it pulse harder.** The Python app's gear
  swelled and brightened with the circles. Now only the circles do: they lose all their paleness instead of 60% of it,
  and each grows outward by the same amount, up to a pixel short of the floor of the gaps between the gear's teeth, at
  any size. The target arc stays still with the gear. The `cadence_mode_gauge` scene, and the Musical Cadence, workout
  and free ride screens' scenes that show the rings, are meant to differ.

- **The RPM gauge has a band of the cadence zones' colors in every mode,** filled up to the needle like the power
  gauge's (the chainring shrunk to sit inside it): the same zones as Musical Cadence Mode's (40/65/90/115 rpm) and the
  same fades, from the theme's zone colors. The Python app's RPM gauge had no band outside Musical Cadence Mode, so the
  `cadence_gauge` parity scene, and the workout and free ride screens' scenes, are meant to differ.

- **The workout screen's graph shows a completed block's stats in a bubble**: its average power and cadence
  ("Avg Pwr 245W", "Avg Cad 88rpm"; no cadence line without a cadence sensor, "--" for one with no readings in the
  block). One shows at a time: the latest block still on the graph with the same target as the block being ridden,
  or else the latest block. In the last 10 s of a block at least 10 s long, if a block on the graph has the next
  block's starting target (a free ride has none), it moves there ahead of time. It sits in the middle of its block with
  its bottom edge on the graph's bottom, and scrolls along with its block. Once scrolling would take it off the graph
  it stays at the left edge, moving right inside a long block, and pops as soon as it no longer fits inside the block
  there. One wider than its block stays in its block's middle, and pops when its left edge reaches the graph's. It's a
  soap bubble like the max bubbles (below), and pops when another block's bubble takes its place. The Python app has
  no block bubbles, so the workout and Musical Cadence Mode screens' parity scenes (`hud_screen`, `cadence_screen`)
  are meant to differ.

- **The workout graph's bubbles are soap bubbles**: the max bubbles beside the stars have round ends, and both they
  and the block bubble are a see-through film in the plot labels' background color with faint rainbow swirls
  drifting across it, stronger toward a thin iridescent rim, and a glint or two of light. When one pops, its film
  tears open from a point on its edge and throws off a spray of colored droplets that fall away and fade (0.45 s). The
  Python app's max bubbles are plain boxes with a black border that swell and fade when they pop. The film's colors
  (`#ff6ec7`, `#ffd36e`, `#7dffb0`, `#6ecbff`, `#b48cff`) and its glints' white (`#fffffd`) can be changed in a
  theme's colors, and LayoutViewer lists them. The `hud_graph` parity scene (and the workout screens' scenes) are
  meant to differ.

- **The power, heart rate, cadence and speed lines are antialiased, and don't flicker as the workout graph scrolls.**
  The Python app's lines (like pyqtgraph's by default) aren't antialiased, so as the graph scrolled by a fraction of
  a pixel at a time, each stretch of a line snapped to whole pixels at a different moment and its width and halo
  flickered. Each line is now drawn once into an image that's shifted by whole pixels as the graph scrolls, with only
  the newest part drawn again as readings come in. The line moves a whole pixel at a time (about every half second)
  and can be up to half a pixel from the bars behind it. The graph takes far less CPU to draw, except for the one
  frame after a change of scale, such as when a new highest reading rescales the graph. The synopsis' lines are
  antialiased the same way. The `hud_graph`, `synopsis_screen` and workout screens' parity scenes no longer match the
  Python app's along the lines.

- **The workout graph has no empty strips at its sides**: the power, heart rate and cadence lines start at its left
  edge (to within a reading: up to about a second, or 3 pixels) and the expected power runs to its right edge. The
  Python app's graph pads its 12 minutes by about 2% on each side (about 40 pixels at 1800 wide). The `hud_graph`
  parity scene (and the workout screens' scenes) are meant to differ.

- **The workout graph's stars and bubbles leave at its left edge, not after it**: once the graph scrolls, a star moves
  to the next highest reading as soon as its left edge reaches the graph's (the Python app's waits for the max itself
  to scroll off), and its max bubble pops with it. A max bubble on its star's left pops when its own left edge gets
  there, and a new one comes in on the star's right. Before the graph scrolls, nothing's moving toward the edge, so
  stars can sit against it.

- **The workout graph's bubbles never touch a star or each other**: one that a star (sliding along its line or
  scrolling into it) or another bubble touches pops, and a new one comes in where it's clear of them all, and of where
  a sliding star is still to go. A max bubble goes beside its star if it can, else over, under or to a corner of it. A
  block bubble goes in the middle of its block on the graph's bottom if it can, else along the bottom of its block,
  else higher up in it; one wider than its block only in its middle. With no clear spot it stays away until there's
  one. Max bubbles used to change sides without popping. The `hud_graph` parity scene (and the workout screens' scenes)
  are meant to differ.

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

- **Only one copy of the app runs at a time.** Starting it again brings up the window that's already open. Starting it
  while the last copy is still closing (its window is gone but it still has the stick for a moment) waits for that copy
  to finish, so a quick close and reopen gets the stick. The Python app started a second copy that couldn't open the
  stick. If the ANT shutdown gets stuck, the app now really exits after its 3 seconds, where it used to stay running
  unseen, with the stick.
- **A stick in use by another program connects by itself once it's free.** When the stick is there but won't open
  (usually because AntWorkout was started twice), the setup screen says it's in use, and the app keeps quietly trying
  it. The Python app only ever looked for a newly plugged in stick, so the stick had to be unplugged and plugged back
  in, even after the other program had closed. The log's "New ANT USB stick detected" is now "ANT USB stick available".
- **The stick is reset when the app closes, and packets from before a launch are thrown away.** Closing used to leave
  the stick scanning with nobody listening. The next launch read that backlog (and an `EVENT_SERIAL_QUE_OVERFLOW`) as
  if it was live, so sensors that had long since gone to sleep showed as found, got auto-selected and connected to, and
  were then reported lost, which could set off a stick reset. Devices now only count as found when they're heard after
  the stick's reset at startup.
- **A sensor's pairing bit isn't read as part of its device type.** A sensor that set it while waking up was logged as
  a new device (a speed/cadence sensor as type 249 rather than 121), and its packets went to that device until the bit
  cleared. This is in AntNative, so the Python app gets it too if it's given a new AntNative build.
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
