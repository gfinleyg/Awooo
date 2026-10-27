# AntWorkout

An indoor cycling workout app for Windows. It talks directly to ANT+ sensors and smart trainers through an ANT USB
stick, and shows a live HUD with power, cadence, speed and heart rate while you ride.

The app is the C++/Qt one in `AntWorkoutQt\`, released from version 1.4.0 on. `AntWorkout\` is the original Python
app (releases 1.0.0 to 1.3.0), kept as the reference: the C++ app's tests check it against the Python app's own
results, and it still runs (see its README).

## Features

- **Workout Mode**: ride structured workouts from `.awo` files (or Zwift `.zwo` files, which are converted on the
  fly). Smart trainers get their target power set automatically (ERG).
- **Free Mode**: ride with no structure and just watch the numbers.
- **FTP tests**: 20 minute, 1 hour, and ramp tests (10W or 20W steps). The result can be applied as your new FTP.
- **Musical Cadence Mode**: pick songs (a folder, files, or an `.m3u`/`.m3u8` playlist). Each song's tempo is
  analyzed and turned into target cadences (RPM), so you pedal to the beat. Analysis results are cached, so each song
  is only analyzed once.
- **Synopsis**: a post-ride summary with graphs and grading.
- **Sessions**: every ride since the app opened is kept (in memory only) and shown together on the synopsis, one
  after the other with a heading and divider for each, and the workout spiral's outer ring carries on from them. Each
  ride keeps the FTP zones it was ridden at. **Clear Session** on the setup screen starts a new session.
- **Themes**: the settings menu's Pick Theme opens a full screen picker: a theme's colors (`themes\colors`) and
  power/cadence gradients (`themes\gradients`), each tried on as it's picked, beside a preview of the workout screen
  in that look. Theme files (fonts too) can be made with `LayoutViewer.exe`.

Supported ANT+ devices: heart rate monitors, speed/cadence sensors, power meters, and FE-C smart trainers.

## Requirements

- 64-bit Windows 10 or later
- An ANT USB stick (ANT+ dongle)

## Installing

Download `AntWorkout-vX.Y.Z.zip` from the [releases page](https://github.com/gfinleyg/Awooo/releases), decompress it
into your Documents folder and run `AntWorkout\AntWorkout.exe`. Nothing else needs installing: Qt, the C++ runtime
and the app's fonts, sounds, themes and workouts are all in the folder.

Moving up from an earlier version: copy your settings and caches (see Saved files below), and any workouts or themes
of your own, from the old `AntWorkout` folder into the new one. 1.4.0 reads 1.3.0's files as they are.

## Building

`AntWorkoutQt\build_windows.ps1` builds everything into `AntWorkoutQt\dist\AntWorkout\` and runs the tests (it needs
Visual Studio 2022, Qt 6.7.0 and the ANT SDK; see `AntWorkoutQt\README.md`, which also says how a release is made).

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

`awo-verifier.exe` checks `.awo` files (the ones named, or every one in a folder: by default the current one), and
`zwo2awo.exe` converts Zwift files the same way.

## Other tools

Beside `AntWorkout.exe`:

| Tool                  | What it does                                                                    |
|-----------------------|---------------------------------------------------------------------------------|
| `LayoutViewer.exe`    | Preview every screen without an ANT stick and edit fonts/colors/themes            |
| `FtpColorsEditor.exe` | Edit the power zone colors                                                       |
| `ColorPicker.exe`     | Color picking helper                                                             |
| `awo-verifier.exe`    | Validate `.awo` files                                                            |
| `zwo2awo.exe`         | Convert Zwift `.zwo` workouts to `.awo`                                          |
| `ant_console.exe`     | Print the readings of the ANT+ sensors it's told to listen for                   |

The tests (`AntWorkoutQt\tests`, no ANT stick needed) and the hardware tests against SimulANT+ simulated sensors
(`AntWorkoutQt\hwtests`) are described in `AntWorkoutQt\README.md`.

## Saved files

These are written next to the app, in the same form as the Python app's, so either app can use the other's:

- `savedSettingsANT.json`: settings (FTP, max heart rate, wheel size, power calibration, theme, etc.)
- `musicCadenceCacheANT.json`: cached song tempo analysis
- `musicSelectionANT.json`: last music selection
- `workoutPickerANT.json`: the workout picker's list
- `antWorkoutLog.txt`: event and error log

## The repository

| Folder           | What it is                                                                          |
|------------------|-------------------------------------------------------------------------------------|
| `AntWorkoutQt\`  | The app (C++/Qt 6.7), LayoutViewer and the tools, their tests and the hardware tests  |
| `AntNative\`     | The ANT stick library (C++), used by both apps                                      |
| `AntWorkout\`    | The Python app, kept as the reference (its fonts, sounds, themes and workouts are the ones both apps ship) |
