# AntWorkout

An indoor cycling workout app for Windows. It talks directly to ANT+ sensors and smart trainers through an ANT USB
stick, and shows a live HUD with power, cadence, speed and heart rate while you ride.

## Features

- **Workout Mode**: ride structured workouts from `.awo` files (or Zwift `.zwo` files, which are converted on the
  fly). Smart trainers get their target power set automatically (ERG).
- **Free Mode**: ride with no structure and just watch the numbers.
- **FTP tests**: 20 minute, 1 hour, and ramp tests (10W or 20W steps). The result can be applied as your new FTP.
- **Musical Cadence Mode**: pick songs (a folder, files, or an `.m3u`/`.m3u8` playlist). Each song's tempo is
  analyzed and turned into target cadences (RPM), so you pedal to the beat. Analysis results are cached, so each song
  is only analyzed once.
- **Synopsis**: a post-ride summary with graphs and grading.
- **Themes**: the settings menu's Pick Theme opens a full screen picker: a theme's colors (`themes/colors`) and
  power/cadence gradients (`themes/gradients`), each tried on as it's picked, beside a preview of the workout screen
  in that look. Theme files (fonts too) can be made with `LayoutViewer.py`.

Supported ANT+ devices: heart rate monitors, speed/cadence sensors, power meters, and FE-C smart trainers.

## Requirements

- 64-bit Windows
- Python 3.10 to 3.14
- An ANT USB stick (ANT+ dongle)

Install the Python dependencies:

```
py -3.x -m pip install -r requirements.txt
```

The ANT stick module (`ant_native.pyd`) comes prebuilt for each supported Python version in
`AntNative\build\Release\`. You only need `requirements-build.txt` if you want to rebuild it or the exe.

## Running

```
python AntWorkout.py
```

Or double-click `AntWorkout.pyw` to run it without a console window. Output and crash tracebacks then go to
`AntWorkout_console.log`.

To run it as an exe instead, build `AntWorkout.exe` into `dist\AntWorkout\` (see Building the exe below).

## Workout files (.awo)

Put workouts in `awoWorkouts\`. Each line of an `.awo` file is one block. Power is a multiple of FTP (0.05 to 3)
or explicit watts like `250W`.

| Line                                       | Block                                    |
|--------------------------------------------|------------------------------------------|
| `T:<title>`                                | Workout title (optional, only one)        |
| `0:<seconds>,<position>`                   | Free ride                                |
| `1:<seconds>,<power>,<position>`           | Steady state                             |
| `2:<seconds>,<start power>,<end power>,<position>` | Ramp                             |
| `3:<repeats>;<block>;<block>;<block>`      | Intervals (1 to 3 blocks, repeated)       |

Position is optional: 0 seated (default), 1 seated high cadence, 2 seated low cadence, 3 standing, 4 single leg,
5 switch single leg. Blocks can be at most 1800 seconds. Any block in an interval can be a free ride, steady or ramp.

A ramp with the same start and end power is ridden as a steady block. An interval of a single block is ridden as one
longer block, so it has to be a free ride or steady block, and 1800 seconds or less in all.

A block can show a message by adding `|<message>|<seconds shown>|<start second in block>` to the end of it.

Example:

```
T:Short Intervals
2:300,0.5,0.75
3:4;1:30,1.2,3;1:60,0.6
1:300,0.5
```

`awo-verifier.py` checks every `.awo` file in the folder it is run from (for the workouts folder: `cd awoWorkouts`, then
`python ..\awo-verifier.py`). `zwo2awo.py` converts the Zwift files there.

## Other tools

| Script                         | What it does                                                            |
|--------------------------------|-------------------------------------------------------------------------|
| `LayoutViewer.py`              | Preview every screen without an ANT stick and edit fonts/colors/themes   |
| `FtpColorsEditor.py`           | Edit the power zone colors                                              |
| `ColorPicker.py`               | Color picking helper                                                    |
| `awo-verifier.py`              | Validate the `.awo` files in the current folder                         |
| `zwo2awo.py`                   | Convert Zwift `.zwo` workouts in the current folder to `.awo`           |
| `SimulantTests/`               | Hardware tests against SimulANT+ simulated sensors (see its README)     |
| `tests/`                       | Unit tests, no ANT stick needed (see Running the tests)                 |

## Running the tests

```
py -3.x -m pip install -r requirements.txt -r requirements-test.txt
py -3.x -m pytest tests
```

Run from this folder. No ANT stick or SimulANT+ needed: the ANT layer runs on a fake stick (`tests\fakes.py`) and
the screens are built offscreen. Each test runs in its own empty folder, so your settings, log and caches aren't
touched. It takes about 35 seconds; `-m "not whole_app"` skips the slower tests that build the real screens.

The SimulANT+ tests in `SimulantTests\` are still the ones that check the radio side (real channels, timing and
dropouts over the air).

## Building the exe

```
py -3.10 -m pip install -r requirements.txt -r requirements-build.txt
py -3.10 -m PyInstaller --noconfirm AntWorkout.spec
```

This builds `AntWorkout.exe` and `LayoutViewer.exe` into `dist\AntWorkout\`.

## Saved files

These are written next to the app:

- `savedSettingsANT.json`: settings (FTP, max heart rate, wheel size, power calibration, theme, etc.)
- `musicCadenceCacheANT.json`: cached song tempo analysis
- `musicSelectionANT.json`: last music selection
- `antWorkoutLog.txt`: event and error log
