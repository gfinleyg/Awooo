# AntWorkoutQt

The AntWorkout app in C++/Qt: the main app (see `..\README.md` for what it does). `..\AntWorkout` is the Python app
it was rewritten from, which stays as the reference. `PLAN.md` is the plan the rewrite followed, `CHANGES.md` what
the C++ app does differently.

## Layout

```
src/core/      awo_core: AppState, workout files (.awo, .zwo), FTP tests, grading, sessions, the zone colors' math,
               settings, the log, the workout engine, the simulated rider (no Qt Widgets)
src/ant/       awo_ant: the ANT connection on AntNative's core (AntBridge), the sticks, the readings pipeline
src/music/     awo_music: tempo analysis (numpy's sums, pocketfft), the analysis cache, playlists, the cadence
               workout, the music player (miniaudio)
src/plot/      awo_plot: the graphs (ViewBox, axes, curves, columns), what the app used pyqtgraph for
src/ui/        awo_ui: the theme and layout scaling, key shortcuts, sounds, the main window; screens/ (setup,
               workout, synopsis), dialogs/ (devices, calibration, pickers, theme picker), widgets/ (gauges,
               spiral, grades, progress bars, the FTP colors editor, the color picker)
src/viewer/    awo_viewer: LayoutViewer's display and controls windows
src/app/       AntWorkout.exe's main; src/tools/ the other exes' (awo-verifier, zwo2awo, ColorPicker,
               FtpColorsEditor); LayoutViewer's main is in src/viewer/
third_party/   pocketfft (as numpy 1.26.4 builds it) and miniaudio 0.11.25
tests/         the Qt Test exes (tst_*.cpp), golden/ (the Python app's results, make_golden.py), parity/ (the
               pixel comparison with the Python app)
hwtests/       the SimulANT+ hardware tests (simulant_test.py, app_test.py and their exes) and ant_console
```

The fonts, sounds, images, themes and workouts aren't copied into this folder: the build takes them from
`..\AntWorkout`.

## Building

Needs Visual Studio 2022 with the C++ workload, Qt 6.7.0 for MSVC (`C:\Qt\6.7.0\msvc2019_64`, with Qt's own CMake
and Ninja in `C:\Qt\Tools`) and the ANT SDK at `..\..\antlib\ANT_LIB`, as for AntNative. From any PowerShell:

```
.\build_windows.ps1                        Release: build\Release, tests, then dist\AntWorkout\
.\build_windows.ps1 -Configuration Debug   build\Debug, dist\AntWorkout-Debug\
.\build_windows.ps1 -SkipTests
```

`dist\AntWorkout\` runs on its own: the exes, `antnative.dll`, Qt, the C++ runtime, and the fonts, sounds, images,
themes and workouts (copied from `..\AntWorkout`). Rebuilding never deletes anything there, and files in it that
are newer than the build's (settings, an edited theme or workout) are kept.

The C++ runtime has to be at least as new as the compiler that built the app (14.40 for VS 2022 17.10): AntNative's
`std::mutex` crashes with an older one, which is what PyQt6's bundled runtime did to the Python app.
`tests\tst_ant_sticks.cpp` checks the runtime that actually got loaded.

## Tools

Built beside the app (`build\Release\bin`, and `dist\AntWorkout\`):

| Tool               | What it does                                                                         |
|--------------------|--------------------------------------------------------------------------------------|
| `awo-verifier.exe` | Checks `.awo` files with the app's own reader, every bad line reported (files or folders, the current folder by default) |
| `zwo2awo.exe`      | Converts Zwift `.zwo` files to `.awo` files beside them (files or folders, the current folder by default) |
| `LayoutViewer.exe` | Every screen in every mode without an ANT stick, at any screen size, with its fonts and colors edited live and saved as a theme (`--directwrite`: DirectWrite fonts) |
| `FtpColorsEditor.exe` | LayoutViewer's FTP Colors tab and mockup on their own: the power zone and free ride colors, saved as a gradients file |
| `ColorPicker.exe`  | An RGBA color picker (`ColorPicker.exe r g b a` to start from a color) |
| `ant_console.exe`  | Prints the readings of the sensors it's told to wait for (a quick look at a sensor, no app needed) |

## Tests

`build_windows.ps1` runs them through ctest. To run them yourself, use `ctest --test-dir build\Release
--output-on-failure` (Qt's CMake: `C:\Qt\Tools\CMake_64\bin\ctest.exe`). They run headless (`QT_QPA_PLATFORM=offscreen`)
with Qt's bin folder put on PATH by ctest, each in its own empty working folder.

Most of them also compare against `tests\golden\*.json`: the Python app's own results for the same inputs (every
shipped workout, every theme's colors, grading scenarios...). After a deliberate change to the Python app's results,
write them again with `py -3.10 tests\golden\make_golden.py` (the Python that has AntWorkout's requirements and
requirements-test installed). Deliberate differences of the C++ app are listed in `CHANGES.md`.

### Widgets side by side with the Python app

The custom painted widgets (gauges, spirals, graphs...) are checked by eye and by pixel against the Python app's:
`tests\parity\render_python.py` renders the Python widgets, set up as the Python screens set them up and fed fixed
made-up readings, and `render_widgets.exe` (built with the tests, in `build\Release\bin`) renders the C++ ones from the
same setup. `tests\parity\compare.py` counts the differing pixels and writes each pair with its differences marked:

```
py -3.10 tests\parity\render_python.py %TEMP%\py
build\Release\bin\render_widgets.exe %TEMP%\cpp
py -3.10 tests\parity\compare.py %TEMP%\py %TEMP%\cpp %TEMP%\diff
```

Both run in `..\AntWorkout` (its `defaultTheme.json`) on the real Windows platform, not offscreen, so the fonts render
as they do on screen. Run `render_widgets.exe` with Qt's bin folder on PATH. The `viewer_*` scenes are LayoutViewer's
views, in five looks (three screen sizes, two colors and gradients themes, a fonts theme). Its workout and synopsis
views also show what runs on the clock, which doesn't match: the heart's beat, the grade and the max value bubbles
fading in and out on the workout screen, and on the synopsis the best sustained powers, which shift with the ride's
start (two hours before it's built) unless both sides are rendered within the same few seconds.

### Hardware tests with SimulANT+

`hwtests\simulant_test.py` is `..\AntWorkout\SimulantTests\run_simulant_test.py` with the C++ app riding: it
installs the same SimulANT+ scripts and follows the same timeline, but the ride is `simulant_ride.exe` (the app's real
ANT bridge, readings pipeline and workout screen in Free Mode), and the Python harness's own checks judge what it
recorded. Set up SimulANT+ as `..\AntWorkout\SimulantTests\README.md` says (its `simulant_automation` helpers work
here too), then:

```
py -3.10 hwtests\simulant_test.py trainer
py -3.10 hwtests\simulant_test.py sensors
```

Each run gets a folder in `..\AntWorkout\SimulantTests\run` (named `..._qt`) with `report.txt`, `app_samples.csv`,
the scripts' logs and the app's own log.

`hwtests\app_test.py` is `run_app_test.py` on the C++ app: `simulant_app.exe` drives the whole app the way a rider
would (main window, setup screen, workout screen, synopsis, exit) against the scripts' steady pairing values, with
the same steps and checks. Only the scenario's simulators may be on (a trainer heard beside a power meter makes the
power device a choice to make):

```
py -3.10 hwtests\app_test.py trainer      Workout Mode on the trainer (ERG), the synopsis, back to setup, exit
py -3.10 hwtests\app_test.py sensors      Musical Cadence Mode (the music silent), a short 20m FTP test and Apply
                                          FTP, Free Mode, exit
```

`--offscreen` runs the build's `simulant_app.exe` on Qt's offscreen platform, so no windows come up.

## Making a release

1. Set the version in `CMakeLists.txt` (`project(AntWorkoutQt VERSION X.Y.Z ...)`), build with the tests, and commit.
2. Bring `main` up to the commit, tag it `vX.Y.Z` (an annotated tag, as the earlier ones) and push both.
3. Zip `dist\AntWorkout\` as `AntWorkout-vX.Y.Z.zip` with the `AntWorkout` folder at its top, leaving out what runs
   left behind in it (`savedSettingsANT.json`, `antWorkoutLog.txt`, `workoutPickerANT.json`, the music cache and
   selection) and the hardware test exes (`simulant_ride.exe`, `simulant_app.exe`). Check the zip on its own: unzipped
   somewhere new, with no Qt on PATH, `AntWorkout.exe` should start.
4. Make a GitHub release for the tag with the zip attached, its notes in the earlier releases' form (the
   requirements, "decompress the zip file into your Documents folder", then what's changed; `CHANGES.md` lists what
   1.4.0 changed from the Python app).
